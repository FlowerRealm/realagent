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
	menuStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuSelStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("229")).Background(lipgloss.Color("238")).Bold(true)
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

// commandsMsg 携带 GET /commands 的拉取结果（斜杠菜单数据源）
type commandsMsg struct {
	cmds []client.Command
	err  error
}

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

// 拉取斜杠命令列表（启动时一次；失败静默降级为无菜单，不影响其余功能）
func fetchCommandsCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		cmds, err := c.FetchCommands()
		return commandsMsg{cmds: cmds, err: err}
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
	commands  []client.Command // 斜杠命令列表（GET /commands，启动时拉取）
	menuSel   int              // 斜杠菜单高亮项（menuMatches 索引）
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
	return tea.Batch(subscribeCmd(m.client), fetchCommandsCmd(m.client))
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

	case commandsMsg:
		if v.err == nil {
			m.commands = v.cmds
		}
		return m, nil

	case eventMsg:
		ev := client.Event(v)
		m.handleEvent(ev)
		return m, waitEventCmd(m.eventsCh)

	case tea.KeyMsg:
		return m.handleKey(v)

	case sendMsg:
		// POST /message 立即返回 {"status":"processing"}（无 reply），不构成回复；
		// 兜底仅在 POST 自身失败或返回明确结果时触发（正常定稿由 turn_end/message_end 完成）。
		if m.streaming != nil && m.streaming.text == "" {
			switch {
			case v.err != nil:
				m.finalize("发送失败: "+v.err.Error(), "error")
			case v.reply.Error != "":
				m.finalize(v.reply.Error, "error")
			case v.reply.Ok:
				// 斜杠命令结果（core 返回 {"ok":true,"command":...}），渲染为 info 消息
				m.finalize(describeCommand(v.reply.Command, v.reply.Messages), "info")
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
// 斜杠菜单（输入以 '/' 开头）时：↑/↓ 移动高亮、Tab 补全、Enter 执行、Esc 关闭。
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
	case "esc":
		if m.menuOpen() {
			m.input = "" // 关闭斜杠菜单（菜单由输入派生，清空即关闭）
		}
		return m, nil
	case "tab", "up", "down":
		return m.menuNav(v.String())
	case "enter":
		if matches := m.menuMatches(); len(matches) > 0 {
			// 菜单打开：执行高亮项（补全 + 发送），不经过普通输入
			m.input = "/" + matches[m.menuIndex(matches)].Name
		}
		return m.submitInput()
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

// menuOpen 判断斜杠菜单是否激活（输入以 '/' 开头）
func (m model) menuOpen() bool {
	return len(m.input) > 0 && m.input[0] == '/'
}

// menuMatches 返回当前输入前缀命中的命令（命令名不带 '/'，前缀匹配）。
// 输入含空格视为进入参数模式（v1 命令无参数），关闭菜单。
func (m model) menuMatches() []client.Command {
	if !m.menuOpen() {
		return nil
	}
	prefix := m.input[1:]
	if strings.Contains(prefix, " ") {
		return nil
	}
	var out []client.Command
	for _, c := range m.commands {
		if strings.HasPrefix(c.Name, prefix) {
			out = append(out, c)
		}
	}
	return out
}

// menuIndex 返回高亮项在 matches 中的索引（越界归 0）
func (m model) menuIndex(matches []client.Command) int {
	if len(matches) == 0 || m.menuSel < 0 || m.menuSel >= len(matches) {
		return 0
	}
	return m.menuSel
}

// menuNav 处理菜单方向键：↑/↓ 移动高亮（循环），Tab 补全当前高亮项到输入框
func (m *model) menuNav(key string) (tea.Model, tea.Cmd) {
	matches := m.menuMatches()
	if len(matches) == 0 {
		return m, nil
	}
	switch key {
	case "up":
		m.menuSel = m.menuIndex(matches) - 1
		if m.menuSel < 0 {
			m.menuSel = len(matches) - 1
		}
	case "down":
		m.menuSel = m.menuIndex(matches) + 1
		if m.menuSel >= len(matches) {
			m.menuSel = 0
		}
	case "tab":
		m.input = "/" + matches[m.menuIndex(matches)].Name // 补全：输入框写为完整命令
		m.menuSel = m.menuIndex(matches)
	}
	return m, nil
}

// submitInput 发送当前输入（普通消息或斜杠命令），清空输入框
func (m *model) submitInput() (tea.Model, tea.Cmd) {
	input := strings.TrimSpace(m.input)
	if input == "" {
		return m, nil
	}
	m.input = ""
	m.messages = append(m.messages, message{role: "user", text: input})
	m.streaming = &message{role: "assistant"}
	return m, sendCmd(m.client, input)
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

// finalize 把 streaming 消息定稿进 messages（text 非空时同时覆盖角色）
func (m *model) finalize(text string, role string) {
	if m.streaming != nil {
		if text != "" {
			m.streaming.text = text
			m.streaming.role = role
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
	// 审批对话框占 2 行、斜杠菜单占 len(matches) 行，从消息区扣除
	approvalLines := 0
	if m.approval != nil {
		approvalLines = 2
	}
	menuMatches := m.menuMatches()
	menuLines := len(menuMatches)
	avail := m.height - 3 - approvalLines - menuLines
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
	if len(menuMatches) > 0 {
		b.WriteString(renderMenu(menuMatches, m.menuIndex(menuMatches), m.width))
	}
	b.WriteString(userStyle.Render("> ") + m.input)
	return b.String()
}

// renderMenu 渲染斜杠命令菜单（输入行上方）：▸ 高亮当前项，描述按宽度截断
func renderMenu(cmds []client.Command, sel, width int) string {
	var b strings.Builder
	for i, c := range cmds {
		line := "/" + c.Name
		if c.Description != "" {
			line += "  " + c.Description
		}
		if width > 4 && len(line) > width-2 {
			line = truncateRunes(line, width-2) + "…"
		}
		if i == sel {
			b.WriteString(menuSelStyle.Render("▸ " + line))
		} else {
			b.WriteString(menuStyle.Render("  " + line))
		}
		b.WriteString("\n")
	}
	return b.String()
}

// truncateRunes 按 rune 截断（避免在 UTF-8 多字节字符中间切断）
func truncateRunes(s string, n int) string {
	runes := []rune(s)
	if len(runes) <= n {
		return s
	}
	return string(runes[:n])
}

// describeCommand 把 core 的命令结果（ok:true + command）渲染为 info 消息
func describeCommand(name string, messages int) string {
	switch name {
	case "new":
		return "✅ 已新建会话（对话历史已清空）"
	case "resume":
		return fmt.Sprintf("📄 当前会话共 %d 条消息", messages)
	}
	return "✅ 命令已执行: /" + name
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
