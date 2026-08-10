// realagent-tui — 终端客户端（M6 基本功能）
//
// Bubble Tea 界面：消息列表 + 底部输入框（参考 claude code / codex，无状态栏）。
// 订阅 /events 推送流渲染流式打字效果；POST /message 提交用户消息。
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"realagent/tui/internal/client"
)

// ==================== 样式 ====================

var (
	userStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("43")).Bold(true)
	assistantStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	toolStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("240")).Italic(true)
	errorStyle     = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	dimStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))
)

// ==================== 消息模型 ====================

type message struct {
	role string // "user" / "assistant" / "tool" / "error" / "info"
	text string
}

// ==================== 事件订阅（推送流 → tea.Msg） ====================

type eventsReady struct{ ch <-chan client.Event }
type eventMsg client.Event
type eventsDone struct{}

// 启动事件订阅：Init 返回此 cmd，goroutine 持续读推送流
func subscribeCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		ch := make(chan client.Event)
		go c.SubscribeEvents(ch)
		return eventsReady{ch: ch}
	}
}

// 等待下一条事件（Bubble Tea 循环）
func waitEventCmd(ch <-chan client.Event) tea.Cmd {
	return func() tea.Msg {
		ev, ok := <-ch
		if !ok {
			return eventsDone{}
		}
		return eventMsg(ev)
	}
}

// ==================== Bubble Tea 模型 ====================

type model struct {
	client    *client.Client
	messages  []message
	streaming *message // 当前正在生成的 assistant 消息（打字效果）
	eventsCh  <-chan client.Event
	input     string
	width     int
	height    int
}

func initialModel(c *client.Client) model {
	return model{
		client: c,
		messages: []message{
			{role: "info", text: "连接 core (QUIC/HTTP3) — 输入消息，Enter 发送，Ctrl+C 退出"},
		},
	}
}

// sendMsg 携带 POST /message 的兜底回复
type sendMsg struct {
	reply client.Reply
	err   error
}

func sendCmd(c *client.Client, input string) tea.Cmd {
	return func() tea.Msg {
		r, err := c.Send(input)
		return sendMsg{reply: r, err: err}
	}
}

// ==================== Bubble Tea 接口 ====================

func (m model) Init() tea.Cmd {
	return subscribeCmd(m.client)
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = v.Width
		m.height = v.Height
		return m, nil

	case eventsReady:
		m.eventsCh = v.ch
		return m, waitEventCmd(m.eventsCh)

	case eventsDone:
		return m, subscribeCmd(m.client) // 断线重连

	case eventMsg:
		ev := client.Event(v)
		m.handleEvent(ev)
		return m, waitEventCmd(m.eventsCh)

	case tea.KeyMsg:
		switch v.String() {
		case "ctrl+c":
			return m, tea.Quit
		case "enter":
			input := strings.TrimSpace(m.input)
			if input == "" {
				return m, nil
			}
			m.input = ""
			m.messages = append(m.messages, message{role: "user", text: input})
			m.streaming = &message{role: "assistant"}
			return m, sendCmd(m.client, input)
		case "backspace":
			if len(m.input) > 0 {
				m.input = m.input[:len(m.input)-1]
			}
		default:
			if len(v.Runes) > 0 && v.Type == tea.KeyRunes {
				m.input += string(v.Runes)
			}
		}
		return m, nil

	case sendMsg:
		// 事件流兜底：若未收到 turn_end 定稿，用 POST 回复
		if m.streaming != nil && m.streaming.text == "" {
			if v.err != nil {
				m.finalize(v.err.Error(), "error")
			} else if v.reply.Error != "" {
				m.finalize(v.reply.Error, "error")
			} else {
				m.finalize(v.reply.Reply, "assistant")
			}
		}
		return m, nil
	}
	return m, nil
}

// handleEvent 处理推送流事件
func (m *model) handleEvent(ev client.Event) {
	switch ev.Type {
	case "message_start":
		if m.streaming == nil {
			m.streaming = &message{role: "assistant"}
		}
	case "message_update":
		var d struct {
			Delta string `json:"delta"`
		}
		jsonUnmarshal(ev.Payload, &d)
		if m.streaming == nil {
			m.streaming = &message{role: "assistant"}
		}
		m.streaming.text += d.Delta
	case "tool_execution_start":
		var d struct {
			Name string `json:"name"`
		}
		jsonUnmarshal(ev.Payload, &d)
		if m.streaming == nil {
			m.streaming = &message{role: "assistant"}
		}
		m.streaming.text += "\n" + toolStyle.Render("🔧 " + d.Name + " …")
	case "tool_execution_end":
		var d struct {
			Name   string `json:"name"`
			Status int    `json:"status"`
		}
		jsonUnmarshal(ev.Payload, &d)
		if m.streaming != nil {
			mark := "✓"
			if d.Status != 0 {
				mark = "✗"
			}
			m.streaming.text += "\n" + toolStyle.Render("   " + mark + " " + d.Name)
		}
	case "turn_end":
		m.finalize("", "assistant") // 定稿当前 streaming
	case "message_end":
		if m.streaming != nil && m.streaming.text != "" {
			m.finalize("", "assistant")
		}
	}
}

// finalize 把 streaming 消息定稿进 messages（text 非空时覆盖）
func (m *model) finalize(text string, role string) {
	if m.streaming != nil {
		if text != "" {
			m.streaming.text = text
		}
		if m.streaming.text != "" || m.streaming.role == "error" {
			m.messages = append(m.messages, *m.streaming)
		}
		m.streaming = nil
	}
}

func jsonUnmarshal(s string, v any) {
	_ = json.Unmarshal([]byte(s), v)
}

// debugKey 临时调试：打印收到的按键
func debugKey(k tea.KeyMsg) {
	if k.Type != tea.KeyRunes {
		fmt.Fprintf(os.Stderr, "[key] %s\n", k.String())
	}
}

// ==================== 渲染 ====================

func (m model) View() string {
	avail := m.height - 3
	if avail < 5 {
		avail = 5
	}

	var lines []string
	// 定稿消息
	for _, msg := range m.messages {
		style := dimStyle
		prefix := ""
		switch msg.role {
		case "user":
			style = userStyle
			prefix = "> "
		case "assistant":
			style = assistantStyle
		case "error":
			style = errorStyle
			prefix = "! "
		case "tool":
			style = toolStyle
		case "info":
			prefix = ""
		}
		for _, line := range strings.Split(msg.text, "\n") {
			lines = append(lines, style.Render(prefix+line))
		}
	}
	// streaming 部分
	if m.streaming != nil && m.streaming.text != "" {
		for _, line := range strings.Split(m.streaming.text, "\n") {
			lines = append(lines, assistantStyle.Render(line))
		}
	}
	if len(lines) > avail {
		lines = lines[len(lines)-avail:]
	}

	var b strings.Builder
	b.WriteString(strings.Join(lines, "\n"))
	b.WriteString("\n")
	b.WriteString(userStyle.Render("> ") + m.input)
	return b.String()
}

func main() {
	addr := "127.0.0.1:12345"
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}
	c := client.New(addr)
	defer c.Close()

	p := tea.NewProgram(initialModel(c), tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "TUI 运行失败:", err)
		os.Exit(1)
	}
}
