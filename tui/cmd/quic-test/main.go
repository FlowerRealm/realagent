// 测试客户端：连接 core 的 QUIC/HTTP3 服务，POST /message
// 用法：go run ./cmd/quic-test
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net/http"
	"strings"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	// 忽略自签名证书
	roundTripper := &http3.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	defer roundTripper.Close()

	client := &http.Client{Transport: roundTripper}

	url := "https://127.0.0.1:12345/message"
	body := `{"message":"hello, list the files in /tmp"}`

	resp, err := client.Post(url, "application/json", strings.NewReader(body))
	if err != nil {
		fmt.Println("ERROR:", err)
		if urlErr, ok := err.(*http3.Error); ok {
			fmt.Printf("H3 code=%d\n", urlErr.ErrorCode)
		}
		if qerr, ok := err.(interface{ Unwrap() error }); ok {
			fmt.Println("caused by:", qerr.Unwrap())
		}
		return
	}
	defer resp.Body.Close()

	data, _ := io.ReadAll(resp.Body)
	fmt.Printf("HTTP/3 status=%d body=%s\n", resp.StatusCode, string(data))
}
