/*
 * bash.cpp — bash 工具：跑一条 shell 命令，边跑边推输出
 *
 * 中止（ADR-0002 R8）只有 bash 需要——它是唯一会跑很久的那个，所以 interrupt_tool()
 * 也定义在这个文件里：它要动的全部状态都在这儿，摊到壳里只会多一层转手。
 * 中止请求从事件循环线程进来，读循环在 agent 线程；单 agent 内工具严格顺序执行，
 * 同时至多一个子进程，一个 pid 就记得住，不需要表。
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <mutex>

#include "tools/tools.hpp"

namespace realagent {

namespace {

constexpr size_t kMaxOut = 50000; // bash 输出截断：50KB

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

} // namespace

ToolDef bash_def()
{
    return {"bash", "执行命令",
            "Run a command in the shell; returns stdout and stderr (merged into one stream).\n"
            "Dangerous operations require user confirmation.",
            R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
            true};
}

nlohmann::json bash_run(const std::string &call_id, const nlohmann::json &params,
                        const EmitFn &emit, const std::string &workdir)
{
    const auto cmd = tool_arg(params, "command");
    if (!cmd) return tool_fail("missing command");

    int fds[2];
    if (pipe(fds) != 0) return tool_fail("pipe failed");

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
        return tool_fail("fork failed");
    }
    FILE *f = fdopen(fds[0], "r");
    if (!f)
    {
        close(fds[0]);
        return tool_fail("fdopen failed");
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
    return tool_result(rc, std::move(out)); // 非零退出码视为错误
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
