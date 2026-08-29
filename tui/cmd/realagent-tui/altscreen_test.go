// 端到端：跑真的 Bubble Tea 程序，验证已定型的行确实写进了终端输出流
// （即用户能用终端原生滚动翻回去的那一份），且不重复、不丢字。
//
// 单元测试只能证明行流对；这一层证明 Println 通路对。
package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/ansi"

	"realagent/tui/internal/client"
)

// safeBuf 是并发安全的输出缓冲（渲染在自己的 goroutine 里写）
type safeBuf struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *safeBuf) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *safeBuf) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

// headless 让模型脱离 core 跑起来（Init 不订阅推送流，事件由测试直接 Send）
type headless struct{ model }

func (h headless) Init() tea.Cmd { return nil }

func (h headless) Update(msg tea.Msg) (tea.Model, tea.Cmd) { return h.model.Update(msg) }

func (h headless) View() string { return h.model.View() }

func TestScrollbackReceivesFinalizedLines(t *testing.T) {
	var out safeBuf
	p := tea.NewProgram(headless{testModel()},
		tea.WithInput(nil),
		tea.WithOutput(&out),
		tea.WithoutSignalHandler(),
	)

	done := make(chan struct{})
	go func() {
		defer close(done)
		p.Run()
	}()

	p.Send(tea.WindowSizeMsg{Width: 40, Height: 12})
	// 一个完整回合：用户提问 → 流式回答（多段）→ 工具 → 收工
	p.Send(eventMsg(client.Event{Type: "turn_start", Payload: `{}`}))
	for _, delta := range []string{"第一段回答。\n", "第二段回答，", "它比较长需要折行折行折行。\n"} {
		payload, _ := json.Marshal(map[string]string{"delta": delta})
		p.Send(eventMsg(client.Event{Type: "message_update", Payload: string(payload)}))
	}
	p.Send(eventMsg(client.Event{Type: "tool_execution_start", Payload: `{"name":"bash"}`}))
	p.Send(eventMsg(client.Event{Type: "tool_execution_end", Payload: `{"name":"bash","status":0}`}))
	p.Send(eventMsg(client.Event{Type: "turn_end", Payload: `{"tool_uses":0}`}))

	time.Sleep(200 * time.Millisecond) // 让渲染器把帧刷出去
	p.Quit()
	<-done

	got := ansi.Strip(out.String())
	for _, want := range []string{"第一段回答。", "第二段回答", "🔧 bash", "✓ bash"} {
		if !strings.Contains(got, want) {
			t.Errorf("终端输出应含 %q\n实际输出:\n%s", want, got)
		}
	}
	// 定型的行只该出现一次——重复即说明活动区与 scrollback 各画了一遍
	if n := strings.Count(got, "第一段回答。"); n != 1 {
		t.Errorf("定型的行应只出现 1 次，got %d 次\n实际输出:\n%s", n, got)
	}
}
