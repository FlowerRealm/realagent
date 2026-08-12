// realagent-tui — 终端客户端（M6 基本功能 + ADR-0005 审批对话框）
//
// Bubble Tea 界面：消息流 + 底部输入框（参考 claude code / codex，无状态栏）。
// 订阅 /events 推送流渲染流式打字效果；POST /message 提交用户消息（立即返回
// {"status":"processing"}，回复与审批事件均走推送流）。
//
// 渲染分两层（见 render.go）：已定型的行打进终端原生 scrollback（滚动/复制/
// 搜索全用终端自带能力），Bubble Tea 只重绘底部活动区（未定型行 + 审批框 +
// 斜杠菜单 + 读秒状态行 + 输入框）。
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/charmbracelet/bubbletea"
	"realagent/tui/internal/client"
)

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

// 斜杠菜单最多同时显示的条目数（超出按高亮项开窗）
const menuMaxRows = 8

type model struct {
	client   *client.Client
	pend     []line // 未提交进 scrollback 的行；m.open 时末行仍在增长
	open     bool   // 末行是否还在流式增长
	out      outbox // scrollback 提交队列（保序）
	eventsCh <-chan client.Event
	ed       editor // 输入行编辑器
	width    int
	height   int
	approval *pendingApproval // 非 nil = 审批模态（忽略除 y/n 外按键）
	commands []client.Command // 斜杠命令列表（GET /commands，启动时拉取）
	menuSel  int              // 斜杠菜单高亮项（menuMatches 索引）
	menuHid  bool             // esc 收起菜单（下次编辑输入即复原）
	awaiting bool             // 已 POST /message 但推送流还没吐出任何内容
	busy     activity         // 读秒状态行（status.go）：模型在干什么 + 已耗时
}

func initialModel(c *client.Client) model {
	m := model{client: c}
	m.emit("info", "连接 core (QUIC/HTTP3) — Enter 发送，Alt+Enter 换行，Esc 中断，Ctrl+C 退出")
	return m
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

// interruptMsg 携带 POST /interrupt 的结果
type interruptMsg struct {
	err error
}

func interruptCmd(c *client.Client) tea.Cmd {
	return func() tea.Msg {
		return interruptMsg{err: c.Interrupt()}
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

// Update 是唯一的状态入口：先跑业务，再把定型的行推进 scrollback。
// 提交收口在这一处——别处只管往 m.pend 追加，谁都不用操心打印时机。
func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	next, cmd := m.update(msg)
	m = next
	m.out.push(m.freeze())
	return m, tea.Batch(cmd, m.out.flush())
}

func (m model) update(msg tea.Msg) (model, tea.Cmd) {
	switch v := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = v.Width
		m.height = v.Height
		return m, nil

	case printedMsg:
		return m, m.out.done()

	case eventsReady:
		m.eventsCh = v.ch
		return m, waitEventCmd(m.eventsCh)

	case eventsDone:
		m.busy.stop()
		return m, subscribeCmd(m.client) // 断线重连

	case tickMsg:
		return m, m.busy.tick(v)

	case commandsMsg:
		if v.err == nil {
			m.commands = v.cmds
		}
		return m, nil

	case eventMsg:
		cmd := m.handleEvent(client.Event(v))
		return m, tea.Batch(cmd, waitEventCmd(m.eventsCh))

	case tea.KeyMsg:
		return m.handleKey(v)

	case sendMsg:
		// POST /message 立即返回 {"status":"processing"}（无 reply），不构成回复；
		// 兜底仅在 POST 自身失败或返回明确结果时触发（正常定稿由事件流完成）。
		if !m.awaiting {
			return m, nil
		}
		switch {
		case v.err != nil:
			m.emit("error", "发送失败: "+v.err.Error())
			m.awaiting = false
			m.busy.stop()
		case v.reply.Error != "":
			m.emit("error", v.reply.Error)
			m.awaiting = false
			m.busy.stop()
		case v.reply.Ok:
			// 斜杠命令结果（core 返回 {"ok":true,"command":...}），渲染为 info 行。
			// plugins 命令携带 data 载荷（[]PluginInfo），可直接展示。
			text := describeCommand(v.reply.Command, v.reply.Messages)
			if v.reply.Command == "plugins" {
				text = renderPlugins(v.reply.Data)
			}
			m.emit("info", text)
			m.awaiting = false
			m.busy.stop() // 命令不启动 agent turn，收到结果即收工
		case v.reply.Reply != "":
			m.emit("assistant", v.reply.Reply)
			m.awaiting = false
			m.busy.stop()
		}
		return m, nil

	case interruptMsg:
		if v.err != nil {
			m.emit("error", "中断请求失败: "+v.err.Error())
		}
		return m, nil

	case approvalResp:
		if v.err != nil {
			m.emit("error", "审批回传失败: "+v.err.Error())
		}
		return m, nil
	}
	return m, nil
}

// ==================== 按键 ====================

// handleKey 处理按键。审批模态下只响应 y/n（a/d）与 ctrl+c，其余忽略。
// 斜杠菜单（输入以 '/' 开头）时：↑/↓ 移动高亮、Tab 补全、Enter 执行、Esc 收起。
func (m model) handleKey(v tea.KeyMsg) (model, tea.Cmd) {
	if m.approval != nil {
		switch v.String() {
		case "ctrl+c":
			return m, tea.Quit
		case "y", "a":
			return m.decideApproval(true)
		case "n", "d", "esc":
			return m.decideApproval(false)
		}
		return m, nil
	}

	// 粘贴：整块原样插入（含换行），不当按键解释
	if v.Paste {
		m.ed.insert(string(v.Runes))
		m.menuHid = false
		return m, nil
	}

	switch v.String() {
	case "ctrl+c":
		return m, tea.Quit

	case "esc":
		switch {
		case len(m.menuMatches()) > 0:
			m.menuHid = true // 只收菜单，不清输入——用户打的字是他的，别替他扔了
		case m.busy.active:
			return m, interruptCmd(m.client)
		}
		return m, nil

	case "enter":
		if matches := m.menuMatches(); len(matches) > 0 {
			m.ed.set("/" + matches[m.menuIndex(matches)].Name) // 菜单执行 = 补全 + 发送
		}
		return m.submitInput()

	case "alt+enter", "ctrl+j":
		m.ed.insert("\n")

	case "tab", "up", "down":
		return m.menuNav(v.String())

	case "backspace":
		m.ed.backspace()
	case "delete":
		m.ed.del()
	case "left", "ctrl+b":
		m.ed.left()
	case "right", "ctrl+f":
		m.ed.right()
	case "home", "ctrl+a":
		m.ed.home()
	case "end", "ctrl+e":
		m.ed.end()
	case "ctrl+k":
		m.ed.killToEnd()
	case "ctrl+u":
		m.ed.killToStart()
	case "ctrl+w":
		m.ed.killWord()

	default:
		if v.Type == tea.KeyRunes && len(v.Runes) > 0 {
			m.ed.insert(string(v.Runes))
		} else if v.Type == tea.KeySpace {
			m.ed.insert(" ")
		}
	}
	m.menuHid = false // 任何编辑都让菜单复原
	return m, nil
}

// menuOpen 判断斜杠菜单是否激活（输入以 '/' 开头且未被 esc 收起）
func (m model) menuOpen() bool {
	return !m.menuHid && strings.HasPrefix(m.ed.value(), "/")
}

// menuMatches 返回当前输入前缀命中的命令（命令名不带 '/'，前缀匹配）。
// 输入含空格视为进入参数模式（v1 命令无参数），关闭菜单。
func (m model) menuMatches() []client.Command {
	if !m.menuOpen() {
		return nil
	}
	prefix := m.ed.value()[1:]
	if strings.ContainsAny(prefix, " \n") {
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
func (m model) menuNav(key string) (model, tea.Cmd) {
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
		m.menuSel = m.menuIndex(matches)
		m.ed.set("/" + matches[m.menuSel].Name) // 补全：输入框写为完整命令
	}
	return m, nil
}

// submitInput 发送当前输入（普通消息或斜杠命令），清空输入框
func (m model) submitInput() (model, tea.Cmd) {
	input := strings.TrimSpace(m.ed.value())
	if input == "" {
		return m, nil
	}
	m.ed.clear()
	m.menuSel = 0
	m.emit("user", input)
	m.awaiting = true
	// 读秒从按下 Enter 起算（不等 turn_start，网络往返也是等待）
	return m, tea.Batch(sendCmd(m.client, input), m.busy.begin("发送中", time.Now()))
}

// decideApproval 处理审批裁决：记录结果 → 退出审批模态 → 回传 core
func (m model) decideApproval(allow bool) (model, tea.Cmd) {
	p := m.approval
	if p == nil {
		return m, nil
	}
	verdict := "已拒绝"
	if allow {
		verdict = "已允许"
	}
	m.emit("info", fmt.Sprintf("🔐 %s工具 %s", verdict, p.tool))
	m.approval = nil
	return m, tea.Batch(approvalCmd(m.client, p.id, allow), m.busy.begin("执行工具", time.Now()))
}

// ==================== 事件 ====================

// handleEvent 处理推送流事件，返回需要执行的 tea.Cmd（读秒计时循环的启动）
func (m *model) handleEvent(ev client.Event) tea.Cmd {
	switch ev.Type {
	case "turn_start":
		return m.busy.begin("思考中", time.Now())

	case "message_start":
		// 无需处理：文本到达时 stream 自然开行

	case "message_update":
		m.awaiting = false
		m.stream("assistant", deltaOf(ev.Payload))
		return m.busy.begin("生成回复", time.Now())

	case "thinking_start":
		return m.busy.begin("思考中", time.Now())

	case "thinking_update":
		m.awaiting = false
		// 思考块头部只在真有内容时冒出来（空的 thinking_start 不留痕）
		if !m.open || m.pend[len(m.pend)-1].role != "thinking" {
			m.emit("info", "💭 思考过程")
		}
		m.stream("thinking", deltaOf(ev.Payload))
		return m.busy.begin("思考中", time.Now())

	case "thinking_stop":
		m.closeLine()

	case "usage":
		// core 给的是本次 run 累计的绝对值：覆盖写，TUI 不做任何算术
		jsonUnmarshal(ev.Payload, &m.busy.tokens)

	case "tool_execution_start":
		var d struct {
			Name string `json:"name"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.awaiting = false
		m.emit("tool", "🔧 "+d.Name+" …")
		return m.busy.begin(toolVerb(d.Name), time.Now())

	case "tool_execution_end":
		var d struct {
			Name   string `json:"name"`
			Status int    `json:"status"`
		}
		jsonUnmarshal(ev.Payload, &d)
		mark := "✓"
		if d.Status != 0 {
			mark = "✗"
		}
		m.emit("tool", "   "+mark+" "+d.Name)
		return m.busy.begin("思考中", time.Now()) // 工具完事，等下一轮 LLM

	case "permission_request":
		// ADR-0005：core 请求审批（agent 阻塞等待），进入审批模态
		var d struct {
			ID     string          `json:"id"`
			Tool   string          `json:"tool"`
			Params json.RawMessage `json:"params"`
		}
		jsonUnmarshal(ev.Payload, &d)
		m.approval = &pendingApproval{id: d.ID, tool: d.Tool, params: string(d.Params)}
		return m.busy.begin("等待你的审批", time.Now())

	case "interrupted":
		m.emit("info", "已中断")
		m.awaiting = false
		m.busy.stop()
		m.approval = nil

	case "turn_end":
		m.closeLine()
		// 带 tool_uses 的 turn_end 只是本轮结束（下一轮继续跑，读秒不断）；
		// stop_reason / error 才是真正收工。
		var d struct {
			ToolUses int `json:"tool_uses"`
		}
		jsonUnmarshal(ev.Payload, &d)
		if d.ToolUses == 0 {
			m.awaiting = false
			m.busy.stop()
		}

	case "message_end":
		m.closeLine()
	}
	return nil
}

// deltaOf 取事件载荷里的 delta 字段（message_update / thinking_update 同构）
func deltaOf(payload string) string {
	var d struct {
		Delta string `json:"delta"`
	}
	jsonUnmarshal(payload, &d)
	return d.Delta
}

func jsonUnmarshal(s string, v any) {
	_ = json.Unmarshal([]byte(s), v)
}

// ==================== 渲染 ====================

// View 只画活动区：未定型行 + 审批框 + 斜杠菜单 + 状态行 + 输入框。
// 已定型的行早已由 outbox 打进终端 scrollback，不在这里重绘。
func (m model) View() string {
	width := m.width
	if width <= 0 {
		width = 80 // 尺寸还没到，先按常规宽度画，下一帧就校准
	}

	var rows []string
	for _, l := range m.pend {
		rows = append(rows, render(l, width)...)
	}
	// 输出与下方交互区之间留一行空白。它只属于活动区，不进 scrollback——
	// 历史里塞空行等于把记录撑稀，看的时候反而费劲。
	rows = append(rows, "")
	if m.approval != nil {
		rows = append(rows, renderApproval(m.approval, width)...)
	}
	if matches := m.menuMatches(); len(matches) > 0 {
		rows = append(rows, renderMenu(matches, m.menuIndex(matches), width)...)
	}
	if status := m.busy.render(width); status != "" {
		rows = append(rows, status)
	}
	rows = append(rows, m.ed.view(width)...)

	// 兜底：活动区绝不能高过终端（否则 Bubble Tea 自己会截，光标算错就花屏）。
	// 正常情况下这里不会触发——定型的行每帧都在往 scrollback 走。
	if max := m.height - 1; max > 0 && len(rows) > max {
		rows = rows[len(rows)-max:]
	}
	return strings.Join(rows, "\n")
}

// renderMenu 渲染斜杠命令菜单（输入行上方）：▸ 高亮当前项，条目多时按高亮开窗
func renderMenu(cmds []client.Command, sel, width int) []string {
	lo := 0
	if len(cmds) > menuMaxRows {
		lo = sel - menuMaxRows/2
		if lo < 0 {
			lo = 0
		}
		if lo > len(cmds)-menuMaxRows {
			lo = len(cmds) - menuMaxRows
		}
	}
	hi := lo + menuMaxRows
	if hi > len(cmds) {
		hi = len(cmds)
	}

	var out []string
	for i := lo; i < hi; i++ {
		text := "/" + cmds[i].Name
		if cmds[i].Description != "" {
			text += "  " + cmds[i].Description
		}
		text = fit(text, width-2) // 菜单一条一行，宽了就截
		if i == sel {
			out = append(out, menuSelStyle.Render("▸ "+text))
		} else {
			out = append(out, menuStyle.Render("  "+text))
		}
	}
	return out
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

// renderPlugins 把 /plugins 结果（[]PluginInfo JSON）渲染为多行 info 文本。
// 每行：名称 v版本 [type] 状态，failed 附加 error。
func renderPlugins(data json.RawMessage) string {
	var list []client.PluginInfo
	if err := json.Unmarshal(data, &list); err != nil {
		return "✅ 命令已执行: /plugins" // 载荷解析失败降级为通用提示
	}
	if len(list) == 0 {
		return "✅ /plugins: 未发现插件"
	}
	var out []string
	for _, p := range list {
		text := p.Name
		if p.Version != "" {
			text += " v" + p.Version
		}
		if p.Type != "" {
			text += " [" + p.Type + "]"
		}
		text += "  " + p.Status
		if p.Status == "failed" && p.Error != "" {
			text += "  " + p.Error
		}
		out = append(out, text)
	}
	return strings.Join(out, "\n")
}

// renderApproval 渲染审批对话框（模态，等待 y/n 裁决）
func renderApproval(p *pendingApproval, width int) []string {
	desc := p.tool
	if p.params != "" {
		desc += " " + p.params
	}
	head := fit("🔐 权限请求: "+desc, width) // 按显示宽截，不切碎中文
	return []string{
		approvalStyle.Render(head),
		approvalStyle.Render("   [y] 允许    [n] 拒绝"),
	}
}

func main() {
	addr := "127.0.0.1:12345"
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}
	c := client.New(addr)
	defer c.Close()

	// 不用 altscreen：历史归终端管，滚动/选中/复制/搜索全走终端原生能力。
	// 也不开鼠标模式——那会抢走滚轮。
	p := tea.NewProgram(initialModel(c))
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "TUI 运行失败:", err)
		os.Exit(1)
	}
}
