/*
 * read.cpp — read 工具：把文件按行打出来，每行前面挂上 edit 要用的 anchor
 *
 * 行号与 hash 不是给人看的装饰，是 edit 的两个入参（ADR-0018）。
 * 模型不用数行、不用猜——它刚读到的那两个值原样填回去就行。
 */
#include "tools/tools.hpp"

namespace realagent {

ToolDef read_def()
{
    return {
        "read", "读文件",
        "读取文件内容。每行开头是 `行号 hash `——那两个值就是 edit 的 line 与 hash 参数。",
        R"({"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]})",
        false};
}

nlohmann::json read_run(const nlohmann::json &params, const std::string &workdir)
{
    const auto arg_path = tool_arg(params, "file_path");
    if (!arg_path) return tool_fail("missing file_path");
    const std::string path = tool_resolve(workdir, *arg_path);
    if (!std::filesystem::exists(path)) return tool_fail("cannot open: " + path);

    std::string out;
    const auto lines = read_lines(path);
    for (size_t i = 0; i < lines.size(); ++i)
        out += std::to_string(i + 1) + ' ' + hash_line(lines[i]) + ' ' + lines[i] + '\n';
    return tool_result(0, std::move(out));
}

} // namespace realagent
