// 子面板的单元测试：数据构造 / 开窗 / 按键状态机 / 与命令结果的衔接。
package main

import (
	"encoding/json"
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"realagent/tui/internal/client"
)

// /provider 与 /model 回的是同一份载荷：provider 那部分的完整状态 + 补好元数据的清单
func modelsJSON() json.RawMessage {
	data, _ := json.Marshal(client.ProviderData{
		Provider: client.Provider{
			Protocol: "anthropic-messages",
			Models:   []string{"deepseek-chat", "deepseek-reasoner"},
			Model:    "deepseek-reasoner",
		},
		Models: []client.ModelInfo{
			{Name: "deepseek-chat", OwnedBy: "deepseek", Context: 131072},
			{Name: "deepseek-reasoner", OwnedBy: "deepseek", Context: 131072, Current: true},
		},
	})
	return data
}

func sessionsJSON() json.RawMessage {
	data, _ := json.Marshal([]client.SessionInfo{
		{ID: "s-002", Title: "改一个 bug", Messages: 12, OpenedBy: 1},
		{ID: "s-001", Title: "读代码", Messages: 4},
	})
	return data
}

func TestPanelWantOf(t *testing.T) {
	cases := []struct {
		input     string
		fromPanel bool
		want      string
	}{
		{"/model", false, "model"},
		{"/model deepseek-chat", false, ""}, // 带名 = 明确指令，选完即走
		{"/model deepseek-chat", true, ""},  // 面板里选中模型同样收工
		{"/resume", false, "resume"},
		{"/resume s-001", false, ""}, // 带 id = 明确指令，恢复完即走
		{"/statusline", false, "statusline"},
		{"/statusline icons nerd", false, ""},
		{"/statusline icons nerd", true, "statusline"},
		{"/new", false, ""},
		{"你好", false, ""},
	}
	for _, c := range cases {
		if got := panelWantOf(c.input, c.fromPanel); got != c.want {
			t.Errorf("panelWantOf(%q, %v) = %q, want %q", c.input, c.fromPanel, got, c.want)
		}
	}
}

func TestSplitCommand(t *testing.T) {
	cases := []struct{ in, cmd, args string }{
		{"/model", "/model", ""},
		{"/model  gpt ", "/model", "gpt"},
		{"/resume s-001", "/resume", "s-001"},
		{"", "", ""},
	}
	for _, c := range cases {
		cmd, args := splitCommand(c.in)
		if cmd != c.cmd || args != c.args {
			t.Errorf("splitCommand(%q) = (%q, %q), want (%q, %q)", c.in, cmd, args, c.cmd, c.args)
		}
	}
}

// 打开面板时高亮落在当前生效项上，确认项发的是完整命令
func TestModelPanel(t *testing.T) {
	p := modelPanel(modelsJSON())
	if p == nil {
		t.Fatal("modelPanel 返回 nil")
	}
	if len(p.items) != 3 {
		t.Fatalf("items = %d, want 3", len(p.items))
	}
	if p.sel != 1 {
		t.Errorf("sel = %d, want 1（current 项）", p.sel)
	}
	if p.items[1].onEnter == nil {
		t.Errorf("expected onEnter to be set")
	}
	if !strings.Contains(p.items[0].label, "128k") {
		t.Errorf("label 少了上下文窗口: %q", p.items[0].label)
	}
}

// 会话面板：高亮落在当前会话上，确认项发的是完整命令
func TestSessionPanel(t *testing.T) {
	p := sessionPanel(sessionsJSON(), 1)
	if p == nil {
		t.Fatal("sessionPanel 返回 nil")
	}
	if p.sel != 0 {
		t.Errorf("sel = %d, want 0（current 项）", p.sel)
	}
	if got := p.items[1].submit; got != "/resume s-001" {
		t.Errorf("submit = %q", got)
	}
	if !strings.Contains(p.items[0].label, "改一个 bug") {
		t.Errorf("label 少了会话标题: %q", p.items[0].label)
	}
}

// 无数据造不出面板：退回文本输出，不是新的失败点
func TestMakePanelEmpty(t *testing.T) {
	if p := makePanel("model", json.RawMessage(`{`), 1); p != nil {
		t.Error("坏载荷不该开面板")
	}
	if p := makePanel("resume", json.RawMessage(`{`), 1); p != nil {
		t.Error("坏载荷不该开面板")
	}
	if p := makePanel("new", nil, 1); p != nil {
		t.Error("无面板的命令不该开面板")
	}
}

func TestWindow(t *testing.T) {
	cases := []struct {
		n, sel, max, lo, hi int
	}{
		{3, 0, 8, 0, 3},     // 装得下就全给
		{20, 0, 8, 0, 8},    // 头部不越界
		{20, 10, 8, 6, 14},  // 高亮居中
		{20, 19, 8, 12, 20}, // 尾部不越界
	}
	for _, c := range cases {
		lo, hi := window(c.n, c.sel, c.max)
		if lo != c.lo || hi != c.hi {
			t.Errorf("window(%d,%d,%d) = (%d,%d), want (%d,%d)", c.n, c.sel, c.max, lo, hi, c.lo, c.hi)
		}
	}
}

func TestWrapIndex(t *testing.T) {
	cases := []struct{ i, n, want int }{{-1, 3, 2}, {3, 3, 0}, {1, 3, 1}, {0, 0, 0}}
	for _, c := range cases {
		if got := wrapIndex(c.i, c.n); got != c.want {
			t.Errorf("wrapIndex(%d,%d) = %d, want %d", c.i, c.n, got, c.want)
		}
	}
}

// ↑/↓ 循环，Esc 关面板
func TestPanelNav(t *testing.T) {
	m := testModel()
	m.panel = modelPanel(modelsJSON())
	m.panel.sel = len(m.panel.items) - 1 // set to last item
	m, _ = m.panelKey(key(tea.KeyDown))
	if m.panel.sel != 0 {
		t.Errorf("down 未从末项绕回首项: sel = %d", m.panel.sel)
	}
	m, _ = m.panelKey(key(tea.KeyUp))
	if m.panel.sel != len(m.panel.items)-1 {
		t.Errorf("up 未绕回末项: sel = %d", m.panel.sel)
	}
	m, _ = m.panelKey(key(tea.KeyEsc))
	if m.panel != nil || m.panelWant != "" {
		t.Error("esc 应关闭面板并清掉重开意图")
	}
}

// Enter 确认 = 把 submit 当成用户输入发出去（复用 submitInput，没有第二条路）
func TestPanelEnterSubmits(t *testing.T) {
	m := testModel()
	m.panel = sessionPanel(sessionsJSON(), 1)
	m.panel.sel = 1 // s-001
	m, cmd := m.panelKey(key(tea.KeyEnter))
	if m.panel != nil {
		t.Error("确认后面板应先关闭，等结果回来再开")
	}
	if cmd == nil {
		t.Error("确认应发出请求")
	}
	if got := lineTexts(m); len(got) == 0 || got[len(got)-1] != "/resume s-001" {
		t.Errorf("提交的输入 = %v", got)
	}
	if m.panelWant != "" {
		t.Errorf("panelWant = %q, want 空（恢复完即走）", m.panelWant)
	}
}

// 命令结果回来时按 panelWant 开面板，且不再往 scrollback 打那坨清单
func TestReplyOpensPanel(t *testing.T) {
	m := testModel()
	m.awaiting = true
	m.panelWant = "model"
	m, _ = m.update(sendMsg{reply: client.Reply{Ok: true, Command: "model", Data: modelsJSON()}})
	if m.panel == nil {
		t.Fatal("结果未开面板")
	}
	if len(lineTexts(m)) != 0 {
		t.Errorf("开了面板还打文本清单: %v", lineTexts(m))
	}
	if m.awaiting || m.busy.active {
		t.Error("命令结果到手就该收工")
	}
}

// 没打算开面板（如手打 /model <name>）时行为不变：照旧渲染文本
func TestReplyWithoutPanelWant(t *testing.T) {
	m := testModel()
	m.awaiting = true
	m, _ = m.update(sendMsg{reply: client.Reply{Ok: true, Command: "model", Data: modelsJSON()}})
	if m.panel != nil {
		t.Error("没要面板就别弹")
	}
	if len(lineTexts(m)) == 0 {
		t.Error("应渲染模型清单文本")
	}
}

// /statusline 无参开面板；面板里改完仍留在面板
func TestStatuslinePanel(t *testing.T) {
	m := testModel()
	m.ed.set("/statusline")
	m, _ = m.submitInput(false)
	if m.panel == nil {
		t.Fatal("/statusline 未开面板")
	}
	sel := len(m.panel.items) - 1 // icons nerd
	m.panel.sel = sel
	m, _ = m.panelKey(key(tea.KeyEnter))
	if m.sl.iconSet != "nerd" {
		t.Errorf("iconSet = %q, want nerd", m.sl.iconSet)
	}
	if m.panel == nil {
		t.Fatal("面板里改完应留在面板")
	}
	if !m.panel.items[sel].mark {
		t.Error("重开的面板应标记新生效项")
	}
}

// 面板是模态：普通按键不进输入框
func TestPanelIsModal(t *testing.T) {
	m := testModel()
	m.panel = modelPanel(modelsJSON())
	m, _ = m.handleKey(keyRunes("x"))
	if !m.ed.empty() {
		t.Errorf("面板开着时按键不该写进输入框: %q", m.ed.value())
	}
	if _, cmd := m.handleKey(key(tea.KeyCtrlC)); cmd == nil {
		t.Error("ctrl+c 仍应能退出")
	}
}

// 表单键位：打字进当前格，↑↓ 换项、←→ 移光标（在选项格上换的是选中的值）。
// 这四件事分不清就得引入「选择态 / 编辑态」两个模式，而 ADR-0023 §8 明确不要模式。
func TestPanelFormNavigation(t *testing.T) {
	m := testModel()
	data, _ := json.Marshal(client.ProviderData{
		Provider:  client.Provider{Protocol: "anthropic-messages", BaseURL: "https://x"},
		Protocols: []string{"anthropic-messages", "openai-chat", "openai-responses"},
	})
	m.panel = providerForm(data) // 高亮落在第一格 protocol

	if len(m.panel.items[0].choices) == 0 {
		t.Fatalf("第一项应为选择项")
	}
	initialChoice := m.panel.items[0].choice
	m, _ = m.panelKey(key(tea.KeyRight))
	if m.panel.items[0].choice == initialChoice {
		t.Errorf("right 键未能切换选项")
	}
	if m.panel.sel != 0 {
		t.Errorf("right 键在选项格上不应换项")
	}

	m, _ = m.panelKey(key(tea.KeyDown))
	if m.panel.sel != 1 {
		t.Errorf("down 未能换项: sel=%d", m.panel.sel)
	}

	m, _ = m.panelKey(keyRunes("n"))
	m, _ = m.panelKey(keyRunes("e"))
	m, _ = m.panelKey(keyRunes("w"))

	val := m.panel.items[m.panel.sel].ed.value()
	if !strings.HasSuffix(val, "new") {
		t.Errorf("打字未能正确修改编辑格: got %q", val)
	}

	m.panel.items[m.panel.sel].ed.left()
	m, _ = m.panelKey(key(tea.KeyLeft))
	if m.panel.sel != 1 {
		t.Errorf("left 键不应换项: sel=%d", m.panel.sel)
	}
}

// 移除的那个正占着主档：清单里去掉它的同时得把主档清空，
// 否则 provider 会指着一个清单里没有的模型名。小档不受牵连。
func TestPanelRemoveMainModel(t *testing.T) {
	pd := client.ProviderData{
		Provider: client.Provider{Models: []string{"m1", "m2", "m3"}, Model: "m2", SmallModel: "m3"},
	}
	menu := modelSubMenu(pd, []string{"m1", "m2", "m3"}, "m2", "m3", "m2")
	cmd, data, _ := menu.items[2].onSubmit()
	if cmd != "/model" {
		t.Errorf("cmd = %q", cmd)
	}
	out, ok := data.(client.ProviderData)
	if !ok {
		t.Fatalf("载荷类型错误")
	}
	if len(out.Provider.Models) != 2 {
		t.Errorf("模型未移除: %v", out.Provider.Models)
	}
	if out.Provider.Model != "" {
		t.Errorf("移除主模型后未置空: got %q", out.Provider.Model)
	}
	if out.Provider.SmallModel != "m3" {
		t.Errorf("移除了主模型却波及了小模型: got %q", out.Provider.SmallModel)
	}
}

func TestPanelFormSave(t *testing.T) {
	data, _ := json.Marshal(client.ProviderData{
		Provider:  client.Provider{Protocol: "anthropic-messages", BaseURL: "https://x"},
		Protocols: []string{"anthropic-messages", "openai-chat", "openai-responses"},
	})
	p := providerForm(data)
	p.items[1].ed.set("https://y")                        // base_url 那一格
	cmd, outData, _ := p.items[len(p.items)-1].onSubmit() // 末项是【保存】
	if cmd != "/provider" {
		t.Errorf("cmd = %q", cmd)
	}
	newPd, ok := outData.(client.ProviderData)
	if !ok {
		t.Fatalf("载荷类型或长度错误")
	}
	if newPd.Provider.BaseURL != "https://y" {
		t.Errorf("编辑后的值未体现: %v", newPd.Provider)
	}
}

// 移除清单里最后一个模型同理：空清单发 []，不发 null
func TestPanelRemoveLastModelSendsEmptyArray(t *testing.T) {
	pd := client.ProviderData{
		Provider: client.Provider{Models: []string{"m1"}, Model: "m1"},
	}
	_, data, _ := modelSubMenu(pd, []string{"m1"}, "m1", "", "m1").items[2].onSubmit()
	body, _ := json.Marshal(data)
	if !strings.Contains(string(body), `"models":[]`) {
		t.Errorf("清单删空后应是空数组: %s", body)
	}
}

// 写完回到刷新后的表单：panelWant 要对上回包的 command，面板才就地重开。
// provider 是新配的（base_url 原本为空）时 panelWant 变成 model。
func TestPanelSaveReopensList(t *testing.T) {
	data1, _ := json.Marshal(client.ProviderData{
		Provider:  client.Provider{Protocol: "anthropic-messages", BaseURL: "https://x"},
		Protocols: []string{"anthropic-messages", "openai-chat", "openai-responses"},
	})
	m := testModel()
	m.panel = providerForm(data1)
	m.panel.sel = len(m.panel.items) - 1 // 【保存】
	m, cmd := m.panelKey(key(tea.KeyEnter))
	if cmd == nil {
		t.Fatal("确认应发出请求")
	}
	if m.panelWant != "provider" {
		t.Errorf("写完已有 provider 该回到 provider 面板: panelWant = %q", m.panelWant)
	}

	data2, _ := json.Marshal(client.ProviderData{
		Provider:  client.Provider{Protocol: "anthropic-messages", BaseURL: ""}, // 新配的
		Protocols: []string{"anthropic-messages", "openai-chat", "openai-responses"},
	})
	m2 := testModel()
	m2.panel = providerForm(data2)
	m2.panel.items[1].ed.set("https://y")
	m2.panel.sel = len(m2.panel.items) - 1 // 【保存】
	m2, cmd2 := m2.panelKey(key(tea.KeyEnter))
	if cmd2 == nil {
		t.Fatal("保存应发出请求")
	}
	if m2.panelWant != "model" {
		t.Errorf("新配 provider 保存完该把 /model 顶上来: panelWant = %q", m2.panelWant)
	}
}
