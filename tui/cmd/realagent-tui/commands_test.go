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
		{Name: "core-tools", Version: "1.0", Capabilities: []string{"tool"}, Status: "loaded"},
		{Name: "session-manager", Version: "0.2", Status: "disabled"},
		{Name: "broken", Version: "0.1", Capabilities: []string{"protocol", "models"}, Status: "failed", Error: "dlopen: not found"},
	})
	got := renderPlugins(data)
	for _, want := range []string{"core-tools", "v1.0", "[tool]", "session-manager", "disabled", "broken", "[protocol,models]", "dlopen: not found"} {
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

// /plugins 失败（ok:false + error）应渲染为 error 行
func TestPluginsErrorRendered(t *testing.T) {
	m := testModel()
	m.awaiting = true
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: false, Command: "plugins", Error: "plugin enable failed: x"}})
	m = nm.(model)
	if len(m.pend) != 1 || m.pend[0].role != "error" {
		t.Fatalf("plugins 失败应为 error 行，got %+v", m.pend)
	}
	if !strings.Contains(m.pend[0].text, "plugin enable failed") {
		t.Errorf("error 应透传 core 错误，got %q", m.pend[0].text)
	}
}

// 多插件列表拆成多行进行流（一行一个插件，各自独立折行）
func TestPluginsListSplitsLines(t *testing.T) {
	m := testModel()
	m.awaiting = true
	data, _ := json.Marshal([]client.PluginInfo{
		{Name: "a", Status: "loaded"},
		{Name: "b", Status: "loaded"},
	})
	nm, _ := m.Update(sendMsg{reply: client.Reply{Ok: true, Command: "plugins", Data: data}})
	m = nm.(model)
	if len(m.pend) != 2 {
		t.Fatalf("两个插件应占 2 行，got %v", pendTexts(m))
	}
}
