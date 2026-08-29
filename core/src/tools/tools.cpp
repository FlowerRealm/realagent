#include "tools/tools.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>

#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/gen/File.h>

namespace realagent {

namespace {

constexpr size_t kMaxOut = 50000; // bash 输出截断：50KB

/* 结果只有一种形状：{"status", "output"}。status 非零即错，output 是给模型看的文本。 */
nlohmann::json result(int status, std::string output)
{
    return nlohmann::json{{"status", status}, {"output", std::move(output)}};
}
nlohmann::json fail(const std::string &msg) { return result(1, msg); }
nlohmann::json ok(const std::string &what) { return result(0, what); }

/* 取一个字符串参数；缺失/非字符串返回 nullopt。
 * 参数是模型给的，形状不由 core 说了算——const operator[] 撞上缺键是未定义行为，
 * 这里只能按迭代器查。 */
std::optional<std::string> arg(const nlohmann::json &params, std::string_view key)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/* ==================== read / edit（ADR-0018） ==================== */

/* 相对路径从 agent 的工作目录算起（ADR-0019）。core 进程的 cwd 与任何 agent 都无关 */
std::string resolve(const std::string &workdir, const std::string &path)
{
    const std::filesystem::path p(path);
    return p.is_absolute() ? path : (std::filesystem::path(workdir) / p).string();
}

/* 一行的 hash：FNV-1a，3 个十六进制字符。空白不算——跑一遍格式化不该让它变。 */
std::string hash_line(folly::StringPiece s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        if (!std::isspace(c)) h = (h ^ c) * 16777619u;
    char buf[4];
    std::snprintf(buf, sizeof buf, "%03x", h & 0xfff);
    return buf;
}

nlohmann::json do_read(const nlohmann::json &params, const std::string &workdir)
{
    const auto arg_path = arg(params, "file_path");
    if (!arg_path) return fail("missing file_path");
    const std::string path = resolve(workdir, *arg_path);
    if (!std::filesystem::exists(path)) return fail("cannot open: " + path);

    std::string out;
    size_t i = 0;
    folly::gen::byLine(folly::File(path)).foreach ([&](folly::StringPiece line) {
        out += std::to_string(++i) + ' ' + hash_line(line) + ' ' + line.str() + '\n';
    });
    return result(0, std::move(out));
}

/* 一条 edit。返回空串即成功，否则是人话原因。
 * hash 对得上才动手——对不上说明这一行已经变了（另一个 agent、人、git checkout）。 */
std::string edit_one(const nlohmann::json &e, const std::string &workdir)
{
    const auto arg_path = arg(e, "file_path");
    const auto text = arg(e, "new_text");
    if (!arg_path || !text) return "edit 缺 file_path 或 new_text";

    const std::string path = resolve(workdir, *arg_path);

    // 读所有行；文件不存在时（创建模式）保持 lines 为空
    std::vector<std::string> lines;
    try
    {
        folly::gen::byLine(folly::File(path)).foreach ([&](folly::StringPiece line) {
            lines.emplace_back(line.str());
        });
    } catch (const std::system_error &)
    {
    }

    const auto n = e.find("line");
    const size_t id = n != e.end() && n->is_number_unsigned() ? n->get<size_t>() : 0;
    if (id == 0) // 不给行号 = 写整个文件（创建）
        return folly::writeFile(*text + '\n', path.c_str()) ? "" : "写不了: " + path;
    if (id > lines.size()) return "行号越界: " + path;
    if (hash_line(lines[id - 1]) != arg(e, "hash").value_or(""))
        return "第 " + std::to_string(id) + " 行的 hash 对不上，它变了，重新 read: " + path;

    // 新内容为空即删掉这一行；否则替换
    if (text->empty())
        lines.erase(lines.begin() + static_cast<long>(id) - 1);
    else
        lines[id - 1] = *text;

    // 拼回并整体写入（O_TRUNC）
    std::string content;
    content.reserve(lines.size() * 80);
    for (const auto &l : lines)
    {
        content += l;
        content += '\n';
    }
    return folly::writeFile(content, path.c_str()) ? "" : "写不了: " + path;
}

nlohmann::json do_edit(const nlohmann::json &params, const std::string &workdir)
{
    const auto it = params.find("edits");
    if (it == params.end() || !it->is_array() || it->empty()) return fail("missing edits");

    // 逐条执行、遇错即停。报错要说清停在第几条，前面那些已经落盘了
    for (size_t i = 0; i < it->size(); ++i)
        if (const std::string err = edit_one((*it)[i], workdir); !err.empty())
            return fail("第 " + std::to_string(i + 1) + " 条：" + err +
                        (i ? "\n前 " + std::to_string(i) + " 条已写入" : ""));
    return ok(std::to_string(it->size()) + " edits");
}

/* ==================== bash ==================== */

/* 在跑的子进程组（0 = 手上没有）。中止请求从事件循环线程进来，读循环在 agent 线程——
 * 锁护住"登记"与"取用"不交错，否则会朝一个已经回收的 pid 开枪。 */
std::mutex g_bash_mtx;
pid_t g_bash_pgid = 0;
bool g_bash_killed = false; // 本次调用挨过刀：收尾时要盯着它死透

/* tool_output 帧（PROTOCOL.md）：命令还在跑的时候就把输出推出去。
 * 与 tool_result 不是二选一——完整输出照旧在结果里回传，这里推的是"现在长什么样"。 */
void emit_output(const EmitFn &emit, const std::string &call_id, const std::string &text)
{
    if (!emit) return;
    nlohmann::json ev;
    ev["call_id"] = call_id;
    // 一条流，不分家（ADR-0017）：stdout 与 stderr 在同一个管道里，
    // 字段值就叫 output，不留一个说谎的 "stdout"
    ev["stream"] = "output";
    ev["text"] = text;
    emit("tool_output", ev.dump());
}

nlohmann::json do_bash(const std::string &call_id, const nlohmann::json &params,
                       const EmitFn &emit, const std::string &workdir)
{
    const auto cmd = arg(params, "command");
    if (!cmd) return fail("missing command");

    int fds[2];
    if (pipe(fds) != 0) return fail("pipe failed");

    /* 不用 popen：拿不到 pid 就没法中止。fork 与登记同在锁里，中止请求要么在开工之前到
     * （那时它什么都不做，executor 会拒掉这次调用），要么排在登记之后——
     * 不存在"子进程已经跑起来但没人记得它"的缝。 */
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(g_bash_mtx);
        pid = fork();
        if (pid == 0)
        {
            /* 子进程：以下都是 async-signal-safe 的，多线程 fork 后只能用这些 */
            setpgid(0, 0); /* 自成进程组：中止时一枪打掉整棵子孙树，不留孤儿 */
            /* 到 agent 的工作目录去（ADR-0019）。不 chdir 的话继承的是 core 进程的 cwd，
             * 而 core 是全机一个、cwd 是启动它那个 shell 当时在哪——跟这个 agent 无关。
             * chdir 是 async-signal-safe 的，可以在 fork 之后 exec 之前调用 */
            if (chdir(workdir.c_str()) != 0) _exit(126);
            /* stdout 与 stderr 都接到同一个管道（ADR-0017）。只接 stdout 的话，
             * 命令失败时模型收到的是"退出码非零，无话可说"——报错原文全在 stderr，
             * 而那正是它判断该不该重试、怎么改的唯一依据。
             * 合流而不是两个管道：交错顺序就是人在终端里看见的顺序，
             * 两个管道各读各的会把因果打乱，而"哪一行来自哪条流"没有第二个读者。 */
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            execl("/bin/sh", "sh", "-c", cmd->c_str(), (char *)nullptr);
            _exit(127);
        }
        if (pid > 0)
        {
            setpgid(pid, pid); /* 父子各设一遍：谁先跑到都算数，不必猜调度 */
            g_bash_pgid = pid;
            g_bash_killed = false;
        }
    }

    close(fds[1]);
    if (pid < 0)
    {
        close(fds[0]);
        return fail("fork failed");
    }
    FILE *f = fdopen(fds[0], "r");
    if (!f)
    {
        close(fds[0]);
        return fail("fdopen failed");
    }

    /* 阻塞读就够了：中止不是让这里醒过来，是把它等的那个东西杀掉——
     * 子进程一死管道就到 EOF，循环自己会退。为此去 poll 一个标志位是白费力气。 */
    std::string out;
    std::array<char, 4096> line{};
    bool truncated = false;
    while (fgets(line.data(), (int)line.size(), f))
    {
        const std::string chunk(line.data());
        if (out.size() + chunk.size() <= kMaxOut)
        {
            out += chunk;
            emit_output(emit, call_id, chunk);
        }
        else
        {
            truncated = true; // 超限后只吞不推：管道还得读干净，撒手等于给命令一个 SIGPIPE
        }
    }
    if (truncated && out.size() >= 3) out.replace(out.size() - 3, 3, "...");
    fclose(f);

    bool killed;
    {
        std::lock_guard<std::mutex> lk(g_bash_mtx);
        g_bash_pgid = 0; // 摘牌：读到 EOF 就没什么可中止的了
        killed = g_bash_killed;
    }

    int st = 0;
    bool reaped = false;
    if (killed)
    {
        /* SIGTERM 递出去了，没人保证它一定死。给一秒，还赖着就 SIGKILL 整组——
         * 用户按了中止，后台不许留下任何东西。 */
        for (int i = 0; i < 100 && !reaped; ++i)
        {
            if (waitpid(pid, &st, WNOHANG) > 0)
                reaped = true;
            else
                usleep(10000);
        }
        if (!reaped) kill(-pid, SIGKILL);
    }
    if (!reaped) waitpid(pid, &st, 0);

    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return result(rc, std::move(out)); // 非零退出码视为错误
}

/* ==================== 工具清单 ==================== */

const ToolDef k_tools[] = {
    {"read", "读文件",
     "读取文件内容。每行开头是 `行号 hash `——那两个值就是 edit 的 line 与 hash 参数。",
     R"({"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]})",
     false},
    {"edit", "编辑文件",
     "把第 line 行换成 new_text。line 与 hash 就是 read 每行开头那两个值，原样填。\n"
     "new_text 带换行即换成多行；为空串即删掉这一行；不给 line 即写整个文件（创建）。\n"
     "hash 对不上说明那一行变了——重新 read，不要猜。\n"
     "edits 是一个数组，逐条执行、遇错即停；跨文件也行。",
     R"({"type":"object","properties":{"edits":{"type":"array","items":{"type":"object","properties":{"file_path":{"type":"string"},"line":{"type":"integer","description":"行号；不给 = 写整个文件"},"hash":{"type":"string","description":"该行的 hash"},"new_text":{"type":"string","description":"新内容；空串 = 删掉这一行"}},"required":["file_path","new_text"]}}},"required":["edits"]})",
     true},
    /* 下面两个由 Executor 实现，不在这个文件里——它们要认识 Agents，
     * 而 tools/ 在 agent/ 下面，反过来包含就是层级倒挂。
     * 定义留在这张表里：LLM 看见的工具清单只有一份。 */
    {"spawn", "派生 agent",
     "派生一个新 agent 去干一件事，立刻返回它的 id，不等它跑完。\n"
     "in_edges 是谁能给它发消息、谁收它的完成通知；out_edges 是它能给谁发消息。\n"
     "要收它的产出就把自己的 id 写进 in_edges；两个列表都填同一组人就是互相能发。\n"
     "两个列表里只能填你自己或你已经认识的 agent——授不出自己没有的能力。",
     R"({"type":"object","properties":{"workdir":{"type":"string","description":"它的工作目录，必填"},"prompt":{"type":"string","description":"派给它的第一条消息"},"in_edges":{"type":"array","items":{"type":"string"}},"out_edges":{"type":"array","items":{"type":"string"}}},"required":["workdir","prompt"]})",
     true},
    {"send_message", "给别的 agent 发消息",
     "把一条消息投进另一个 agent 的收件箱。只能发给你有出边的那些——没有边就不知道它存在。",
     R"({"type":"object","properties":{"to":{"type":"string"},"text":{"type":"string"}},"required":["to","text"]})",
     false},
    {"bash", "执行命令", "在 shell 中执行命令，返回标准输出与标准错误（合流）。危险操作需用户确认。",
     R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
     true},
};

} // namespace

std::span<const ToolDef> tool_defs() { return k_tools; }

const ToolDef *find_tool(std::string_view name)
{
    const auto it = std::find_if(std::begin(k_tools), std::end(k_tools),
                                 [&](const ToolDef &t) { return t.name == name; });
    return it == std::end(k_tools) ? nullptr : it;
}

nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const std::string &params_json, const EmitFn &emit,
                        const std::string &workdir)
{
    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();
    if (name == "read") return do_read(params, workdir);
    if (name == "edit") return do_edit(params, workdir);
    if (name == "bash") return do_bash(call_id, params, emit, workdir);
    // spawn / send_message 由 Executor 拦下，走不到这里
    return fail("unknown tool: " + name);
}

void interrupt_tool()
{
    std::lock_guard<std::mutex> lk(g_bash_mtx);
    if (g_bash_pgid <= 0) return;
    /* 打的是进程组，不是单个进程。第一次 SIGTERM，给命令一个自己收尾的机会；
     * 再来一次就 SIGKILL——捂着 TERM 不撒手、还攥着 stdout 的进程会把读循环
     * 一起吊死，用户再按一次就不该再有商量。 */
    kill(-g_bash_pgid, g_bash_killed ? SIGKILL : SIGTERM);
    g_bash_killed = true;
}

} // namespace realagent
