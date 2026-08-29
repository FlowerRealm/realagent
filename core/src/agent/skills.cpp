/*
 * skills.cpp — 扫盘、读 frontmatter、拼提示词
 *
 * frontmatter 用 fkYAML 解析，不手写（ADR-0022 §5）。SKILL.md 是从互联网抄来的
 * 第三方输入，形状不由 core 说了算：现成 skill 里约三成的 description 是 YAML
 * 折叠标量（`description: >`，值在后续缩进行），手写的解析器碰上它不会报错，
 * 它会安静地把描述设成 `>`。「不兜底」反对的是替用户擦屁股，不是反对按格式的
 * 真实定义去解析它。
 */
#include "agent/skills.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "config.hpp"
/* fkYAML 0.4.4 用了 std::is_trivial，C++26 里它被弃用了。头文件是逐字节 vendored 的
 * （与 json.hpp 同一条路子），不改它一个字符——升级时才好逐字节替换。 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "fkYAML.hpp"
#pragma clang diagnostic pop

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 去掉行尾空白：`---` 后面跟一个 \r（Windows 换行）仍然是 `---` */
std::string_view rstrip(std::string_view s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

/* 取出 frontmatter 那一段原文。首行必须是 `---`，到下一条 `---` 为止。
 * 正文一个字都不看——那是模型的事，core 只要一个 description。 */
std::optional<std::string> frontmatter(const fs::path &md)
{
    std::ifstream f(md);
    std::string line;
    if (!std::getline(f, line) || rstrip(line) != "---") return std::nullopt;
    std::ostringstream yaml;
    while (std::getline(f, line))
    {
        if (rstrip(line) == "---") return yaml.str();
        yaml << line << '\n';
    }
    return std::nullopt; // 开了头没收尾，不是 frontmatter
}

/* 读一份 SKILL.md 的 description。读不出来就报错原文、返回 nullopt，
 * 调用方跳过这一个——skill 是 N 份各自独立的文件，一份坏了不让另一份变得可疑。 */
std::optional<std::string> read_description(const fs::path &md)
{
    const std::optional<std::string> yaml = frontmatter(md);
    if (!yaml)
    {
        fprintf(stderr, "[skill] %s: 没有 YAML frontmatter，跳过\n", md.c_str());
        return std::nullopt;
    }
    try
    {
        fkyaml::node n = fkyaml::node::deserialize(*yaml);
        if (!n.is_mapping() || !n.contains("description") || !n["description"].is_string())
        {
            fprintf(stderr, "[skill] %s: frontmatter 里没有字符串 description，跳过\n", md.c_str());
            return std::nullopt;
        }
        const std::string d = n["description"].get_value<std::string>();
        if (d.empty())
        {
            fprintf(stderr, "[skill] %s: description 是空的，跳过\n", md.c_str());
            return std::nullopt;
        }
        return d;
    } catch (const std::exception &e)
    {
        fprintf(stderr, "[skill] %s: frontmatter 解析失败：%s，跳过\n", md.c_str(), e.what());
        return std::nullopt;
    }
}

/* 扫一处目录。同名的后来者覆盖先到者——调用方按「远的先扫」的顺序调，近的就赢了。 */
void scan_dir(const fs::path &root, std::vector<Skill> &out)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return; // 没有这个目录：多数人一个 skill 都没有
    for (const fs::directory_entry &e : fs::directory_iterator(root, ec))
    {
        if (!e.is_directory(ec)) continue;
        const fs::path md = e.path() / "SKILL.md";
        if (!fs::is_regular_file(md, ec)) continue; // 目录里没有 SKILL.md：它就不是 skill
        const std::optional<std::string> desc = read_description(md);
        if (!desc) continue;
        Skill s{e.path().filename().string(), *desc, fs::absolute(md, ec).string()};
        const auto it = std::find_if(out.begin(), out.end(),
                                     [&](const Skill &o) { return o.name == s.name; });
        if (it != out.end())
            *it = std::move(s);
        else
            out.push_back(std::move(s));
    }
}

} // namespace

std::vector<Skill> scan_skills(const std::string &workdir)
{
    std::vector<Skill> out;
    // 远的先扫，近的后扫：workdir 里的同名 skill 就此盖掉全局那份
    scan_dir(fs::path(getenv_or("HOME", ".")) / ".realagent" / "skills", out);
    scan_dir(fs::path(workdir) / ".realagent" / "skills", out);
    // directory_iterator 的顺序是未指定的，排一下——同一个目录两次运行给同一份提示词
    std::sort(out.begin(), out.end(), [](const Skill &a, const Skill &b) { return a.name < b.name; });
    return out;
}

std::string skills_prompt(const std::vector<Skill> &skills)
{
    if (skills.empty()) return {};
    std::ostringstream s;
    s << "\n\nSkills available to you. Each one is a Markdown document of instructions. "
         "When a description matches what you are about to do, `read` that file first and "
         "follow it.\n";
    for (const Skill &k : skills) s << "- " << k.name << ": " << k.description << " (" << k.path << ")\n";
    return s.str();
}

} // namespace realagent
