/*
 * history.hpp — 会话历史 → 事件帧（ADR-0020）
 *
 * TUI 是独立进程、可能还在另一台机器上，读不到 JSONL；而它要能翻一个自己没在看的
 * agent 那段时间里干了什么。所以 core 给它一段回放。
 *
 * **回放的是事件帧，不是抽象对话消息。** 于是客户端复用同一个渲染器、一份代码。
 * 返回消息形状看起来「core 侧零转换」，但那句省事是假的：转换没消失，只是从 core
 * 挪到客户端，而且挪成了两份必须逐像素一致的渲染代码——同一段对话，实时看和翻历史看
 * 长得不一样，是那种没人报 bug 但一直硌人的东西。转换在这里，一处，可测。
 */
#pragma once

#include "json.hpp"

namespace realagent {

/* 一段消息历史 → 事件帧序列 `[{"type": ..., "data": {...}}]`。
 *
 * 帧的形状与实时推送流逐字相同（PROTOCOL.md），这是这个函数存在的全部理由。
 *
 * 三种帧回放不出来，它们不在历史里：
 *   - `status_update`（花费）—— 按 run 累计，不随消息落盘
 *   - `agent_start` / `agent_end` —— 生命周期标记，不渲染任何内容，缺了不影响画面
 *   - 被中断那一刻的 `interrupted` —— 中断的痕迹留在 tool_result 的 is_error 上
 * 不为它们编造数据：回放里多一个假帧，比少一个真帧糟得多。 */
nlohmann::json history_frames(const nlohmann::json &messages);

} // namespace realagent
