/*
 * quic_server.cpp — QUIC/HTTP3 服务端实现（quiche）
 *
 * 基于 quiche C API 的 HTTP/3 服务端。事件驱动：
 *  UDP socket → quiche 引擎 → h3 事件（HEADERS/BODY/FINISHED）→ 响应
 *
 * 依赖：quiche（brew install cloudflare-quiche）
 */
#include "server/quic_server.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <map>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/udp.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <quiche.h>

namespace realagent {

struct QuicConn {
    quiche_conn* conn = nullptr;
    quiche_h3_conn* h3 = nullptr;
    sockaddr_storage peer{};
    socklen_t peer_len = 0;
    uint64_t scid = 0;
    std::string request_method;
    std::string request_path;
    std::string request_body;
    uint64_t request_stream_id = 0;
    bool expecting_body = false;
    /* 事件推送流（GET /events 订阅） */
    bool events_subscribed = false;
    int64_t events_stream = -1;
};

struct QuicServer::Impl {
    int fd = -1;
    quiche_config* config = nullptr;
    quiche_h3_config* h3_config = nullptr;
    std::map<uint64_t, QuicConn> conns;
    QuicServerConfig cfg;
    QuicCallbacks cbs;
    bool running = false;

    ~Impl() { cleanup(); }
    void cleanup();
};

QuicServer::QuicServer(const QuicServerConfig& cfg) : cfg_(cfg) {}
QuicServer::~QuicServer() = default;

/* 推送事件到所有订阅 /events 的客户端（SSE 语义，事件循环线程内调用） */
void QuicServer::push_event(const std::string& type, const std::string& payload) {
    if (!impl_ || !impl_->running) return;
    const std::string frame = "event: " + type + "\ndata: " + payload + "\n\n";
    for (auto& [_, c] : impl_->conns) {
        if (!c.h3 || !c.events_subscribed || c.events_stream < 0) continue;
        if (!c.conn || !quiche_conn_is_established(c.conn)) continue;
        quiche_h3_send_body(c.h3, c.conn, (uint64_t)c.events_stream,
                            (const uint8_t*)frame.data(), frame.size(), false);
        // 立即发送（agent 事件要流式到达，不等事件循环下次 tick）
        uint8_t out[65536];
        quiche_send_info si{};
        ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &si);
        if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr*)&c.peer, c.peer_len);
    }
}

/* ==================== 证书 ==================== */

static bool ensure_cert(const std::string& cert_file, const std::string& key_file) {
    if (access(cert_file.c_str(), R_OK) == 0 && access(key_file.c_str(), R_OK) == 0) return true;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 365 -nodes "
        "-subj '/CN=realagent' 2>/dev/null",
        key_file.c_str(), cert_file.c_str());
    return system(cmd) == 0;
}

/* ==================== 事件循环 ==================== */

void QuicServer::Impl::cleanup() {
    for (auto& [_, c] : conns) {
        if (c.h3) quiche_h3_conn_free(c.h3);
        if (c.conn) quiche_conn_free(c.conn);
    }
    conns.clear();
    if (h3_config) quiche_h3_config_free(h3_config);
    if (config) quiche_config_free(config);
    if (fd >= 0) close(fd);
    fd = -1;
}

void QuicServer::run() {
    impl_ = std::make_unique<Impl>();
    if (!impl_) return;
    impl_->cfg = cfg_;
    impl_->cbs = cbs_;
    running_ = true;
    impl_->running = true;

    // UDP socket
    impl_->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->fd < 0) { perror("socket"); running_ = false; return; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(impl_->fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); running_ = false; return;
    }

    // quiche 配置
    impl_->config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!impl_->config) { fprintf(stderr, "[server] quiche_config_new 失败\n"); running_ = false; return; }
    impl_->h3_config = quiche_h3_config_new();
    if (!impl_->h3_config) { fprintf(stderr, "[server] h3_config 创建失败\n"); running_ = false; return; }
    quiche_config_set_application_protos(impl_->config, (const uint8_t*)"\x02h3", 3);
    quiche_config_grease(impl_->config, false); // 兼容性：禁用 GREASE 流
    quiche_config_set_max_idle_timeout(impl_->config, 30000);
    quiche_config_set_initial_max_data(impl_->config, 1048576);
    quiche_config_set_initial_max_stream_data_bidi_local(impl_->config, 262144);
    quiche_config_set_initial_max_stream_data_bidi_remote(impl_->config, 262144);
    quiche_config_set_initial_max_streams_bidi(impl_->config, 100);
    quiche_config_set_initial_max_streams_uni(impl_->config, 100); // QPACK/控制流需要

    if (!ensure_cert(cfg_.cert_file, cfg_.key_file)) {
        fprintf(stderr, "[server] 证书生成失败\n"); running_ = false; return;
    }
    if (quiche_config_load_cert_chain_from_pem_file(impl_->config, cfg_.cert_file.c_str()) < 0 ||
        quiche_config_load_priv_key_from_pem_file(impl_->config, cfg_.key_file.c_str()) < 0) {
        fprintf(stderr, "[server] 证书加载失败\n"); running_ = false; return;
    }

    fprintf(stderr, "[server] QUIC/HTTP3（quiche）运行在 127.0.0.1:%d\n", cfg_.port);

    // 事件循环
    std::vector<pollfd> pfds(1);
    pfds[0].fd = impl_->fd;
    pfds[0].events = POLLIN;
    uint8_t buf[65536];

    while (running_ && impl_->running) {
        int rv = poll(pfds.data(), pfds.size(), 1000);
        if (rv < 0) { if (errno == EINTR) continue; break; }

        if (pfds[0].revents & POLLIN) {
            sockaddr_storage peer_addr{};
            socklen_t peer_len = sizeof(peer_addr);
            ssize_t n = recvfrom(impl_->fd, buf, sizeof(buf), 0, (sockaddr*)&peer_addr, &peer_len);
            if (n <= 0) continue;

            // 解析 QUIC 头获取连接 ID
            uint8_t type;
            uint32_t version;
            uint8_t scid[QUICHE_MAX_CONN_ID_LEN]; size_t scid_len = sizeof(scid);
            uint8_t dcid[QUICHE_MAX_CONN_ID_LEN]; size_t dcid_len = sizeof(dcid);
            uint8_t token[256];                  size_t token_len = sizeof(token);

            int rc = quiche_header_info(buf, n, QUICHE_MAX_CONN_ID_LEN, &version, &type,
                                        scid, &scid_len, dcid, &dcid_len,
                                        token, &token_len);
            if (rc < 0) { fprintf(stderr, "[server] header_info 失败 rc=%d len=%zd\n", rc, n); continue; }
            
            // 查找或创建连接：按 dcid（= 服务端 SCID）查找，对齐官方示例
            uint64_t dcid_val = 0;
            memcpy(&dcid_val, dcid, dcid_len < 8 ? dcid_len : 8);
            auto it = impl_->conns.find(dcid_val);
            if (it == impl_->conns.end()) {
                // quiche_accept：服务端创建连接的正确 API（内部处理 TLS/握手）
                QuicConn c;
                c.peer = peer_addr;
                c.peer_len = peer_len;
                c.scid = dcid_val;
                c.conn = quiche_accept(dcid, dcid_len, NULL, 0,
                    (sockaddr*)&addr, sizeof(addr),
                    (sockaddr*)&peer_addr, peer_len,
                    impl_->config);
                if (!c.conn) { fprintf(stderr, "[server] 连接创建失败\n"); continue; }
                it = impl_->conns.emplace(dcid_val, std::move(c)).first;
            }
            auto& c = it->second;

            // 喂包给引擎
            quiche_recv_info recv_info = {
                .from = (struct sockaddr*)&peer_addr, .from_len = peer_len,
                .to = (struct sockaddr*)&addr, .to_len = sizeof(addr),
            };
            ssize_t done = quiche_conn_recv(c.conn, buf, n, &recv_info);
                        if (done < 0) continue;

            // 握手完成后创建 h3 连接
            if (quiche_conn_is_established(c.conn) && !c.h3) {
                const uint8_t* proto; size_t proto_len;
                quiche_conn_application_proto(c.conn, &proto, &proto_len);
                if (proto && proto_len >= 2 && memcmp(proto, "h3", 2) == 0) {
                    c.h3 = quiche_h3_conn_new_with_transport(c.conn, impl_->h3_config);
                }
                            }

            // 处理 h3 事件
            if (c.h3) {
                quiche_h3_event* ev;
                int64_t stream_id;
                int poll_count = 0;
                while ((stream_id = quiche_h3_conn_poll(c.h3, c.conn, &ev)) >= 0) {
                    auto t = quiche_h3_event_type(ev);
                                        ++poll_count;
                    if (t == QUICHE_H3_EVENT_HEADERS) {
                        c.request_stream_id = stream_id;
                        c.request_method.clear();
                        c.request_path.clear();
                        c.request_body.clear();
                        c.expecting_body = false;
                        quiche_h3_event_for_each_header(ev, [](uint8_t* name, size_t namelen,
                            uint8_t* value, size_t valuelen, void* argp) {
                            auto* c2 = static_cast<QuicConn*>(argp);
                            if (namelen == 7 && memcmp(name, ":method", 7) == 0)
                                c2->request_method.assign((const char*)value, valuelen);
                            if (namelen == 5 && memcmp(name, ":path", 5) == 0)
                                c2->request_path.assign((const char*)value, valuelen);
                            return 0;
                        }, &c);
                    } else if (t == QUICHE_H3_EVENT_DATA) {
                        c.expecting_body = true;
                        // 读取 body
                        uint8_t body_buf[65536];
                        ssize_t body_n;
                        while ((body_n = quiche_h3_recv_body(c.h3, c.conn, stream_id,
                                body_buf, sizeof(body_buf))) > 0) {
                            c.request_body.append((const char*)body_buf, body_n);
                        }
                    } else if (t == QUICHE_H3_EVENT_FINISHED) {
                        // 处理请求
                        if (c.request_method == "GET" && c.request_path == "/events") {
                            // 事件推送流订阅：响应 200（fin=false），持续推送 agent 事件
                            c.events_subscribed = true;
                            c.events_stream = stream_id;
                            quiche_h3_header ev_headers[] = {
                                {(uint8_t*)":status", 7, (uint8_t*)"200", 3},
                                {(uint8_t*)"content-type", 12, (uint8_t*)"text/event-stream", 17},
                            };
                            quiche_h3_send_response(c.h3, c.conn, stream_id,
                                                    ev_headers, 2, false);
                        } else if (c.request_method == "POST" && c.request_path == "/message") {
                            std::string resp_body;
                            if (impl_->cbs.on_message)
                                resp_body = impl_->cbs.on_message(c.request_body);
                            else
                                resp_body = "{\"status\":\"ok\"}";
                            quiche_h3_header resp_headers[] = {
                                {(uint8_t*)":status", 7, (uint8_t*)"200", 3},
                                {(uint8_t*)"content-type", 12, (uint8_t*)"application/json", 16},
                                {(uint8_t*)"server", 6, (uint8_t*)"realagent", 9},
                            };
                            quiche_h3_send_response(c.h3, c.conn, c.request_stream_id,
                                                    resp_headers, 3, false);
                            quiche_h3_send_body(c.h3, c.conn, c.request_stream_id,
                                                (const uint8_t*)resp_body.data(),
                                                resp_body.size(), true);
                        } else {
                            const char* err = "{\"error\":\"not found\"}";
                            quiche_h3_header resp_headers[] = {
                                {(uint8_t*)":status", 7, (uint8_t*)"404", 3},
                                {(uint8_t*)"content-type", 12, (uint8_t*)"application/json", 16},
                            };
                            quiche_h3_send_response(c.h3, c.conn, c.request_stream_id,
                                                    resp_headers, 2, false);
                            quiche_h3_send_body(c.h3, c.conn, c.request_stream_id,
                                                (const uint8_t*)err, strlen(err), true);
                        }
                        c.request_method.clear();
                        c.request_path.clear();
                        c.request_body.clear();
                        c.expecting_body = false;
                    }
                    quiche_h3_event_free(ev);
                }
            }

            // 发送（无论是否 established——握手包也要发）
            uint8_t out[65536];
            quiche_send_info send_info{};
            ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &send_info);
            if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr*)&c.peer, c.peer_len);
        }

        // 所有连接：定时器 + 发送挂起数据
        for (auto it = impl_->conns.begin(); it != impl_->conns.end();) {
            auto& c = it->second;
            if (quiche_conn_is_closed(c.conn)) {
                if (c.h3) { quiche_h3_conn_free(c.h3); c.h3 = nullptr; }
                quiche_conn_free(c.conn); c.conn = nullptr;
                it = impl_->conns.erase(it);
                continue;
            }
            uint8_t out[65536];
            quiche_send_info send_info{};
            ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &send_info);
            if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr*)&c.peer, c.peer_len);
            ++it;
        }
    }
    impl_->cleanup();
    running_ = false;
    fprintf(stderr, "[server] 已停止\n");
}

} // namespace realagent