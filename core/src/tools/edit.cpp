/*
 * edit.cpp — edit 工具：把一行换成一段文本（ADR-0018）
 *
 * 改、删、插、创建是它的四种用法，不是四个操作：new_text 为空即删，带换行即多行，
 * 不给 line 即写整个文件。所以没有独立的 write 工具。
 *
 * hash 对得上才动手——对不上说明这一行已经变了（另一个 agent、人、git checkout）。
 */
#include "tools/tools.hpp"

namespace realagent {

ToolDef edit_def()
{
    return {
        "edit", "编辑文件",
        "把第 line 行换成 new_text。line 与 hash 就是 read 每行开头那两个值，原样填。\n"
        "new_text 带换行即换成多行；为空串即删掉这一行；不给 line 即写整个文件（创建）。\n"
        "hash 对不上说明那一行变了——重新 read，不要猜。\n"
        "edits 是一个数组，逐条执行、遇错即停；跨文件也行。",
        R"({"type":"object","properties":{"edits":{"type":"array","items":{"type":"object","properties":{"file_path":{"type":"string"},"line":{"type":"integer","description":"行号；不给 = 写整个文件"},"hash":{"type":"string","description":"该行的 hash"},"new_text":{"type":"string","description":"新内容；空串 = 删掉这一行"}},"required":["file_path","new_text"]}}},"required":["edits"]})",
        true};
}

namespace {

/* 一条 edit。返回空串即成功，否则是人话原因。 */
std::string edit_one(const nlohmann::json &e, const std::string &workdir)
{
    const auto arg_path = tool_arg(e, "file_path");
    const auto text = tool_arg(e, "new_text");
    if (!arg_path || !text) return "edit 缺 file_path 或 new_text";

    const std::string path = tool_resolve(workdir, *arg_path);
    auto lines = read_lines(path);
    const auto n = e.find("line");
    const size_t id = n != e.end() && n->is_number_unsigned() ? n->get<size_t>() : 0;
    if (id == 0) // 不给行号 = 写整个文件（创建）
        return write_lines(path, {*text}) ? "" : "写不了: " + path;
    if (id > lines.size()) return "行号越界: " + path;
    if (hash_line(lines[id - 1]) != tool_arg(e, "hash").value_or(""))
        return "第 " + std::to_string(id) + " 行的 hash 对不上，它变了，重新 read: " + path;

    // 新内容为空即删掉这一行；带换行就是多行，write_lines 拼回去时自然展开
    if (text->empty())
        lines.erase(lines.begin() + static_cast<long>(id) - 1);
    else
        lines[id - 1] = *text;
    return write_lines(path, lines) ? "" : "写不了: " + path;
}

} // namespace

nlohmann::json edit_run(const nlohmann::json &params, const std::string &workdir)
{
    const auto it = params.find("edits");
    if (it == params.end() || !it->is_array() || it->empty()) return tool_fail("missing edits");

    // 逐条执行、遇错即停。报错要说清停在第几条，前面那些已经落盘了
    for (size_t i = 0; i < it->size(); ++i)
        if (const std::string err = edit_one((*it)[i], workdir); !err.empty())
            return tool_fail("第 " + std::to_string(i + 1) + " 条：" + err +
                             (i ? "\n前 " + std::to_string(i) + " 条已写入" : ""));
    return tool_ok(std::to_string(it->size()) + " edits");
}

} // namespace realagent
