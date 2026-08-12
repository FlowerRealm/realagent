// 读秒状态行（输入框上方）：模型在干什么 + 已耗时，参考 claude code。
//
// 数据结构决定一切：整个特性只有 activity 一个状态，active=false 即空闲，
// 渲染层与事件层都不需要额外的特殊判断分支。
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/bubbletea"
)

// 状态行刷新周期（spinner 转动帧率；读秒精度 1s 足够，120ms 只为动画顺滑）
const statusTickInterval = 120 * time.Millisecond

var spinnerFrames = []string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}

// activity 是"模型正在干活"的唯一状态源。
// start 跨 turn 连续（工具执行 → 下一轮 LLM 调用是同一次等待，读秒不重置），
// 直到 turn_end 收工或出错才停。
type activity struct {
	active bool
	verb   string    // 当前动作：思考中 / 生成回复 / 执行工具 bash / 等待审批
	start  time.Time // 本次等待的起点（读秒基准）
	now    time.Time // 最近一次 tick 时间（elapsed = now - start）
	seq    int       // tick 代号：只有 seq 相同的 tick 续命，杜绝并发计时循环
	frame  int       // spinner 帧序号
	tokens tokens    // 本次 run 的 token 累计（core 的 usage 帧，绝对值覆盖写）
}

// tokens 是 usage 帧的载荷（PROTOCOL.md：本次 run 累计、绝对值、模型上报的真实用量）。
// 全零 = core 没给数据（旧 core 或端点不报 usage），渲染时整段隐藏，绝不显示 0。
type tokens struct {
	In         int64 `json:"input"`
	Out        int64 `json:"output"`
	CacheRead  int64 `json:"cache_read"`
	CacheWrite int64 `json:"cache_write"`
}

func (t tokens) empty() bool {
	return t.In == 0 && t.Out == 0 && t.CacheRead == 0 && t.CacheWrite == 0
}

// text 渲染为 "↑12.3k ↓842"；缓存命中并入上行（花的是同一份输入预算）。
func (t tokens) text() string {
	if t.empty() {
		return ""
	}
	return "↑" + humanTokens(t.In+t.CacheRead+t.CacheWrite) + " ↓" + humanTokens(t.Out)
}

// humanTokens 折算量级：1000 以下原样，之后 k / M，保一位小数
func humanTokens(n int64) string {
	switch {
	case n < 1000:
		return fmt.Sprintf("%d", n)
	case n < 1000000:
		return strings.TrimSuffix(fmt.Sprintf("%.1f", float64(n)/1000), ".0") + "k"
	default:
		return strings.TrimSuffix(fmt.Sprintf("%.1f", float64(n)/1000000), ".0") + "M"
	}
}

// tickMsg 是状态行的定时刷新（seq 标识所属计时循环）
type tickMsg struct {
	seq int
	t   time.Time
}

func tickCmd(seq int) tea.Cmd {
	return tea.Tick(statusTickInterval, func(t time.Time) tea.Msg {
		return tickMsg{seq: seq, t: t}
	})
}

// begin 开始（或切换）一次等待。已在计时则只换措辞，不重置读秒、不再起计时循环。
func (a *activity) begin(verb string, now time.Time) tea.Cmd {
	a.verb = verb
	if a.active {
		return nil
	}
	a.active = true
	a.start = now
	a.now = now
	a.frame = 0
	a.tokens = tokens{} // 新一次 run，计数跟读秒同一个重置点
	a.seq++
	return tickCmd(a.seq)
}

// stop 结束等待。计时循环在下一个 tick 自然消亡（seq 或 active 不匹配即不续命）。
func (a *activity) stop() {
	a.active = false
}

// tick 推进动画并续命。过期 tick（旧 seq / 已停止）返回 nil，循环终止。
func (a *activity) tick(msg tickMsg) tea.Cmd {
	if !a.active || msg.seq != a.seq {
		return nil
	}
	a.now = msg.t
	a.frame++
	return tickCmd(a.seq)
}

func (a activity) elapsed() time.Duration {
	if a.now.Before(a.start) {
		return 0
	}
	return a.now.Sub(a.start)
}

// elapsedText 读秒文本：1 分钟内 "12s"，超过则 "1m05s"
func elapsedText(d time.Duration) string {
	secs := int(d.Seconds())
	if secs < 60 {
		return fmt.Sprintf("%ds", secs)
	}
	return fmt.Sprintf("%dm%02ds", secs/60, secs%60)
}

// render 渲染状态行；空闲返回 ""（调用方按空串跳过，无需判空逻辑）。
// 窄终端按"提示 → token → 括号全丢"逐级降级，保证 spinner 与动作词永远可见。
func (a activity) render(width int) string {
	if !a.active {
		return ""
	}
	spin := spinnerFrames[a.frame%len(spinnerFrames)]
	head := spin + " " + a.verb + "…"
	elapsed := elapsedText(a.elapsed())

	tails := []string{
		fmt.Sprintf(" (%s · %s · esc 中断)", elapsed, a.tokens.text()),
		fmt.Sprintf(" (%s · %s)", elapsed, a.tokens.text()),
		fmt.Sprintf(" (%s · esc 中断)", elapsed),
		fmt.Sprintf(" (%s)", elapsed),
		"",
	}
	if a.tokens.empty() {
		tails = tails[2:]
	}
	tail := tails[len(tails)-1]
	for _, t := range tails {
		if width <= 0 || len([]rune(head+t)) <= width {
			tail = t
			break
		}
	}
	return spinnerStyle.Render(spin) + " " + statusStyle.Render(a.verb+"…") + dimStyle.Render(tail)
}

// toolVerb 把工具名折成动作词（空名兜底为通用措辞）
func toolVerb(name string) string {
	if name == "" {
		return "执行工具"
	}
	return "执行工具 " + name
}
