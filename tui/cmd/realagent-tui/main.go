// realagent-tui — 终端客户端（M6）
//
// Bubble Tea 界面：消息列表 + 底部输入框，参考 claude code / codex 客户端外观（无状态栏）。
// 通过 QUIC/HTTP3 连接 core（PROTOCOL.md）。
package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"realagent/tui/internal/client"
)

// ==================== 界面样式 ====================

var (
	userStyle     = lipgloss.NewStyle().Foreground(lipgloss.Color("43")).Bold(true)
	assistantStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	errorStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	dimStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))
)

// ==================== 消息模型 ====================

type message struct {
	role string // "user" / "assistant" / "error" / "info"
	text string
}

// ==================== Bubble Tea 模型 ====================

type model struct {
	client   *client.Client
	messages []message
	input    string
	busy     bool // 等待 agent 回复
	width    int
	height   int
}

func initialModel(c *client.Client) model {
	return model{
		client:   c,
		messages: []message{{role: "info", text: "连接 core (QUIC/HTTP3) — 输入消息，Enter 发送，Ctrl+C 退出"}},
	}
}

// sendMsg 携带回复结果
type sendMsg struct {
	reply client.Reply
	err   error
}

// sendCmd 异步发送消息（tea.Cmd）
func sendCmd(c *client.Client, input string) tea.Cmd {
	return func() tea.Msg {
		r, err := c.Send(input)
		return sendMsg{reply: r, err: err}
	}
}

// ==================== Bubble Tea 接口 ====================

func (m model) Init() tea.Cmd { return nil }

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = v.Width
		m.height = v.Height
		return m, nil

	case tea.KeyMsg:
		if m.busy {
			return m, nil // 等待回复期间忽略输入
		}
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
			m.busy = true
			m.messages = append(m.messages, message{role: "info", text: "…"})
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
		m.busy = false
		// 移除 "…" 占位
		if len(m.messages) > 0 && m.messages[len(m.messages)-1].role == "info" &&
			m.messages[len(m.messages)-1].text == "…" {
			m.messages = m.messages[:len(m.messages)-1]
		}
		if v.err != nil {
			m.messages = append(m.messages, message{role: "error", text: v.err.Error()})
		} else if v.reply.Error != "" {
			m.messages = append(m.messages, message{role: "error", text: v.reply.Error})
		} else {
			m.messages = append(m.messages, message{role: "assistant", text: v.reply.Reply})
		}
		return m, nil
	}
	return m, nil
}

func (m model) View() string {
	// 消息列表（截取尾部适配高度）
	var b strings.Builder
	avail := m.height - 3 // 输入框区域
	if avail < 5 {
		avail = 5
	}
	var lines []string
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
		case "info":
			prefix = ""
		}
		for _, line := range strings.Split(msg.text, "\n") {
			lines = append(lines, style.Render(prefix+line))
		}
	}
	if len(lines) > avail {
		lines = lines[len(lines)-avail:]
	}
	b.WriteString(strings.Join(lines, "\n"))
	b.WriteString("\n")

	// 输入框（底部）
	if m.busy {
		b.WriteString(dimStyle.Render("…"))
	} else {
		b.WriteString(userStyle.Render("> ") + m.input)
	}
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
