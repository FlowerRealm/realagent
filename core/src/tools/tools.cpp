#include "tools/tools.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace realagent {

namespace {

constexpr size_t kMaxOut = 50000; // 工具输出截断：50KB

/* 结果只有一种形状：{"status", "output"}。status 非零即错，output 是给模型看的文本。 */
nlohmann::json result(int status, std::string output) {
    return nlohmann::json{{"status", status}, {"output", std::move(output)}};
}
nlohmann::json fail(const std::string& msg) { return result(1, msg); }
nlohmann::json ok(const std::string& what) { return result(0, what); }

/* 取一个字符串参数；缺失/非字符串返回 nullopt。
 * 参数是模型给的，形状不由 core 说了算——const operator[] 撞上缺键是未定义行为，
 * 这里只能按迭代器查。 */
std::optional<std::string> arg(const nlohmann::json& params, std::string_view key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/* ==================== read ==================== */

nlohmann::json do_read(const nlohmann::json& params) {
    const auto path = arg(params, "file_path");
    if (!path) return fail("missing file_path");

    std::ifstream f(*path, std::ios::binary);
    if (!f) return fail("cannot open: " + *path);

    // 多读一个字节：只读 kMaxOut 的话，"文件恰好这么大"与"文件更大被截断"
    // 读回来一模一样（gcount 都是 kMaxOut），分不出来就只能一律当截断处理，
    // 于是恰好读满的文件被无辜削掉三个字节。多读一个，这个特殊情况就没了
    std::string buf(kMaxOut + 1, '\0');
    f.read(buf.data(), kMaxOut + 1);
    buf.resize(static_cast<size_t>(f.gcount()));
    if (buf.size() > kMaxOut) {
        buf.resize(kMaxOut);
        buf.replace(buf.size() - 3, 3, "...");
    }
    return result(0, std::move(buf));
}

/* ==================== edit（+x-0 = 创建） ==================== */

nlohmann::json do_edit(const nlohmann::json& params) {
    const auto path = arg(params, "file_path");
    if (!path) return fail("missing file_path");
    const auto new_s = arg(params, "new_string");
    if (!new_s) return fail("missing new_string");
    const std::string old_s = arg(params, "old_string").value_or("");

    std::ifstream in(*path, std::ios::binary);
    if (!in) {
        // 文件不存在 → 创建（write 语义：edit +x-0）
        std::ofstream out(*path, std::ios::binary);
        if (!out) return fail("cannot create: " + *path);
        out << *new_s;
        return ok("created");
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (old_s.empty()) {
        std::ofstream out(*path, std::ios::binary | std::ios::app);
        if (!out) return fail("cannot append");
        out << *new_s;
        return ok("appended");
    }
    const size_t hit = content.find(old_s);
    if (hit == std::string::npos) return fail("old_string not found");
    content.replace(hit, old_s.size(), *new_s);

    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write");
    out << content;
    return ok("edited");
}

/* ==================== bash ==================== */

/* 在跑的子进程组（0 = 手上没有）。中止请求从事件循环线程进来，读循环在 agent 线程——
 * 锁护住"登记"与"取用"不交错，否则会朝一个已经回收的 pid 开枪。 */
std::mutex g_bash_mtx;
pid_t g_bash_pgid = 0;
bool g_bash_killed = false; // 本次调用挨过刀：收尾时要盯着它死透

/* tool_output 帧（PROTOCOL.md）：命令还在跑的时候就把输出推出去。
 * 与 tool_result 不是二选一——完整输出照旧在结果里回传，这里推的是"现在长什么样"。 */
void emit_output(const EmitFn& emit, const std::string& call_id, const std::string& text) {
    if (!emit) return;
    nlohmann::json ev;
    ev["call_id"] = call_id;
    // 一条流，不分家（ADR-0017）：stdout 与 stderr 在同一个管道里，
    // 字段值就叫 output，不留一个说谎的 "stdout"
    ev["stream"] = "output";
    ev["text"] = text;
    emit("tool_output", ev.dump());
}

nlohmann::json do_bash(const std::string& call_id, const nlohmann::json& params, const EmitFn& emit) {
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
        if (pid == 0) {
            /* 子进程：以下都是 async-signal-safe 的，多线程 fork 后只能用这些 */
            setpgid(0, 0); /* 自成进程组：中止时一枪打掉整棵子孙树，不留孤儿 */
            /* stdout 与 stderr 都接到同一个管道（ADR-0017）。只接 stdout 的话，
             * 命令失败时模型收到的是"退出码非零，无话可说"——报错原文全在 stderr，
             * 而那正是它判断该不该重试、怎么改的唯一依据。
             * 合流而不是两个管道：交错顺序就是人在终端里看见的顺序，
             * 两个管道各读各的会把因果打乱，而"哪一行来自哪条流"没有第二个读者。 */
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            execl("/bin/sh", "sh", "-c", cmd->c_str(), (char*)nullptr);
            _exit(127);
        }
        if (pid > 0) {
            setpgid(pid, pid); /* 父子各设一遍：谁先跑到都算数，不必猜调度 */
            g_bash_pgid = pid;
            g_bash_killed = false;
        }
    }

    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return fail("fork failed");
    }
    FILE* f = fdopen(fds[0], "r");
    if (!f) {
        close(fds[0]);
        return fail("fdopen failed");
    }

    /* 阻塞读就够了：中止不是让这里醒过来，是把它等的那个东西杀掉——
     * 子进程一死管道就到 EOF，循环自己会退。为此去 poll 一个标志位是白费力气。 */
    std::string out;
    std::array<char, 4096> line{};
    bool truncated = false;
    while (fgets(line.data(), (int)line.size(), f)) {
        const std::string chunk(line.data());
        if (out.size() + chunk.size() <= kMaxOut) {
            out += chunk;
            emit_output(emit, call_id, chunk);
        } else {
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
    if (killed) {
        /* SIGTERM 递出去了，没人保证它一定死。给一秒，还赖着就 SIGKILL 整组——
         * 用户按了中止，后台不许留下任何东西。 */
        for (int i = 0; i < 100 && !reaped; ++i) {
            if (waitpid(pid, &st, WNOHANG) > 0) reaped = true;
            else usleep(10000);
        }
        if (!reaped) kill(-pid, SIGKILL);
    }
    if (!reaped) waitpid(pid, &st, 0);

    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return result(rc, std::move(out)); // 非零退出码视为错误
}

/* ==================== 工具清单 ==================== */

const ToolDef k_tools[] = {
    {"read", "读文件", "读取指定文件的内容。路径不存在或不可读时返回错误。",
     R"({"type":"object","properties":{"file_path":{"type":"string","description":"文件路径"}},"required":["file_path"]})",
     false},
    {"edit", "编辑文件",
     "修改文件内容。目标文件不存在时创建新文件（old_string 可为空表示创建/追加）；"
     "old_string 为空时把 new_string 追加到文件末尾；否则替换第一个匹配的 old_string。",
     R"({"type":"object","properties":{"file_path":{"type":"string"},"old_string":{"type":"string","description":"被替换的原文；为空=创建或追加"},"new_string":{"type":"string"}},"required":["file_path","new_string"]})",
     true},
    {"bash", "执行命令", "在 shell 中执行命令，返回标准输出与标准错误（合流）。危险操作需用户确认。",
     R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
     true},
};

} // namespace

std::span<const ToolDef> tool_defs() { return k_tools; }

const ToolDef* find_tool(std::string_view name) {
    const auto it = std::find_if(std::begin(k_tools), std::end(k_tools),
                                 [&](const ToolDef& t) { return t.name == name; });
    return it == std::end(k_tools) ? nullptr : it;
}

nlohmann::json run_tool(const std::string& call_id, const std::string& name,
              const std::string& params_json, const EmitFn& emit) {
    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();
    if (name == "read") return do_read(params);
    if (name == "edit") return do_edit(params);
    if (name == "bash") return do_bash(call_id, params, emit);
    return fail("unknown tool: " + name);
}

void interrupt_tool() {
    std::lock_guard<std::mutex> lk(g_bash_mtx);
    if (g_bash_pgid <= 0) return;
    /* 打的是进程组，不是单个进程。第一次 SIGTERM，给命令一个自己收尾的机会；
     * 再来一次就 SIGKILL——捂着 TERM 不撒手、还攥着 stdout 的进程会把读循环
     * 一起吊死，用户再按一次就不该再有商量。 */
    kill(-g_bash_pgid, g_bash_killed ? SIGKILL : SIGTERM);
    g_bash_killed = true;
}

} // namespace realagent
