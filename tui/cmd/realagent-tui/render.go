// 渲染层：行模型 + 折行 + scrollback 提交。
//
// 数据结构决定一切：整个界面只有一条 line 流（原始文本 + 角色），数据里永远
// 没有 ANSI——样式在渲染的最后一刻才套上，所以同一份数据能按任意宽度重排。
//
// 历史归终端管（claude code / codex 走法）：不进 altscreen，已定型的行用
// tea.Println 打进终端原生 scrollback，Bubble Tea 只重绘底部「活动区」。
// 滚动、选中、复制、搜索全是终端原生能力，退出后记录还在。
//
// 定型判据只有一条：**追加式文本的贪心折行是前缀稳定的**——往末尾加字不会
// 改变前面的断点。于是「除最后一个折行外，其余折行都已定型」，没有特殊情况。
package main

import (
	"strings"

	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// ==================== 样式 ====================

var (
	userStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("43")).Bold(true)
	assistantStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	toolStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("240")).Italic(true)
	thinkingStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("245")).Italic(true)
	errorStyle      = lipgloss.NewStyle().Foreground(lipgloss.Color("196"))
	dimStyle        = lipgloss.NewStyle().Foreground(lipgloss.Color("240"))
	approvalStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
	spinnerStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("205"))
	statusStyle     = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuStyle       = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	menuSelStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("229")).Background(lipgloss.Color("238")).Bold(true)
	panelTitleStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("205")).Bold(true)
	cursorStyle     = lipgloss.NewStyle().Reverse(true)
)

// ==================== 行模型 ====================

// line 是渲染的最小单位：一行原始文本 + 决定样式的角色。
type line struct {
	role string // user / assistant / thinking / tool / error / info
	text string
}

// styleOf 返回角色样式
func styleOf(role string) lipgloss.Style {
	switch role {
	case "user":
		return userStyle
	case "assistant":
		return assistantStyle
	case "error":
		return errorStyle
	case "tool":
		return toolStyle
	case "thinking":
		return thinkingStyle
	}
	return dimStyle // info / 未知
}

// prefixOf 返回角色前缀。前缀只出现在首折行，续行用等宽空格挂起缩进对齐。
// 注意：只有前缀为空的角色（assistant / thinking）才会流式增长，
// 这让「开着的行」在中途提交时不必操心前缀归属（见 model.freeze）。
func prefixOf(role string) string {
	switch role {
	case "user":
		return "> "
	case "error":
		return "! "
	}
	return ""
}

// 折行后正文的最小可用宽度：终端窄到离谱时也别把字劈成一列
const minWrapWidth = 4

// wrapText 把一段原文按宽度折成渲染行：首行带 prefix，续行挂起缩进对齐。
// prefix 允许自带 ANSI（宽度按显示宽算），正文不着色。
func wrapText(prefix, text string, width int) []string {
	pw := ansi.StringWidth(prefix)
	w := width - pw
	if w < minWrapWidth {
		w = minWrapWidth
	}
	indent := strings.Repeat(" ", pw)
	rows := strings.Split(ansi.Wrap(text, w, ""), "\n")
	for i := range rows {
		if i == 0 {
			rows[i] = prefix + rows[i]
		} else {
			rows[i] = indent + rows[i]
		}
	}
	return rows
}

// fit 把一行按显示宽截断（宽度兜底，窄终端下也不会算出负数）。
// 用于菜单/审批框这种「一条就一行、宁截不折」的场合。
func fit(s string, width int) string {
	if width < minWrapWidth {
		width = minWrapWidth
	}
	return ansi.Truncate(s, width, "…")
}

// layout 把一行折成未着色的渲染行
func layout(l line, width int) []string {
	return wrapText(prefixOf(l.role), l.text, width)
}

// render 把一行折成着色后的渲染行
func render(l line, width int) []string {
	st := styleOf(l.role)
	rows := layout(l, width)
	out := make([]string, 0, len(rows))
	for _, r := range rows {
		out = append(out, st.Render(r))
	}
	return out
}

// ==================== 行流的追加与定型 ====================

// emit 追加若干整行（自带 \n 则拆开）。整行天生是定型的。
func (m *model) emit(role, text string) {
	m.open = false
	for _, s := range strings.Split(text, "\n") {
		m.pend = append(m.pend, line{role: role, text: s})
	}
}

// stream 把流式增量续写到开着的行；遇 \n 起新行，角色变了先收尾再另起。
func (m *model) stream(role, delta string) {
	if !m.open || len(m.pend) == 0 || m.pend[len(m.pend)-1].role != role {
		m.pend = append(m.pend, line{role: role})
		m.open = true
	}
	parts := strings.Split(delta, "\n")
	m.pend[len(m.pend)-1].text += parts[0]
	for _, s := range parts[1:] {
		m.pend = append(m.pend, line{role: role, text: s})
	}
}

// closeLine 给开着的行收尾：它不再增长，下一次 freeze 即可进 scrollback。
func (m *model) closeLine() { m.open = false }

// freeze 取出所有已定型的渲染行（交给 outbox 打进 scrollback），
// 未定型的留在 m.pend 里继续由活动区重绘。
//
// 两级定型：整行级——除开着的末行外全部定型；折行级——开着的末行里，
// 除最后一个折行外也已定型（贪心折行前缀稳定，追加不会改断点）。
func (m *model) freeze() []string {
	if m.width <= 0 || len(m.pend) == 0 {
		return nil // 还没拿到窗口尺寸，先攒着，宽度对了再折行
	}
	n := len(m.pend)
	if m.open {
		n--
	}
	var rows []string
	for _, l := range m.pend[:n] {
		rows = append(rows, render(l, m.width)...)
	}
	m.pend = m.pend[n:]

	// 开着的末行：把已定型的折行也吐出去，活动区永远只剩最后一个折行，
	// 长段落流式输出时自然向上滚进 scrollback，不会撑爆活动区。
	if m.open && len(m.pend) == 1 && prefixOf(m.pend[0].role) == "" {
		if wrapped := layout(m.pend[0], m.width); len(wrapped) > 1 {
			st := styleOf(m.pend[0].role)
			for _, r := range wrapped[:len(wrapped)-1] {
				rows = append(rows, st.Render(r))
			}
			m.pend[0].text = wrapped[len(wrapped)-1]
		}
	}
	return rows
}

// ==================== scrollback 提交队列 ====================

// printedMsg 是一批 Println 落地后的回执（outbox 用它串行化下一批）
type printedMsg struct{}

// outbox 保证提交进 scrollback 的行严格保序。
//
// tea.Cmd 各跑各的 goroutine，多条 Println 同时在飞就会乱序——记录一旦错乱
// 无法挽回。于是立一条铁律：**同一时刻只许一条 Println 在飞**，其余排队。
type outbox struct {
	rows   []string
	flying bool
}

func (o *outbox) push(rows []string) { o.rows = append(o.rows, rows...) }

// flush 发出下一批（一次 Println 打完整批，批内保序），在飞则返回 nil 等回执。
func (o *outbox) flush() tea.Cmd {
	if o.flying || len(o.rows) == 0 {
		return nil
	}
	batch := strings.Join(o.rows, "\n")
	o.rows = nil
	o.flying = true
	return tea.Sequence(
		tea.Println(batch),
		func() tea.Msg { return printedMsg{} },
	)
}

// done 收回执，接着发下一批
func (o *outbox) done() tea.Cmd {
	o.flying = false
	return o.flush()
}
