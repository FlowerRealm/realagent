// realagent-tui — 终端客户端（M6 基本功能 + ADR-0005 审批对话框）
//
// Bubble Tea 界面：消息列表 + 底部输入框（参考 claude code / codex，无状态栏）。
// 订阅 /events 推送流渲染流式打字效果；POST /message 提交用户消息（立即返回
// {"status":"processing"}，回复与审批事件均走推送流）。
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
	approvalStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
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

// pendingApproval 是挂起的审批请求（ADR-0005）
type pendingApproval struct {
	id     string // permission_request / approval-response 关联 ID
	tool   string
	params string // 工具参数（紧凑 JSON，仅展示）
}

type model struct {
	client    *client.Client
	messages  []message
	streaming *message // 当前正在生成的 assistant 消息（打字效果）
	eventsCh  <-chan client.Event
	input     string
	width     int
	height    int
	approval  *pendingApproval // 非 nil = 审批模态（忽略除 y/n 外按键）
}

func initialModel(c *client.Client) model {
	return model{
		client: c,
		messages: []message{
			{role: "info", text: "连接 core (QUIC/HTTP3) — 输入消息，Enter 发送，Ctrl+C 退出"},
		},
	}
}

// sendMsg 携带 POST /message 的兜底结果（正常回复走事件流）
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

// approvalResp 携带 POST /approval-response 的回传结果
type approvalResp struct {
	err error
}

func approvalCmd(c *client.Client, id string, allow bool) tea.Cmd {
	return func() tea.Msg {
		return approvalResp{err: c.RespondApproval(id, allow)}
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
		return m.handleKey(v)

	case sendMsg:
		// POST /message 立即返回 {"status":"processing"}（无 reply），不构成回复；
		// 兜底仅在 POST 自身失败或返回明确错误时触发（正常定稿由 turn_end/message_end 完成）。
		if m.streaming != nil && m.streaming.text == "" {
			switch {
			case v.err != nil:
				m.finalize("发送失败: "+v.err.Error(), "error")
			case v.reply.Error != "":
				m.finalize(v.reply.Error, "error")
			case v.reply.Reply != "":
				m.finalize(v.reply.Reply, "assistant")
			}
		}
		return m, nil

	case approvalResp:
		if v.err != nil {
			m.messages = append(m.messages, message{role: "error", text: "审批回传失败: " + v.err.Error()})
		}
		return m, nil
	}
	return m, nil
}

// handleKey 处理按键。审批模态下只响应 y/n（a/d）与 ctrl+c，其余忽略。
func (m *model) handleKey(v tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.approval != nil {
		switch v.String() {
		case "ctrl+c":
			return m, tea.Quit
		case "y", "a":
			return m.decideApproval(true)
		case "n", "d":
			return m.decideApproval(false)
		}
		return m, nil
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
}

// decideApproval 处理审批裁决：记录结果 → 退出审批模态 → 回传 core
func (m *model) decideApproval(allow bool) (tea.Model, tea.Cmd) {
	p := m.approval
	if p == nil {
		return m, nil
	}
	verdict := "已拒绝"
	if allow {
		verdict = "已允许"
	}
	m.messages = append(m.messages, message{role: "info", text: fmt.Sprintf("🔐 %s工具 %s", verdict, p.tool)})
	m.approval = nil
	return m, approvalCmd(m.client, p.id, allow)
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
		m.streaming.text += "\n" + toolStyle.Render("🔧 "+d.Name+" …")
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
			m.streaming.text += "\n" + toolStyle.Render("   "+mark+" "+d.Name)
		}
	case "permission_request":
		// ADR-0005：core 请求审批（agent 阻塞等待），进入审批模态
		var d struct {
			ID     string          `json:"id"`
			Tool   string          `json:"tool"`
			Params json.RawMessage `json:"params"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.approval = &pendingApproval{id: d.ID, tool: d.Tool, params: string(d.Params)}
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
	// 审批对话框占 2 行，从消息区扣除
	approvalLines := 0
	if m.approval != nil {
		approvalLines = 2
	}
	avail := m.height - 3 - approvalLines
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
	if m.approval != nil {
		b.WriteString(renderApproval(m.approval))
		b.WriteString("\n")
	}
	b.WriteString(userStyle.Render("> ") + m.input)
	return b.String()
}

// renderApproval 渲染审批对话框（模态，等待 y/n 裁决）
func renderApproval(p *pendingApproval) string {
	desc := p.tool
	if p.params != "" {
		params := p.params
		if len(params) > 60 {
			params = params[:60] + "…"
		}
		desc += " " + params
	}
	return approvalStyle.Render("🔐 权限请求: " + desc + "\n   [y] 允许    [n] 拒绝")
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
