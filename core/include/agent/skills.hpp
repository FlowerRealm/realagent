/*
 * skills.hpp — skill：一个目录 + 一份 SKILL.md，纯提示词（ADR-0022）
 *
 * core 不加载它、不执行它、不给它任何执行语义——只把它的名字、描述和路径写进
 * system prompt。正文读不读由模型决定，用现成的 `read` 读，**没有第七个工具**
 * （ADR-0018 拒过 `read_skill`，理由是工具每多一个，模型每次调用就多一次选错的机会）。
 *
 * 形状不由本项目定义，照抄公开的 Agent Skills 规范（https://agentskills.io/specification）：
 * YAML frontmatter + Markdown 正文，`name` 必须与父目录名一致——所以名字直接取目录名，
 * 解析器唯一的活是拿到 `description`。
 *
 * 两处来源，同名近的覆盖远的：
 *   ~/.realagent/skills/<name>/SKILL.md          跟着人走
 *   <workdir>/.realagent/skills/<name>/SKILL.md  跟着仓库走，可进版本库
 * 按 agent 的工作目录取，不按 core 的 cwd——core 是全机单实例，它的 cwd 与任何 agent 无关。
 *
 * 扫盘在 agent 创建时发生，一次，清单挂在 Agent 上：与 workdir 同期确定、同期不变。
 * 不在 build_dialog 里扫——那会让一趟中间 system prompt 变形，prompt cache 当场碎掉。
 */
#pragma once

#include <string>
#include <vector>

namespace realagent {

/* 一个 skill 在 core 眼里就这三个字段。正文 core 不读。 */
struct Skill {
    std::string name;        // = 目录名
    std::string description; // frontmatter 里的 description
    std::string path;        // SKILL.md 的绝对路径，原样交给模型
};

/* 扫两处目录，按名字排序返回。目录不存在不是错（多数人一个 skill 都没有）。
 * 坏 skill（frontmatter 解析失败、没有 description）不进清单，错误原文进 stderr——
 * 挑的是 models.json 那条先例（报错但不拒绝启动），不是 settings.json 那条（硬错退出）。 */
std::vector<Skill> scan_skills(const std::string &workdir);

/* 拼进 system prompt 的那一段。**清单为空时返回空串**：没有 skill 的 agent，
 * 它的 system prompt 与加这个功能之前一个字不差。 */
std::string skills_prompt(const std::vector<Skill> &skills);

} // namespace realagent
