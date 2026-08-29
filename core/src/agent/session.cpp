#include "agent/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/gen/File.h>

#include "config.hpp"

namespace realagent {
namespace fs = std::filesystem;

namespace {

/* 会话 id：`YYYYMMDD-HHMMSS-xxxx`。按字典序排 = 按时间排，所以清单不需要索引文件；
 * 后四位随机是为了同一秒内开两个会话不撞名（真会发生：客户端重连时连按两下 /new）。 */
std::string make_id()
{
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

fs::path path_of(const std::string &dir, const std::string &id)
{
    return fs::path(dir) / (id + ".jsonl");
}

/* 标题：第一条 user 消息的第一个 text 块，截到 60 字节。
 * 从内容里现取，不另存——另存就得管它什么时候失效。 */
constexpr std::size_t kTitleMax = 60;

std::string title_of(const nlohmann::json &msg)
{
    // value() 在非对象上会抛。坏行是常态（断电写了一半），不能让它变成一次崩溃
    if (!msg.is_object() || msg.value("role", std::string()) != "user") return "";
    // find 在非对象上恒返回 end()，所以不必先问一句 is_object
    const auto content = msg.find("content");
    if (content == msg.end() || !content->is_array()) return "";
    for (const nlohmann::json &b : *content)
    {
        if (!b.is_object() || b.value("type", std::string()) != "text") continue;
        std::string s = b.value("text", std::string());
        // 换行会把清单一行撑成多行，直接压成空格
        std::replace(s.begin(), s.end(), '\n', ' ');
        if (s.size() > kTitleMax)
        {
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

Session::Session(std::string dir)
    : dir_(std::move(dir)), id_(make_id()), path_(path_of(dir_, id_).string())
{
}

void Session::append(const nlohmann::json &msg)
{
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    const std::string line = msg.dump() + '\n';
    if (!folly::writeFile(line, path_.c_str(), O_WRONLY | O_CREAT | O_APPEND))
        fprintf(stderr, "[session] 写不进 %s，本条未落盘\n", path_.c_str());
}

bool Session::resume(const std::string &id, nlohmann::json &out)
{
    if (!read(dir_, id, out)) return false;
    id_ = id;
    path_ = path_of(dir_, id).string();
    return true;
}

bool Session::read(const std::string &dir, const std::string &id, nlohmann::json &out)
{
    const fs::path p = path_of(dir, id);
    nlohmann::json msgs = nlohmann::json::array();
    try
    {
        long long lineno = 0;
        folly::gen::byLine(folly::File(p.c_str())).foreach ([&](folly::StringPiece sv) {
            ++lineno;
            if (sv.empty()) return;
            nlohmann::json parsed = nlohmann::json::parse(sv.begin(), sv.end(), nullptr, false);
            if (parsed.is_discarded())
            {
                // 坏行跳过而不是整个会话作废：append-only 文件的末尾可能是断电时写了一半的，
                // 为了那一行丢掉前面几百条对话是本末倒置
                fprintf(stderr, "[session] %s:%lld 不是合法 JSON，跳过\n", p.c_str(), lineno);
                return;
            }
            msgs.push_back(std::move(parsed));
        });
    } catch (const std::system_error &)
    {
        return false;
    }
    out = std::move(msgs);
    return true;
}

std::vector<SessionInfo> Session::list(const std::string &dir_arg)
{
    std::vector<SessionInfo> out;
    std::error_code ec;
    const fs::path dir(dir_arg);
    if (!fs::is_directory(dir, ec)) return out; // 还没开过会话，不是错

    for (const auto &e : fs::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (!e.is_regular_file() || e.path().extension() != ".jsonl") continue;

        SessionInfo info;
        info.id = e.path().stem().string();
        info.messages = 0;
        try
        {
            folly::gen::byLine(folly::File(e.path().c_str())).foreach ([&](folly::StringPiece sv) {
                if (sv.empty()) return;
                ++info.messages;
                if (!info.title.empty()) return;
                if (const nlohmann::json m = nlohmann::json::parse(sv.begin(), sv.end(), nullptr, false);
                    !m.is_discarded())
                    info.title = title_of(m);
            });
        } catch (const std::system_error &)
        {
            continue;
        }

        // file_clock → system_clock：这个 libc++ 没有 clock_cast，用两个时钟的"此刻"
        // 之差换算。误差在两次 now() 之间，对"最近改过的排前面"绰绰有余。
        const auto tp = fs::last_write_time(e.path(), ec);
        info.mtime = 0;
        if (!ec)
        {
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
              [](const SessionInfo &a, const SessionInfo &b) { return a.mtime > b.mtime; });
    return out;
}

} // namespace realagent
