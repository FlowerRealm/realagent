/*
 * agent_caps.h — realagent 的插件词汇表（C ABI）
 *
 * realugin 只给机制：容器、能力表、依赖图、宿主 API（<realugin/plugin_api.h>）。
 * 它不认识"工具"、不认识"模型"、不认识"权限"。这个文件就是 realagent 补上的那半边：
 * **能力键叫什么、各自是什么签名**。
 *
 * 加一个能力 = 这里加一个键加一个 typedef。realugin 不动、ABI 不变、
 * 已有容器不重编（能力表里没这个键，core 查不到即跳过）。
 *
 * 设计要点（ADR-0011 / ADR-0012 / ADR-0013）：
 *  - **一个能力 = 一个名字 + 一个函数**，零例外
 *  - **管线**：一次 LLM 调用由 core 拆成前后相接的几段，插件互不认识、互不调用
 *
 *      对话 ──request.build──▶ 粗请求 ──request.refine──▶ 精请求 ──[core 发出]──▶ 响应流
 *                                                                                │
 *             事件 ◀──usage.meter── usage 事件 ◀──response.parse──────────────────┘
 *
 *  - 跨边界内存两条规则（realugin 定的，这里照用）：
 *      **借阅**：插件长期持有的（清单、静态表）→ const 指针，有效期 = 容器在位时长
 *      **转移**：本次调用现造的（请求 JSON、执行结果）→ 经 host_api->alloc 分配，core 释放
 */
#ifndef REALAGENT_AGENT_CAPS_H
#define REALAGENT_AGENT_CAPS_H

#include <realugin/plugin_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 能力键 ==================== */

/* 管线：对话 → 粗请求 → 精请求 →（core 发出）→ 响应流 → 事件 → 计价 */
#define REALAGENT_CAP_REQUEST_BUILD   "request.build"    /* 独占：一次调用只有一个请求 */
#define REALAGENT_CAP_REQUEST_REFINE  "request.refine"   /* 独占：由配置 provider 指名 */
#define REALAGENT_CAP_RESPONSE_PARSE  "response.parse"   /* 独占 */
#define REALAGENT_CAP_USAGE_METER     "usage.meter"      /* 独占：随 provider 一起 */
/* 工具与命令：可合并，多个容器各报各的，core 按对外名字寻址 */
#define REALAGENT_CAP_TOOL_LIST       "tool.list"
#define REALAGENT_CAP_TOOL_EXECUTE    "tool.execute"
#define REALAGENT_CAP_TOOL_INTERRUPT  "tool.interrupt" /* 可选：不具备即工具不可中断 */
#define REALAGENT_CAP_COMMAND_LIST    "command.list"
#define REALAGENT_CAP_COMMAND_EXECUTE "command.execute"
/* 其余 */
#define REALAGENT_CAP_PERMISSION      "permission.decide" /* 独占：一个裁决者 */
#define REALAGENT_CAP_MODEL_LIST      "model.list"        /* core 只问当前 provider */
/* 事件旁听。签名是 realugin_broadcast_fn —— realugin 唯一会自己调的那个，
 * 键名由 CoreHost::broadcast_capability() 报给它。 */
#define REALAGENT_CAP_EVENT_OBSERVE   "event.observe"

/* ==================== 数据结构 ==================== */

/* 权限裁决（ADR-0005：core 永远是发起方）。
 * 槽位独占：只有一个裁决者，ALLOW 就是字面意义的放行。 */
typedef enum {
    REALAGENT_PERM_ALLOW = 0, /* 放行 */
    REALAGENT_PERM_DENY  = 1, /* 拒绝 */
    REALAGENT_PERM_ASK   = 2  /* 需要用户裁决：core 向客户端发询问 */
} realagent_permission_t;

/* 事件接收器：插件解析出事件时同步回调（payload 仅在回调内有效） */
typedef void (*realagent_event_sink_t)(void* sink_ctx, const char* type, const char* payload);

/* 工具定义（借阅：core 只读，寿命 = 容器在位时长）。
 * name 是插件侧本名；对外名字由 core 加命名空间前缀，执行时传回的仍是本名。 */
typedef struct {
    const char* name;
    const char* label;       /* UI 显示名 */
    const char* description; /* 发给 LLM 的描述 */
    const char* parameters;  /* 参数 JSON Schema 字符串 */
    int dangerous;           /* 1 = 执行前触发权限检查点 */
} realagent_tool_t;

/* 斜杠命令定义（借阅，同上）。无 handler 字段——执行走 command.execute 按名分发。 */
typedef struct {
    const char* name;        /* 本名，不带 '/' */
    const char* description;
} realagent_command_t;

/* 执行结果（工具与命令共用）。
 * messages 经 host_api->alloc 分配，**core 释放**（转移）。 */
typedef struct {
    int status;           /* 0 = 成功，非 0 = 错误 */
    const char* messages; /* JSON（数组或字符串） */
} realagent_result_t;

/* ==================== 各能力的签名（转型的唯一依据） ==================== */

/* request.build：对话 → 粗请求 JSON
 *   入参 dialog_json：{model, system, messages, tools}
 *   返回 {"url":..., "headers":{...}, "body":{...}}，**转移**（host_api->alloc，core 释放）
 *   粗请求允许留空处：url 可以只有路径、body.model 可以是空串、headers 可以没有凭证——
 *   那些由 request.refine 补。失败返回 NULL。 */
typedef const char* (*realagent_request_build_fn)(realugin_plugin_t*, const char* dialog_json);

/* request.refine：粗请求 JSON → 精请求 JSON
 *   收一个请求、还一个请求，**不知道也不关心请求是谁生成的**。
 *   返回值**转移**；失败返回 NULL（core 视为本次调用失败，不会拿粗请求硬发）。 */
typedef const char* (*realagent_request_refine_fn)(realugin_plugin_t*, const char* request_json);

/* response.parse：响应流分片 → 事件
 *   chunk == NULL 表示流结束（flush）。解析出事件即同步调 sink。 */
typedef realugin_status_t (*realagent_response_parse_fn)(realugin_plugin_t*, const char* chunk,
                                                         realagent_event_sink_t sink,
                                                         void* sink_ctx);

/* usage.meter：token 用量 → 钱（USD）
 *   core 收到 parse 产出的 usage 事件后调它，把结果作为 cost 下发，**usage 事件本身不再上传**
 *   （ADR-0009：core 不认识 token）。算不出返回 0（不发 cost，不发 0）。
 *   本次用的是哪个模型，由提供方自己在 request.refine 时记下——core 不传，也不知道。 */
typedef double (*realagent_usage_meter_fn)(realugin_plugin_t*, const char* usage_json);

/* tool.list：交出工具清单。返回条数，*out 指向插件自有数组（**借阅**） */
typedef size_t (*realagent_tool_list_fn)(realugin_plugin_t*, const realagent_tool_t** out);

/* tool.execute：按本名执行。结果 messages **转移** */
typedef realagent_result_t (*realagent_tool_execute_fn)(realugin_plugin_t*, const char* call_id,
                                                        const char* tool_name,
                                                        const char* params_json);

/* tool.interrupt：中止**正在执行**的那次工具调用（call_id 即 tool.execute 收到的那个）。
 * 不具备此能力 = 该容器的工具不可中断，core 只好等它自己跑完。
 *
 * 线程契约：由 core 从**另一条线**调进来（tool.execute 还没返回，agent 线程正卡在里面），
 * 这是唯一一处并发进入插件的地方。实现必须只做"捅一下"——给子进程发个信号、关个 fd——
 * 然后立刻返回；在这里等子进程死掉就是把事件循环也一起卡住。
 * 真正的收尾（reap、拼结果）留在 tool.execute 那条线上做。
 *
 * 中止是**请求，不是承诺**：怎么判定"这次调用算被中断了"由 core 说了算（是 core 提的），
 * 插件照常返回它手上有的东西即可，不必编造特殊状态码。 */
typedef void (*realagent_tool_interrupt_fn)(realugin_plugin_t*, const char* call_id);

/* command.list / command.execute：与工具同形 */
typedef size_t (*realagent_command_list_fn)(realugin_plugin_t*, const realagent_command_t** out);
typedef realagent_result_t (*realagent_command_execute_fn)(realugin_plugin_t*,
                                                           const char* command_name,
                                                           const char* args_json);

/* permission.decide：危险工具执行前的裁决 */
typedef realagent_permission_t (*realagent_permission_fn)(realugin_plugin_t*,
                                                          const char* tool_name,
                                                          const char* params_json);

/* model.list：模型清单 JSON 数组 [{name, owned_by, context}]（**借阅**）。
 * 单价不报——core 不算钱。无清单返回 NULL。 */
typedef const char* (*realagent_model_list_fn)(realugin_plugin_t*);

/* event.observe 的签名就是 realugin_broadcast_fn：
 *   void (*)(realugin_plugin_t*, const char* type, const char* payload)
 * 可合并能力：所有订阅者都收到同一份事件，顺序 = 加载序。**旁听，不是拦截**——
 * 无返回值，订阅者改不了事件、也拦不下事件。线程与寿命契约见 realugin/plugin_api.h。 */

#ifdef __cplusplus
}
#endif

#endif /* REALAGENT_AGENT_CAPS_H */
