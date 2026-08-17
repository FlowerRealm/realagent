// 工具实时输出（tool_output 帧）：拼行、归属角色、不留空尾行。
package main

import (
	"encoding/json"
	"reflect"
	"testing"

	"realagent/tui/internal/client"
)

func outputEvent(text string) client.Event {
	b, _ := json.Marshal(map[string]string{"call_id": "t1", "stream": "stdout", "text": text})
	return client.Event{Type: "tool_output", Payload: string(b)}
}

// 插件按行推，超长行会被切成几帧：客户端要把它们拼回用户看到的那一行。
func TestToolOutputJoinsSplitFrames(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "tool_execution_start", Payload: `{"name":"bash"}`},
		outputEvent("hello "),
		outputEvent("world\n"),
		outputEvent("second\n"),
	)
	// 末尾那条空的 output 是光标位置（流还开着），收工时由 closeLine 丢掉——见下一个用例
	want := []string{"tool:🔧 bash …", "output:hello world", "output:second", "output:"}
	if got := roleTexts(m); !reflect.DeepEqual(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}

// 末帧的 \n 只是光标换行，不是一条空输出行——收工时不该留在记录里。
func TestToolOutputNoTrailingBlankLine(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "tool_execution_start", Payload: `{"name":"bash"}`},
		outputEvent("done\n"),
		client.Event{Type: "tool_execution_end", Payload: `{"name":"bash","status":0}`},
	)
	want := []string{"tool:🔧 bash …", "output:done", "tool:   ✓ bash"}
	if got := roleTexts(m); !reflect.DeepEqual(got, want) {
		t.Errorf("行流 = %v, want %v", got, want)
	}
}
