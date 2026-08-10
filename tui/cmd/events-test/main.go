// 测试推送流：订阅 /events，同时发 POST /message，打印收到的流式事件
package main

import (
	"bufio"
	"crypto/tls"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	rt := &http3.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	defer rt.Close()
	client := &http.Client{Transport: rt}

	// 1) 订阅 /events（长连接流式）
	resp, err := client.Get("https://127.0.0.1:12345/events")
	if err != nil {
		fmt.Println("订阅失败:", err)
		return
	}
	defer resp.Body.Close()
	fmt.Println("已订阅 /events, status =", resp.StatusCode)

	// 2) 发一条消息（另一个连接）
	go func() {
		time.Sleep(500 * time.Millisecond)
		r2, err := client.Post("https://127.0.0.1:12345/message", "application/json",
			strings.NewReader(`{"message":"say hello in one word"}`))
		if err != nil {
			fmt.Println("POST 失败:", err)
			return
		}
		defer r2.Body.Close()
		fmt.Println("POST 完成")
	}()

	// 3) 读事件流（前 20 秒）
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 65536), 65536)
	deadline := time.Now().Add(20 * time.Second)
	count := 0
	for time.Now().Before(deadline) && sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "event: ") || strings.HasPrefix(line, "data: ") {
			fmt.Printf("[%s] %s\n", time.Now().Format("15:04:05.000"), line)
			count++
		}
		if count >= 40 {
			break
		}
	}
	fmt.Println("=== 收到", count, "行事件 ===")
}
