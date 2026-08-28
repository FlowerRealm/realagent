#include "agent/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "config.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

/* 会话 id：`YYYYMMDD-HHMMSS-xxxx`。按字典序排 = 按时间排，所以清单不需要索引文件；
 * 后四位随机是为了同一秒内开两个会话不撞名（真会发生：客户端重连时连按两下 /new）。 */
std::string make_id() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    static std::mt19937 rng{std::random_device{}()};
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%04x", (unsigned)(rng() & 0xffff));
    return std::string(stamp) + "-" + suffix;
}

fs::path path_of(const std::string& id) {
    return fs::path(Config::session_dir()) / (id + ".jsonl");
}

/* 标题：第一条 user 消息的第一个 text 块，截到 60 字节。
 * 从内容里现取，不另存——另存就得管它什么时候失效。 */
constexpr std::size_t kTitleMax = 60;

std::string title_of(const nlohmann::json& msg) {
    // value() 在非对象上会抛。坏行是常态（断电写了一半），不能让它变成一次崩溃
    if (!msg.is_object() || msg.value("role", std::string()) != "user") return "";
    // find 在非对象上恒返回 end()，所以不必先问一句 is_object
    const auto content = msg.find("content");
    if (content == msg.end() || !content->is_array()) return "";
    for (const nlohmann::json& b : *content) {
        if (!b.is_object() || b.value("type", std::string()) != "text") continue;
        std::string s = b.value("text", std::string());
        // 换行会把清单一行撑成多行，直接压成空格
        std::replace(s.begin(), s.end(), '\n', ' ');
        if (s.size() > kTitleMax) {
            // UTF-8 不许切在半个字符上：退到最近的字符起始字节
            std::size_t cut = kTitleMax;
            while (cut > 0 && (s[cut] & 0xC0) == 0x80) --cut;
            s = s.substr(0, cut) + "…";
        }
        return s;
    }
    return "";
}

} // namespace

Session::Session() : id_(make_id()), path_(path_of(id_).string()) {}

void Session::append(const nlohmann::json& msg) {
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::app);
    if (!f) {
        fprintf(stderr, "[session] 写不进 %s，本条未落盘\n", path_.c_str());
        return;
    }
    // 一行一条，不缩进——JSONL 的行边界就是记录边界，dump 里出现换行就全散了
    f << msg.dump() << '\n';
}

bool Session::resume(const std::string& id, nlohmann::json& out) {
    const fs::path p = path_of(id);
    std::ifstream f(p);
    if (!f) return false;

    nlohmann::json msgs = nlohmann::json::array();
    std::string line;
    long long lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (line.empty()) continue;
        nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            // 坏行跳过而不是整个会话作废：append-only 文件的末尾可能是断电时写了一半的，
            // 为了那一行丢掉前面几百条对话是本末倒置
            fprintf(stderr, "[session] %s:%lld 不是合法 JSON，跳过\n", p.c_str(), lineno);
            continue;
        }
        msgs.push_back(std::move(parsed));
    }
    out = std::move(msgs);
    id_ = id;
    path_ = p.string();
    return true;
}

std::vector<SessionInfo> Session::list() {
    std::vector<SessionInfo> out;
    std::error_code ec;
    const fs::path dir(Config::session_dir());
    if (!fs::is_directory(dir, ec)) return out; // 还没开过会话，不是错

    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file() || e.path().extension() != ".jsonl") continue;

        std::ifstream f(e.path());
        if (!f) continue;
        SessionInfo info;
        info.id = e.path().stem().string();
        info.messages = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            ++info.messages;
            if (!info.title.empty()) continue;
            if (const nlohmann::json m = nlohmann::json::parse(line, nullptr, false); !m.is_discarded())
                info.title = title_of(m);
        }
        // file_clock → system_clock：这个 libc++ 没有 clock_cast，用两个时钟的"此刻"
        // 之差换算。误差在两次 now() 之间，对"最近改过的排前面"绰绰有余。
        const auto tp = fs::last_write_time(e.path(), ec);
        info.mtime = 0;
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                tp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            info.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                             sys.time_since_epoch())
                             .count();
        }
        out.push_back(std::move(info));
    }
    // 最近写过的在前：恢复会话十有八九是恢复上一个
    std::sort(out.begin(), out.end(),
              [](const SessionInfo& a, const SessionInfo& b) { return a.mtime > b.mtime; });
    return out;
}

} // namespace realagent
