// 行模型的单元测试：折行 / 挂起缩进 / 定型提交 / 提交保序。
package main

import (
	"strings"
	"testing"

	"github.com/charmbracelet/x/ansi"
)

// strip 去掉样式，只比内容（着色与否不该影响布局断言）
func strip(rows []string) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = ansi.Strip(r)
	}
	return out
}

func TestWrapTextWidth(t *testing.T) {
	rows := wrapText("", strings.Repeat("中", 20), 10)
	if len(rows) < 4 {
		t.Fatalf("20 个中文（宽 40）在 10 列应折成 ≥4 行，got %d: %v", len(rows), rows)
	}
	for _, r := range rows {
		if w := ansi.StringWidth(r); w > 10 {
			t.Errorf("折行后宽度 %d 超出 10：%q", w, r)
		}
	}
}

// 前缀只出现在首行，续行用等宽空格挂起缩进——续行也不能撑破宽度
func TestWrapTextHangingIndent(t *testing.T) {
	rows := wrapText("> ", strings.Repeat("ab ", 20), 20)
	if len(rows) < 2 {
		t.Fatalf("应折成多行，got %v", rows)
	}
	if !strings.HasPrefix(rows[0], "> ") {
		t.Errorf("首行应带前缀，got %q", rows[0])
	}
	for i, r := range rows[1:] {
		if !strings.HasPrefix(r, "  ") {
			t.Errorf("续行 %d 应挂起缩进，got %q", i, r)
		}
		if w := ansi.StringWidth(r); w > 20 {
			t.Errorf("续行 %d 宽度 %d 超出 20：%q", i, w, r)
		}
	}
}

// 极窄终端不能算出负宽度而 panic
func TestWrapTextTinyWidth(t *testing.T) {
	for _, w := range []int{0, 1, 2, 3} {
		if rows := wrapText("> ", "中文abc", w); len(rows) == 0 {
			t.Errorf("width=%d 不应产出空结果", w)
		}
	}
}

// 定型规则：收尾的行进 scrollback，开着的行留在活动区
func TestFreezeClosedOnly(t *testing.T) {
	m := model{width: 40}
	m.emit("user", "问题")
	m.stream("assistant", "答案还在写")
	rows := strip(m.freeze())
	if len(rows) != 1 || !strings.Contains(rows[0], "问题") {
		t.Fatalf("只有收尾的行该提交，got %v", rows)
	}
	if len(m.pend) != 1 || m.pend[0].text != "答案还在写" {
		t.Errorf("开着的行应留在活动区，got %+v", m.pend)
	}
	m.closeLine()
	if rows := strip(m.freeze()); len(rows) != 1 || !strings.Contains(rows[0], "答案还在写") {
		t.Errorf("收尾后应提交，got %v", rows)
	}
	if len(m.pend) != 0 {
		t.Errorf("提交后活动区应清空，got %+v", m.pend)
	}
}

// 还没拿到窗口尺寸时先攒着——按错的宽度折行等于把记录写坏
func TestFreezeWaitsForWidth(t *testing.T) {
	m := model{}
	m.emit("info", "开场白")
	if rows := m.freeze(); rows != nil {
		t.Errorf("无宽度不应提交，got %v", rows)
	}
	m.width = 40
	if rows := m.freeze(); len(rows) != 1 {
		t.Errorf("拿到宽度后应补交，got %v", rows)
	}
}

// 核心不变量：贪心折行前缀稳定 —— 逐字流式喂入，与一次性整段渲染的结果必须逐行相同。
// 这条保证了「开着的行也能中途提交折行」是安全的：不丢字、不重复、不错行。
func TestFreezeIncrementalEqualsWhole(t *testing.T) {
	text := "先说结论：这套渲染层把历史交还给终端管理，" +
		"so scrolling and copying are handled natively by the terminal itself, " +
		"而 Bubble Tea 只负责重绘底部那一小块活动区。"

	for _, width := range []int{20, 37, 80} {
		m := model{width: width}
		var got []string
		for _, r := range []rune(text) {
			m.stream("assistant", string(r))
			got = append(got, m.freeze()...)
		}
		m.closeLine()
		got = append(got, m.freeze()...)

		want := render(line{role: "assistant", text: text}, width)
		if g, w := strip(got), strip(want); !equal(g, w) {
			t.Errorf("width=%d 流式与整段渲染不一致：\n流式 %q\n整段 %q", width, g, w)
		}
	}
}

// 流式长段落时活动区只留最后一个折行，不会越长越高把屏幕撑爆
func TestFreezeBoundsLiveRegion(t *testing.T) {
	m := model{width: 20}
	for range 50 {
		m.stream("assistant", "这是一段很长的中文内容")
		m.freeze()
	}
	if len(m.pend) != 1 {
		t.Fatalf("活动区应只剩 1 行，got %d", len(m.pend))
	}
	if rows := layout(m.pend[0], 20); len(rows) != 1 {
		t.Errorf("活动区那行应只占 1 个折行，got %d", len(rows))
	}
}

// 带前缀的角色（user/error）不做折行级中途提交——前缀归属会错
func TestFreezeKeepsPrefixedLinesWhole(t *testing.T) {
	m := model{width: 10}
	m.stream("user", strings.Repeat("中", 30))
	if rows := m.freeze(); len(rows) != 0 {
		t.Errorf("带前缀的开着的行不应中途提交，got %v", strip(rows))
	}
}

// 提交必须严格保序：同一时刻只许一条 Println 在飞
func TestOutboxSerializes(t *testing.T) {
	var o outbox
	o.push([]string{"a", "b"})
	if cmd := o.flush(); cmd == nil {
		t.Fatal("有内容应发出提交")
	}
	o.push([]string{"c"})
	if cmd := o.flush(); cmd != nil {
		t.Error("上一批还在飞时不应再发")
	}
	if cmd := o.done(); cmd == nil {
		t.Error("收到回执后应接着发下一批")
	}
	if cmd := o.done(); cmd != nil {
		t.Error("队列空了不应再发")
	}
}

// 一批多行合成一条 Println（printLineMessage 自己按 \n 拆），批内天然保序
func TestOutboxBatchesRows(t *testing.T) {
	var o outbox
	o.push([]string{"一", "二", "三"})
	cmd := o.flush()
	if cmd == nil || cmd() == nil {
		t.Fatal("应发出一条提交命令")
	}
	if !o.flying {
		t.Error("发出后应标记在飞")
	}
	if len(o.rows) != 0 {
		t.Error("发出后队列应清空")
	}
}

// 活动区绝不能高过终端——否则 Bubble Tea 自己截断，光标算错就花屏
func TestViewFitsHeight(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 10
	for i := range 50 {
		m.emit("assistant", strings.Repeat("长", 30)+string(rune('a'+i%26)))
	}
	rows := strings.Split(m.View(), "\n")
	if len(rows) > m.height-1 {
		t.Errorf("活动区 %d 行超出终端高度 %d", len(rows), m.height)
	}
}

func TestRenderApprovalTruncates(t *testing.T) {
	p := &pendingApproval{tool: "bash", params: `{"cmd":"` + strings.Repeat("中", 100) + `"}`}
	rows := renderApproval(p, 40)
	if len(rows) != 2 {
		t.Fatalf("审批框应为 2 行，got %d", len(rows))
	}
	for _, r := range rows {
		if w := ansi.StringWidth(r); w > 40 {
			t.Errorf("审批框宽度 %d 超出 40：%q", w, ansi.Strip(r))
		}
	}
	if !strings.Contains(ansi.Strip(rows[0]), "中") {
		t.Error("按显示宽截断不应把中文切碎")
	}
}

// 输出与输入栏之间空一行（空行只在活动区，不进 scrollback）
func TestViewBlankLineBeforeInput(t *testing.T) {
	m := testModel()
	m.width, m.height = 40, 12
	m.emit("assistant", "回答")
	rows := strings.Split(m.View(), "\n")
	if len(rows) < 3 {
		t.Fatalf("应有 输出/空行/输入 三行，got %v", rows)
	}
	in := len(rows) - 1
	if ansi.Strip(rows[in-1]) != "" {
		t.Errorf("输入栏上方应是空行，got %q", ansi.Strip(rows[in-1]))
	}
	if !strings.Contains(ansi.Strip(rows[in-2]), "回答") {
		t.Errorf("空行上方应是模型输出，got %q", ansi.Strip(rows[in-2]))
	}
	// 空行是活动区的排版，不该被当成内容提交进 scrollback
	if got := strip(m.freeze()); len(got) != 1 || got[0] != "回答" {
		t.Errorf("提交进 scrollback 的只该是输出本身，got %v", got)
	}
}
