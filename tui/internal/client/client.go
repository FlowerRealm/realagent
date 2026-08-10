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
	Status string `json:"status"`
	Reply  string `json:"reply,omitempty"`
	Error  string `json:"error,omitempty"`
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
