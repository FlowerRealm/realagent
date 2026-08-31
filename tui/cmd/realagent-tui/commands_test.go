// 斜杠命令结果的渲染测试：Data 载荷 → info 消息、失败 → error 消息。
package main

import (
	"encoding/json"
	"strings"
	"testing"

	"realagent/tui/internal/client"
)

func TestRenderModels(t *testing.T) {
	data, _ := json.Marshal(client.ProviderData{Models: []client.ModelInfo{
		{Name: "deepseek-v4-flash", OwnedBy: "deepseek", Context: 1048576, Current: true},
		{Name: "deepseek-v4-pro", OwnedBy: "deepseek", Context: 1048576},
	}})
	got := renderModels(data)
	for _, want := range []string{"deepseek-v4-flash", "deepseek-v4-pro", "deepseek", "●"} {
		if !strings.Contains(got, want) {
			t.Errorf("renderModels 应包含 %q，got %q", want, got)
		}
	}
}

func TestRenderModelsBadPayload(t *testing.T) {
	got := renderModels(json.RawMessage(`{`))
	if got == "" {
		t.Error("载荷解析失败应降级为通用提示，而不是空串")
	}
}

// 命令带 command 名失败（ok:false + error）应渲染为 error 行，不去渲染空的 data
func TestNamedCommandErrorRendered(t *testing.T) {
	m := testModel()
	m.awaiting = true
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: false, Command: "model", Error: "unknown model: x"}})
	m = nm.(model)
	if len(m.lines) != 1 || m.lines[0].role != "error" {
		t.Fatalf("命令失败应为 error 行，got %+v", m.lines)
	}
	if !strings.Contains(m.lines[0].text, "unknown model") {
		t.Errorf("error 应透传 core 错误，got %q", m.lines[0].text)
	}
}

// 多条清单拆成多行进行流（一行一条，各自独立折行）
func TestModelListSplitsLines(t *testing.T) {
	m := testModel()
	m.awaiting = true
	data, _ := json.Marshal(client.ProviderData{Models: []client.ModelInfo{{Name: "a"}, {Name: "b"}}})
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "model", Data: data}})
	m = nm.(model)
	if len(m.lines) != 2 {
		t.Fatalf("两个模型应占 2 行，got %v", lineTexts(m))
	}
}
