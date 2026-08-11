// thinking 事件（DeepSeek v4 reasoning）的流式累积 / 定稿 / 渲染。
package main

import (
	"strings"
	"testing"

	"realagent/tui/internal/client"
)

func feedEvents(m *model, evs ...client.Event) {
	for _, ev := range evs {
		m.handleEvent(ev)
	}
}

func TestThinkingStreaming(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "thinking_start", Payload: `{"signature":"sig-1"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"先分析"}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"再动手"}`},
		client.Event{Type: "thinking_stop", Payload: `{}`},
		client.Event{Type: "message_update", Payload: `{"delta":"答案"}`},
	)
	if m.streaming == nil {
		t.Fatal("thinking 事件未创建 streaming 消息")
	}
	if m.streaming.thinking != "先分析再动手" {
		t.Errorf("thinking 累积 = %q, want 先分析再动手", m.streaming.thinking)
	}
	if m.streaming.text != "答案" {
		t.Errorf("text = %q, want 答案", m.streaming.text)
	}
}

func TestThinkingFinalize(t *testing.T) {
	m := testModel()
	feedEvents(&m,
		client.Event{Type: "thinking_start", Payload: `{}`},
		client.Event{Type: "thinking_update", Payload: `{"delta":"思考中"}`},
		client.Event{Type: "message_end", Payload: `{}`},
	)
	if len(m.messages) != 1 {
		t.Fatalf("定稿消息数 = %d, want 1（thinking-only 消息也应定稿）", len(m.messages))
	}
	if m.messages[0].thinking != "思考中" {
		t.Errorf("定稿 thinking = %q, want 思考中", m.messages[0].thinking)
	}
}

func TestRenderThinking(t *testing.T) {
	m := testModel()
	m.messages = []message{
		{role: "assistant", thinking: "先想一下", text: "这是回答"},
	}
	view := m.View()
	// 思考块：dim 头部 + 内容在正文之前
	thinkHead := strings.Index(view, "💭 思考过程")
	thinkBody := strings.Index(view, "先想一下")
	answer := strings.Index(view, "这是回答")
	if thinkHead < 0 || thinkBody < 0 {
		t.Errorf("思考块未渲染（head=%d body=%d）", thinkHead, thinkBody)
	}
	if thinkBody > answer {
		t.Errorf("思考内容应在正文之前（think=%d answer=%d）", thinkBody, answer)
	}

	// 无思考的消息不渲染头部
	m2 := testModel()
	m2.messages = []message{{role: "assistant", text: "无思考的回答"}}
	if strings.Contains(m2.View(), "💭") {
		t.Errorf("无思考的消息不应渲染思考头")
	}
}
