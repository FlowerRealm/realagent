// 审批链路端到端测试：订阅 /events + POST /message（触发危险工具）
// → 收 permission_request → RespondApproval → 验证 agent 按裁决继续
package main

import (
	"bufio"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/quic-go/quic-go/http3"

	"realagent/tui/internal/client"
)

func main() {
	rt := &http3.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	defer rt.Close()
	hc := &http.Client{Transport: rt}

	// 1) 订阅 /events（goroutine 收集事件）
	events := make(chan client.Event, 256)
	subErr := make(chan error, 1)
	go func() {
		subErr <- subscribeRaw(hc, events)
	}()
	time.Sleep(1 * time.Second)

	// 2) 发消息（触发 bash 危险工具）
	body := `{"message":"use bash to list files in /tmp"}`
	r, err := hc.Post("https://127.0.0.1:12345/message", "application/json",
		strings.NewReader(body))
	if err != nil {
		fmt.Println("POST 失败:", err)
		return
	}
	io.Copy(io.Discard, r.Body)
	r.Body.Close()

	// 3) 等待 permission_request → 回传 allow
	deadline := time.Now().Add(30 * time.Second)
	approved := false
	for time.Now().Before(deadline) {
		select {
		case ev, ok := <-events:
			if !ok {
				fmt.Println("事件流关闭")
				return
			}
			fmt.Printf("[event] %s %s\n", ev.Type, truncate(ev.Payload, 80))
			if ev.Type == "permission_request" && !approved {
				// 模拟用户按 y：回传裁决
				var d struct {
					ID string `json:"id"`
				}
				jsonUnmarshal(ev.Payload, &d)
				fmt.Printf(">>> 回传审批允许 id=%s\n", d.ID)
				resp, err := hc.Post("https://127.0.0.1:12345/approval-response",
					"application/json",
					strings.NewReader(fmt.Sprintf(`{"id":%q,"allow":true}`, d.ID)))
				if err != nil {
					fmt.Println("回传失败:", err)
				} else {
					io.Copy(io.Discard, resp.Body)
					resp.Body.Close()
					fmt.Println(">>> 回传成功")
					approved = true
				}
			}
			if ev.Type == "turn_end" {
				fmt.Println("=== turn 结束（审批链路通过）===")
				return
			}
		case <-time.After(2 * time.Second):
			// 超时无事件，继续等
		}
	}
	fmt.Println("=== 超时：未完成审批链路 ===")
}

func subscribeRaw(hc *http.Client, ch chan<- client.Event) error {
	resp, err := hc.Get("https://127.0.0.1:12345/events")
	if err != nil {
		close(ch)
		return err
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
			ch <- client.Event{Type: evType, Payload: strings.TrimPrefix(line, "data: ")}
			evType = ""
		}
	}
	return sc.Err()
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n] + "..."
	}
	return s
}

func jsonUnmarshal(s string, v any) {
	_ = json.Unmarshal([]byte(s), v)
}
