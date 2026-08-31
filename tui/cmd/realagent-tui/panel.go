// 子面板：斜杠命令的第二层选择（参考 codex cli 的 /model 弹窗）与配置表单。
//
// 数据结构决定一切：面板就是「一列项目 + 一个高亮下标」。项可以是可选项，
// 也可以是包含 editor 的输入格，或者包含选项轮换（choices）的单选项。
//
// 提交仍只有一处（发送、awaiting、面板续开都只写一遍），但载荷有两种形态：
// 文本命令（submit），或一份结构化 data（onSubmit）。表单走结构化载荷，
// 直接把拼接好的完整目标状态发给 core。
//
// 面板数据不新增端点：core 的命令回包本来就带 data 载荷，
// 原先拿它渲染文本，现在拿它渲染面板和表单。
package main

import (
	"encoding/json"
	"fmt"
	"strings"

	"realagent/tui/internal/client"
)

// 面板最多同时显示的条目数（超出按高亮项开窗，同斜杠菜单）
const panelMaxRows = 10

// panelItem 是面板里的一项
type panelItem struct {
	label  string // 展示文本（不含标记与光标）
	mark   bool   // 当前生效项（● 标记，打开时高亮落在这里）
	submit string // 确认时提交的整条输入，如 "/model gpt-5"

	// ADR-0023 §8 表单扩展
	ed      *editor  // 非 nil = 可编辑输入框
	choices []string // 非空 = 选项轮换（通过 ←→ 切换）
	choice  int      // 当前选中的 choices 下标

	// 嵌套交互与结构化提交
	onEnter  func() *panel              // Enter 时进入子面板（如二级菜单）
	onSubmit func() (string, any, bool) // Enter 时提交表单。返回 (command, data, autoOpenModel)
}

// panel 是打开着的子面板。approval 是模态，panel 也是：开着时按键只喂它。
type panel struct {
	title string
	items []panelItem
	sel   int
}

// makePanel 按命令名与结果载荷造面板；造不出（无数据/不认识的命令）返回 nil，
// 调用方退回原来的文本输出——面板是锦上添花，不是新的失败点。
func makePanel(command string, data json.RawMessage, agentID int) *panel {
	switch command {
	case "model":
		return modelPanel(data)
	case "resume":
		return sessionPanel(data, agentID)
	case "provider":
		return providerForm(data)
	}
	return nil
}

// withModels 拿一份完整的 provider，只换掉那三个模型字段，返回新的一份。
//
// 客户端发回去的必须是完整的一束（core 不合并），所以每一个模型操作都得把
// 端点、凭证原样带上。
func withModels(pd client.ProviderData, models []string, main, small string) client.ProviderData {
	out := pd.Provider
	out.Models = models
	out.Model = main
	out.SmallModel = small
	return client.ProviderData{Provider: out}
}

// modelPanel 把 provider 的模型清单做成选择面板：主档 / 小档在同一列里用标记区分
func modelPanel(data json.RawMessage) *panel {
	var pd client.ProviderData
	if err := json.Unmarshal(data, &pd); err != nil {
		return nil
	}

	names := []string{}
	var mainModel, smallModel string
	for _, mi := range pd.Models {
		names = append(names, mi.Name)
		if mi.Current {
			mainModel = mi.Name
		}
		if mi.Small {
			smallModel = mi.Name
		}
	}

	p := &panel{title: "选模型（主档 / 小档）"}
	for _, mi := range pd.Models {
		mi := mi
		text := mi.Name
		if mi.OwnedBy != "" {
			text += " [" + mi.OwnedBy + "]"
		}
		if c := humanContext(mi.Context); c != "" {
			text += "  " + c
		}

		// 两档标记占一列固定宽度，否则没标记的那些行会往左缩进去
		markText := "     "
		if mi.Current && mi.Small {
			markText = "主/小 "
		} else if mi.Current {
			markText = "主    "
		} else if mi.Small {
			markText = "小    "
		}

		p.items = append(p.items, panelItem{
			label: markText + text,
			mark:  mi.Current,
			onEnter: func() *panel {
				return modelSubMenu(pd, names, mainModel, smallModel, mi.Name)
			},
		})
	}

	newEd := &editor{}
	p.items = append(p.items, panelItem{
		label: "+ 新增模型  ",
		ed:    newEd,
		onSubmit: func() (string, any, bool) {
			name := strings.TrimSpace(newEd.value())
			if name == "" {
				return "", nil, false
			}
			return "/model", withModels(pd, append(append([]string{}, names...), name),
				mainModel, smallModel), false
		},
	})

	p.sel = p.markIndex()
	return p
}

func modelSubMenu(pd client.ProviderData, names []string, mainModel, smallModel, target string) *panel {
	p := &panel{title: "对 " + target + " 做什么"}

	p.items = append(p.items, panelItem{
		label: "设为主模型",
		onSubmit: func() (string, any, bool) {
			return "/model", withModels(pd, names, target, smallModel), false
		},
	})
	p.items = append(p.items, panelItem{
		label: "设为小模型",
		onSubmit: func() (string, any, bool) {
			return "/model", withModels(pd, names, mainModel, target), false
		},
	})
	p.items = append(p.items, panelItem{
		label: "从清单移除",
		onSubmit: func() (string, any, bool) {
			// 起手就是空切片而不是 nil：nil 编码出来是 null，清单删空时
			// core 那头拿到 null 会当成"这个键没提"，于是清单纹丝不动
			left := []string{}
			for _, m := range names {
				if m != target {
					left = append(left, m)
				}
			}
			// 被移除的那个如果正占着某一档，把那一档一起清掉——
			// 否则 provider 会指着一个清单里没有的模型名
			main, small := mainModel, smallModel
			if main == target {
				main = ""
			}
			if small == target {
				small = ""
			}
			return "/model", withModels(pd, left, main, small), false
		},
	})

	return p
}

// providerForm 是 /provider 的全部界面：一张表单，三格。
//
// 没有列表、没有二级菜单——盘上就一份 provider，没有"选哪一份"这件事。
// 模型清单归 /model，不进这张表单。
func providerForm(data json.RawMessage) *panel {
	var pd client.ProviderData
	if err := json.Unmarshal(data, &pd); err != nil {
		return nil
	}
	prov := pd.Provider

	// 字段名就用配置键名：屏幕上填的那几格与 settings.json 里那几个键一一对上，
	// 用户哪天要手改文件，不必再翻译一遍
	p := &panel{title: "配置模型后端"}

	// 可选值由 core 下发：它认哪几个，客户端就给哪几个。抄一份在这儿，
	// core 加一个协议的那天这里不会跟着变，而且没人会发现
	protos := pd.Protocols
	if len(protos) == 0 {
		protos = []string{prov.Protocol} // 没下发就只给当前这个：没得选，但也不会空到越界
	}
	protoIdx := 0
	for i, pr := range protos {
		if pr == prov.Protocol {
			protoIdx = i
		}
	}
	// 记住它排第几：保存时要回来读用户轮到了哪个值。写死一个下标的话，
	// 哪天在它上面插一格，读到的就是隔壁那一格，而且不会报错
	protoRow := len(p.items)
	p.items = append(p.items, panelItem{
		label:   "protocol    ",
		choices: protos,
		choice:  protoIdx,
	})

	edBaseURL := &editor{}
	edBaseURL.set(prov.BaseURL)
	p.items = append(p.items, panelItem{label: "base_url    ", ed: edBaseURL})

	edAPIKey := &editor{}
	edAPIKey.set(prov.APIKey)
	p.items = append(p.items, panelItem{label: "api_key     ", ed: edAPIKey})

	// 端点是新配的（原来一个都没有）：存完把 /model 顶上来，
	// 免得用户先撞一句"配置缺少必填键"再回头找模型配在哪
	fresh := prov.BaseURL == ""
	p.items = append(p.items, panelItem{
		label: "【保存】",
		onSubmit: func() (string, any, bool) {
			out := prov
			out.Protocol = protos[p.items[protoRow].choice]
			out.BaseURL = strings.TrimSpace(edBaseURL.value())
			out.APIKey = strings.TrimSpace(edAPIKey.value())
			if out.Models == nil {
				out.Models = []string{}
			}
			return "/provider", client.ProviderData{Provider: out}, fresh
		},
	})

	return p
}

// sessionPanel 把 /resume 的会话清单做成选择面板：Enter = 恢复那个会话。
// 清单已按最近写入倒序（core 侧排好），所以第一项就是"上一个会话"。
func sessionPanel(data json.RawMessage, agentID int) *panel {
	var list []client.SessionInfo
	if err := json.Unmarshal(data, &list); err != nil || len(list) == 0 {
		return nil
	}
	p := &panel{title: "恢复会话"}
	for _, s := range list {
		title := s.Title
		if title == "" {
			title = "（空会话）"
		}
		p.items = append(p.items, panelItem{
			label:  fmt.Sprintf("%s  %s  %d 条", s.ID, title, s.Messages),
			mark:   s.OpenedBy == agentID,
			submit: "/resume " + s.ID,
		})
	}
	p.sel = p.markIndex()
	return p
}

// agentPanel 把 agent 清单做成选择面板：Enter = 切过去看它（ADR-0020）。
func agentPanel(list []client.AgentInfo, cur int) *panel {
	if len(list) == 0 {
		return nil
	}
	p := &panel{title: "切到哪个 agent"}
	for _, a := range list {
		label := fmt.Sprintf("%d  %s  %s", a.ID, a.State, a.Workdir)
		if n := len(a.InEdges) + len(a.OutEdges); n > 0 {
			label += fmt.Sprintf("  (入 %d 出 %d)", len(a.InEdges), len(a.OutEdges))
		}
		p.items = append(p.items, panelItem{
			label:  label,
			mark:   a.ID == cur,
			submit: fmt.Sprintf("/agents %d", a.ID),
		})
	}
	p.sel = p.markIndex()
	return p
}

// markIndex 返回当前生效项的下标（没有则 0）：打开面板时光标就落在那儿
func (p *panel) markIndex() int {
	for i, it := range p.items {
		if it.mark {
			return i
		}
	}
	return 0
}

// panelWantOf 判断一条输入的结果该不该开面板。
func panelWantOf(input string, fromPanel bool) string {
	cmd, args := splitCommand(input)
	switch cmd {
	case "/model":
		if args == "" {
			return "model"
		}
	case "/resume":
		if args == "" {
			return "resume"
		}
	case "/provider":
		if args == "" {
			return "provider"
		}
	case "/statusline":
		if args == "" || fromPanel {
			return "statusline"
		}
	}
	return ""
}

// splitCommand 按首个空白把输入切成命令名与参数（参数已去掉首尾空白）
func splitCommand(input string) (string, string) {
	input = strings.TrimSpace(input)
	if i := strings.IndexAny(input, " \n\t"); i >= 0 {
		return input[:i], strings.TrimSpace(input[i+1:])
	}
	return input, ""
}

// window 按高亮项在 n 条里开一个最多 max 行的窗口，返回 [lo, hi)
func window(n, sel, max int) (int, int) {
	if n <= max {
		return 0, n
	}
	lo := sel - max/2
	if lo < 0 {
		lo = 0
	}
	if lo > n-max {
		lo = n - max
	}
	return lo, lo + max
}

// wrapIndex 把下标绕回 [0, n)（面板与菜单的上下键都循环）
func wrapIndex(i, n int) int {
	if n <= 0 {
		return 0
	}
	return ((i % n) + n) % n
}

// renderPanel 渲染子面板（输入行上方）：标题 + 可选项 + 键位提示
func renderPanel(p *panel, width int) []string {
	out := []string{panelTitleStyle.Render(fit(p.title, width))}
	lo, hi := window(len(p.items), p.sel, panelMaxRows)

	hasChoices := false
	hasEd := false
	for i := lo; i < hi; i++ {
		item := p.items[i]
		if len(item.choices) > 0 {
			hasChoices = true
		}
		if item.ed != nil {
			hasEd = true
		}

		mark := "  "
		if item.mark {
			mark = "● "
		}

		content := ""
		if item.ed != nil {
			content = item.ed.display()
		} else if len(item.choices) > 0 {
			content = item.choices[item.choice]
		}

		text := fit(mark+item.label+content, width-2)
		if i == p.sel {
			out = append(out, menuSelStyle.Render("▸ "+text))
		} else {
			out = append(out, menuStyle.Render("  "+text))
		}
	}

	hint := "↑/↓ 换项"
	if hasChoices || hasEd {
		hint += " · ←/→ 移光标/切选项"
	}
	hint += " · Enter 确认 · Esc 取消"
	out = append(out, dimStyle.Render("  "+hint))
	return out
}
