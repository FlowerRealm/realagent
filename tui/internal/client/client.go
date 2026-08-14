// Package client 封装 core 的 QUIC/HTTP3 客户端（PROTOCOL.md）
package client

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/quic-go/quic-go/http3"
)

// Client 是 core 的 QUIC/HTTP3 客户端
type Client struct {
	hc  *http.Client
	rt  *http3.Transport
	url string // 基地址 https://<addr>，路径按端点拼接
}

// Reply 是请求-响应端点的通用响应
type Reply struct {
	Status   string          `json:"status"`
	Reply    string          `json:"reply,omitempty"`
	Error    string          `json:"error,omitempty"`
	Ok       bool            `json:"ok,omitempty"`
	Command  string          `json:"command,omitempty"`
	Data     json.RawMessage `json:"data,omitempty"` // 斜杠命令结果载荷（/plugins 的 []PluginInfo JSON）
	Messages int             `json:"messages,omitempty"`
}

// PluginInfo 是一条插件记录（GET /plugins，status: loaded/disabled/failed + error）。
type PluginInfo struct {
	Name        string   `json:"name"`
	Version     string   `json:"version"`
	Type        string   `json:"type"` // 插件类型（core 的 type_name 映射，契约字段名）
	Description string   `json:"description"`
	Dir         string   `json:"dir"`
	Status      string   `json:"status"`
	Error       string   `json:"error"`
	Deps        []string `json:"deps"`
}

// Command 是一条可用的斜杠命令（GET /commands，core 是唯一真相源）
type Command struct {
	Name        string `json:"name"`        // 不带 '/'
	Description string `json:"description"` // 菜单展示用
}

// New 创建客户端。addr 形如 "127.0.0.1:12345"。
func New(addr string) *Client {
	rt := &http3.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	return &Client{
		hc:  &http.Client{Transport: rt, Timeout: 120 * time.Second},
		rt:  rt,
		url: fmt.Sprintf("https://%s", addr),
	}
}

// Send 提交用户消息。core 立即返回 {"status":"processing"}，完整回复经 /events 推送流送达。
func (c *Client) Send(message string) (Reply, error) {
	body, _ := json.Marshal(map[string]string{"message": message})
	resp, err := c.hc.Post(c.url+"/message", "application/json", bytes.NewReader(body))
	if err != nil {
		return Reply{}, fmt.Errorf("发送失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return Reply{}, fmt.Errorf("读取响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(data, &r); err != nil {
		return Reply{}, fmt.Errorf("解析响应失败: %s", string(data))
	}
	return r, nil
}

// Interrupt 请求中断当前 agent run（POST /interrupt）
func (c *Client) Interrupt() error {
	resp, err := c.hc.Post(c.url+"/interrupt", "application/json", bytes.NewReader([]byte("{}")))
	if err != nil {
		return fmt.Errorf("中断请求失败: %w", err)
	}
	defer resp.Body.Close()
	io.ReadAll(resp.Body)
	return nil
}

// RespondApproval 回传审批裁决（POST /approval-response，PROTOCOL.md）
func (c *Client) RespondApproval(id string, allow bool) error {
	body, _ := json.Marshal(map[string]any{"id": id, "allow": allow})
	resp, err := c.hc.Post(c.url+"/approval-response", "application/json", bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("回传审批失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("读取审批响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(data, &r); err != nil {
		return fmt.Errorf("解析审批响应失败: %s", string(data))
	}
	if r.Error != "" {
		return fmt.Errorf("审批回传被拒: %s", r.Error)
	}
	return nil
}

// FetchCommands 拉取可用斜杠命令列表（GET /commands）。失败返回错误（TUI 降级为无菜单）。
func (c *Client) FetchCommands() ([]Command, error) {
	resp, err := c.hc.Get(c.url + "/commands")
	if err != nil {
		return nil, fmt.Errorf("获取命令列表失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("读取命令列表失败: %w", err)
	}
	var cmds []Command
	if err := json.Unmarshal(data, &cmds); err != nil {
		return nil, fmt.Errorf("解析命令列表失败: %s", string(data))
	}
	return cmds, nil
}

// getJSON 发起 GET 并把响应体 JSON 解码到 v（传指针）。失败返回错误。
func (c *Client) getJSON(path string, v any) error {
	resp, err := c.hc.Get(c.url + path)
	if err != nil {
		return fmt.Errorf("请求失败: %w", err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("读取响应失败: %w", err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		return fmt.Errorf("解析响应失败: %s", string(data))
	}
	return nil
}

// postJSON 发起 POST（JSON 体），返回解析后的 Reply（ok/error 由调用方判断）。
func (c *Client) postJSON(path string, body any) (Reply, error) {
	data, _ := json.Marshal(body)
	resp, err := c.hc.Post(c.url+path, "application/json", bytes.NewReader(data))
	if err != nil {
		return Reply{}, fmt.Errorf("请求失败: %w", err)
	}
	defer resp.Body.Close()
	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return Reply{}, fmt.Errorf("读取响应失败: %w", err)
	}
	var r Reply
	if err := json.Unmarshal(respBody, &r); err != nil {
		return Reply{}, fmt.Errorf("解析响应失败: %s", string(respBody))
	}
	return r, nil
}

// FetchPlugins 拉取插件列表（GET /plugins，含 loaded/disabled/failed 全部状态）。
func (c *Client) FetchPlugins() ([]PluginInfo, error) {
	var list []PluginInfo
	if err := c.getJSON("/plugins", &list); err != nil {
		return nil, err
	}
	return list, nil
}

// ModelInfo 是一条模型记录（/model 命令的 data 载荷）。
// 无单价——计价全在插件侧，core 与 TUI 都看不到（ADR-0009）。
type ModelInfo struct {
	Name    string `json:"name"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
	Current bool   `json:"current"`
}

// Statusline 是状态栏数据（GET /statusline）：会话身份信息。
// OwnedBy/Context 来自 core 的模型注册表（插件声明），配了表外的模型时为空——
// 那不是错误，模型表是参考资料不是白名单（ADR-0009）。
type Statusline struct {
	Model   string `json:"model"`
	OwnedBy string `json:"owned_by"`
	Context int64  `json:"context"`
}

// FetchStatusline 拉取状态栏数据（GET /statusline）。失败返回错误（TUI 降级为隐藏该段）。
func (c *Client) FetchStatusline() (Statusline, error) {
	var s Statusline
	if err := c.getJSON("/statusline", &s); err != nil {
		return Statusline{}, err
	}
	return s, nil
}

// EnablePlugin 启用插件（POST /plugins/enable，core 从 known dir 重载）。
func (c *Client) EnablePlugin(name string) error {
	r, err := c.postJSON("/plugins/enable", map[string]string{"name": name})
	if err != nil {
		return err
	}
	if r.Error != "" {
		return fmt.Errorf("启用插件失败: %s", r.Error)
	}
	return nil
}

// DisablePlugin 禁用插件（POST /plugins/disable，core 卸载并持久化禁用清单）。
func (c *Client) DisablePlugin(name string) error {
	r, err := c.postJSON("/plugins/disable", map[string]string{"name": name})
	if err != nil {
		return err
	}
	if r.Error != "" {
		return fmt.Errorf("禁用插件失败: %s", r.Error)
	}
	return nil
}

// Close 关闭传输层
func (c *Client) Close() {
	if c.rt != nil {
		c.rt.Close()
	}
}

// Event 是推送流中的一条事件（PROTOCOL.md 帧）
type Event struct {
	Type    string // message_start / message_update / tool_execution_* / turn_end / permission_request ...
	Payload string // JSON 载荷
}

// SubscribeEvents 订阅 /events 推送流，把事件持续发送到 ch。
// 流断开或出错时关闭 ch 返回。阻塞调用（goroutine 中使用）。
func (c *Client) SubscribeEvents(ch chan<- Event) error {
	resp, err := c.hc.Get(c.url + "/events")
	if err != nil {
		close(ch)
		return fmt.Errorf("订阅事件流失败: %w", err)
	}
	defer resp.Body.Close()
	defer close(ch)

	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 65536), 65536)
	var evType string
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "event: ") {
			evType = strings.TrimPrefix(line, "event: ")
		} else if strings.HasPrefix(line, "data: ") {
			payload := strings.TrimPrefix(line, "data: ")
			ch <- Event{Type: evType, Payload: payload}
			evType = ""
		}
	}
	return sc.Err()
}
