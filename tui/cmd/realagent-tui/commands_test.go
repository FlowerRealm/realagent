// /plugins 命令结果的渲染测试：Data 载荷 → info 消息、失败 → error 消息。
package main

import (
	"encoding/json"
	"strings"
	"testing"

	"realagent/tui/internal/client"
)

func TestRenderPlugins(t *testing.T) {
	data, _ := json.Marshal([]client.PluginInfo{
		{Name: "core-tools", Version: "1.0", Type: "native", Status: "loaded"},
		{Name: "session-manager", Version: "0.2", Type: "native", Status: "disabled"},
		{Name: "broken", Version: "0.1", Type: "native", Status: "failed", Error: "dlopen: not found"},
	})
	got := renderPlugins(data)
	for _, want := range []string{"core-tools", "v1.0", "[native]", "session-manager", "disabled", "broken", "dlopen: not found"} {
		if !strings.Contains(got, want) {
			t.Errorf("renderPlugins 应包含 %q，got %q", want, got)
		}
	}
}

func TestRenderPluginsEmpty(t *testing.T) {
	got := renderPlugins(json.RawMessage(`[]`))
	if !strings.Contains(got, "未发现") {
		t.Errorf("空列表应提示未发现，got %q", got)
	}
}

// /plugins 失败（ok:false + error）应渲染为 error 消息
func TestPluginsErrorRendered(t *testing.T) {
	m := testModel()
	m.streaming = &message{role: "assistant"}
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: false, Command: "plugins", Error: "plugin enable failed: x"}})
	m = nm.(model)
	if len(m.messages) != 1 || m.messages[0].role != "error" {
		t.Fatalf("plugins 失败应为 error 消息，got %+v", m.messages)
	}
	if !strings.Contains(m.messages[0].text, "plugin enable failed") {
		t.Errorf("error 应透传 core 错误，got %q", m.messages[0].text)
	}
}
