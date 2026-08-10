/*
 * quic_server.hpp — QUIC/HTTP3 服务端（M5）
 *
 * 基于 ngtcp2 + nghttp3 + OpenSSL 的最小实现：
 *  - 接受 QUIC 连接
 *  - 解析 HTTP/3 请求（POST /message / POST /command 等）
 *  - 事件推送流（占位，M6 接入 agent 事件）
 *
 * 生命周期：单线程事件循环（poll-based），通过回调与 agent 线程交互。
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace realagent {

/* 服务端配置 */
struct QuicServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 12345;
    std::string cert_file = ".realagent/cert.pem";
    std::string key_file = ".realagent/key.pem";
    /* 最大并发连接数 */
    int max_connections = 16;
};

/* QUIC 服务端回调（agent 线程注册） */
struct QuicCallbacks {
    /* 收到用户消息 → 投递 agent 线程。返回 JSON 响应字符串。 */
    std::function<std::string(const std::string& body)> on_message;
    /* 收到审批裁决（POST /approval-response）→ 交给审批协调器 */
    std::function<void(const std::string& id, bool allow)> on_approval_response;
    /* 每轮事件循环调用（main 把事件队列 flush 到推送流，ADR-0002 线程模型） */
    std::function<void()> on_tick;
    /* 新推送流注册（M6 接入：agent 事件推送给客户端） */
    std::function<void(uint64_t stream_id)> on_push_stream;
};

/* 最小 QUIC/HTTP3 服务端。事件循环阻塞在 run() 中。 */
class QuicServer {
public:
    explicit QuicServer(const QuicServerConfig& cfg);
    ~QuicServer();

    QuicServer(const QuicServer&) = delete;
    QuicServer& operator=(const QuicServer&) = delete;

    /* 设置回调（agent 线程注册） */
    void set_callbacks(const QuicCallbacks& cbs) { cbs_ = cbs; }

    /* 推送事件到所有订阅了 /events 的客户端（agent 事件流，SSE 语义） */
    void push_event(const std::string& type, const std::string& payload);

    /* 运行事件循环（阻塞，信号退出） */
    void run();
    /* 请求停止 */
    void stop() { running_ = false; }

    bool is_running() const { return running_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    QuicServerConfig cfg_;
    QuicCallbacks cbs_;
    std::atomic<bool> running_{false};
};

} // namespace realagent