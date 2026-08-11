// 斜杠命令菜单的单元测试：过滤 / 补全 / 按键状态机 / 命令结果渲染。
package main

import (
	"strings"
	"testing"

	"github.com/charmbracelet/bubbletea"

	"realagent/tui/internal/client"
)

var testCmds = []client.Command{
	{Name: "new", Description: "新建会话，清空当前对话"},
	{Name: "resume", Description: "查看当前会话消息数"},
}

func testModel() model {
	return model{commands: testCmds}
}

func key(k tea.KeyType) tea.KeyMsg { return tea.KeyMsg{Type: k} }

func keyRunes(s string) tea.KeyMsg {
	return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
}

func TestMenuOpen(t *testing.T) {
	cases := []struct {
		input string
		want  bool
	}{
		{"", false},
		{"hello", false},
		{"/", true},
		{"/ne", true},
		{"/new 1", true}, // menuOpen 只看前缀；含空格由 menuMatches 关闭
	}
	for _, c := range cases {
		m := testModel()
		m.input = c.input
		if got := m.menuOpen(); got != c.want {
			t.Errorf("menuOpen(%q) = %v, want %v", c.input, got, c.want)
		}
	}
}

func TestMenuMatches(t *testing.T) {
	cases := []struct {
		input string
		want  []string // 命中的命令名（nil 表示无）
	}{
		{"", nil},
		{"hello", nil},
		{"/", []string{"new", "resume"}},
		{"/n", []string{"new"}},
		{"/re", []string{"resume"}},
		{"/xyz", nil},
		{"/new x", nil}, // 参数模式关闭菜单
		{"/new ", nil},
	}
	for _, c := range cases {
		m := testModel()
		m.input = c.input
		got := m.menuMatches()
		if c.want == nil {
			if len(got) != 0 {
				t.Errorf("menuMatches(%q) = %v, want none", c.input, got)
			}
			continue
		}
		if len(got) != len(c.want) {
			t.Errorf("menuMatches(%q) = %v, want %v", c.input, names(got), c.want)
			continue
		}
		for i, w := range c.want {
			if got[i].Name != w {
				t.Errorf("menuMatches(%q)[%d] = %q, want %q", c.input, i, got[i].Name, w)
			}
		}
	}
}

func names(cmds []client.Command) []string {
	var out []string
	for _, c := range cmds {
		out = append(out, c.Name)
	}
	return out
}

func TestMenuMatchesEmptyList(t *testing.T) {
	m := model{} // 未拉到命令列表（core 未启动/失败）
	m.input = "/"
	if got := m.menuMatches(); len(got) != 0 {
		t.Errorf("空命令列表应无菜单，got %v", got)
	}
}

func TestMenuIndexClamp(t *testing.T) {
	m := testModel()
	m.menuSel = 5
	if got := m.menuIndex(testCmds); got != 0 {
		t.Errorf("越界 menuSel 应归 0，got %d", got)
	}
	m.menuSel = 1
	if got := m.menuIndex(testCmds); got != 1 {
		t.Errorf("有效 menuSel 应保留，got %d", got)
	}
	if got := m.menuIndex(nil); got != 0 {
		t.Errorf("空匹配应归 0，got %d", got)
	}
}

func TestEnterExecutesHighlighted(t *testing.T) {
	m := testModel()
	m.input = "/re" // 命中 resume，高亮它
	nm, cmd := m.handleKey(key(tea.KeyEnter))
	m = *nm.(*model)
	if cmd == nil {
		t.Fatal("enter 应返回发送命令")
	}
	if m.input != "" {
		t.Errorf("发送后输入应清空，got %q", m.input)
	}
	if len(m.messages) != 1 || m.messages[0].text != "/resume" {
		t.Errorf("应发送 /resume，got %+v", m.messages)
	}
}

func TestEnterNormalMessage(t *testing.T) {
	m := testModel()
	m.input = "hello world"
	nm, cmd := m.handleKey(key(tea.KeyEnter))
	m = *nm.(*model)
	if cmd == nil {
		t.Fatal("enter 应返回发送命令")
	}
	if len(m.messages) != 1 || m.messages[0].text != "hello world" {
		t.Errorf("普通输入应原样发送，got %+v", m.messages)
	}
}

func TestTabCompletes(t *testing.T) {
	m := testModel()
	m.input = "/"
	nm, _ := m.handleKey(key(tea.KeyTab))
	m = *nm.(*model)
	if m.input != "/new" {
		t.Errorf("Tab 应补全为 /new，got %q", m.input)
	}
}

func TestUpDownWrap(t *testing.T) {
	m := testModel()
	m.input = "/"
	// down: 0 → 1，再 down 回绕到 0
	nm, _ := m.handleKey(key(tea.KeyDown))
	m = *nm.(*model)
	if m.menuSel != 1 {
		t.Errorf("down 应移到 1，got %d", m.menuSel)
	}
	nm, _ = m.handleKey(key(tea.KeyDown))
	m = *nm.(*model)
	if m.menuSel != 0 {
		t.Errorf("down 应回绕到 0，got %d", m.menuSel)
	}
	// up 回绕：0 → 末项
	nm, _ = m.handleKey(key(tea.KeyUp))
	m = *nm.(*model)
	if m.menuSel != 1 {
		t.Errorf("up 应回绕到末项 1，got %d", m.menuSel)
	}
}

func TestEscClosesMenu(t *testing.T) {
	m := testModel()
	m.input = "/re"
	nm, _ := m.handleKey(key(tea.KeyEsc))
	m = *nm.(*model)
	if m.input != "" {
		t.Errorf("菜单态 esc 应清空输入，got %q", m.input)
	}

	// 非菜单态 esc 不破坏输入
	m2 := testModel()
	m2.input = "hello"
	nm, _ = m2.handleKey(key(tea.KeyEsc))
	m2 = *nm.(*model)
	if m2.input != "hello" {
		t.Errorf("非菜单态 esc 不应动输入，got %q", m2.input)
	}
}

func TestSpaceClosesMenu(t *testing.T) {
	m := testModel()
	m.input = "/ne"
	m.handleKey(keyRunes("w"))
	// 直接验证过滤逻辑：含空格即无菜单
	m.input = "/new "
	if got := m.menuMatches(); len(got) != 0 {
		t.Errorf("含空格应关闭菜单，got %v", got)
	}
}

func TestDescribeCommand(t *testing.T) {
	cases := []struct {
		name     string
		messages int
		wantSub  string
	}{
		{"new", 0, "新建会话"},
		{"resume", 5, "5 条消息"},
		{"unknown", 0, "/unknown"},
	}
	for _, c := range cases {
		got := describeCommand(c.name, c.messages)
		if !strings.Contains(got, c.wantSub) {
			t.Errorf("describeCommand(%q) = %q, want contains %q", c.name, got, c.wantSub)
		}
	}
}

// sendMsg 兜底：斜杠命令结果（ok:true）应渲染为 info 消息而非悬挂空 streaming
func TestCommandResultRendered(t *testing.T) {
	m := testModel()
	m.streaming = &message{role: "assistant"}
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "new"}})
	m = nm.(model)
	if len(m.messages) != 1 {
		t.Fatalf("命令结果应产生 1 条消息，got %d", len(m.messages))
	}
	msg := m.messages[0]
	if msg.role != "info" || !strings.Contains(msg.text, "新建会话") {
		t.Errorf("命令结果应为 info 渲染，got %+v", msg)
	}
	if m.streaming != nil {
		t.Error("命令结果定稿后 streaming 应清空")
	}
}

func TestCommandErrorRendered(t *testing.T) {
	m := testModel()
	m.streaming = &message{role: "assistant"}
	nm, _ := m.Update(sendMsg{reply: client.Reply{Error: "unknown command"}})
	m = nm.(model)
	if len(m.messages) != 1 || m.messages[0].role != "error" {
		t.Fatalf("命令错误应渲染为 error，got %+v", m.messages)
	}
}
