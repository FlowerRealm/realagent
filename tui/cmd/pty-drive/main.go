package main

import (
	"fmt"
	"os/exec"
	"time"

	"github.com/creack/pty"
)

func main() {
	cmd := exec.Command("/tmp/realagent-tui", "127.0.0.1:12345")
	f, err := pty.Start(cmd)
	if err != nil {
		fmt.Println("PTY 启动失败:", err)
		return
	}
	defer f.Close()

	// 持续读输出
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := f.Read(buf)
			if n > 0 {
				fmt.Print(cleanAnsi(string(buf[:n])))
			}
			if err != nil {
				return
			}
		}
	}()

	time.Sleep(5 * time.Second)
	fmt.Println("\n--- 发送消息 ---")
	time.Sleep(1 * time.Second)
	fmt.Fprint(f, "say hi in one word\r")
	time.Sleep(12 * time.Second)
	fmt.Println("\n--- 退出 ---")
	fmt.Fprint(f, "\x03")
	time.Sleep(1 * time.Second)
}

func cleanAnsi(s string) string {
	var out []byte
	in := []byte(s)
	for i := 0; i < len(in); i++ {
		if in[i] == 0x1b {
			if i+1 < len(in) && in[i+1] == '[' {
				i += 2
				for i < len(in) && !((in[i] >= 'A' && in[i] <= 'Z') || (in[i] >= 'a' && in[i] <= 'z')) {
					i++
				}
				continue
			}
		}
		out = append(out, in[i])
	}
	return string(out)
}
