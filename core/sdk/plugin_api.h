/*
 * plugin_api.h — realagent 插件 SDK（C ABI）
 *
 * 插件与 core 的唯一边界。纯 C、自包含、无 STL。
 * 所有跨边界数据以 const char*（JSON）传递。
 *
 * 设计要点（ADR-0001）：
 *  - C ABI 动态库（.so/.dylib），单一接口版本号强校验
 *  - 插件元数据独立 plugin.json（名称/描述/版本/ABI/前置依赖/type）
 *  - 事件订阅：单入口 on_event，插件内按 type 字符串分发
 *  - 工具参数 Schema：JSON 字符串
 *  - 插件不感知客户端（TUI/gui），core 是中枢
 *
 * ABI 版本：1
 */
#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUGIN_ABI_VERSION 1

/* 导出宏：插件库须导出 plugin_create（Windows 用 dllexport，macOS/Linux 用 visibility） */
#if defined(_WIN32)
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ==================== 基础类型 ==================== */

/* 插件类型：决定生命周期接口表（ADR-0001） */
typedef enum {
    PLUGIN_TYPE_PROTOCOL   = 1, /* Provider 链协议层：请求构造 + 响应解析（成对，ADR-0004） */
    PLUGIN_TYPE_TOOL       = 2, /* 工具插件：注册一个或多个工具 */
    PLUGIN_TYPE_PERMISSION = 3, /* 权限策略：挂 core 检查点（ADR-0005） */
    PLUGIN_TYPE_SESSION    = 4  /* 会话管理：注册会话命令（/new /resume） */
} plugin_type_t;

typedef enum {
    PLUGIN_OK  = 0,
    PLUGIN_ERR = -1
} plugin_status_t;

/* 权限裁决（ADR-0005：core 永远是发起方） */
typedef enum {
    PLUGIN_PERM_ALLOW = 0, /* 放行 */
    PLUGIN_PERM_DENY  = 1, /* 拒绝 */
    PLUGIN_PERM_ASK   = 2  /* 需要用户裁决：core 向客户端（TUI/gui）发询问 */
} plugin_permission_t;

/* 日志级别 */
enum {
    PLUGIN_LOG_TRACE = 0,
    PLUGIN_LOG_DEBUG = 1,
    PLUGIN_LOG_INFO  = 2,
    PLUGIN_LOG_WARN  = 3,
    PLUGIN_LOG_ERROR = 4
};

/* ==================== 不透明句柄 ==================== */

typedef struct plugin_core   plugin_core_t;   /* core 实例（core 分配，插件只持有） */
typedef struct plugin_plugin plugin_t;        /* 插件实例（插件分配） */

/* ==================== 事件 ==================== */

/* core → 插件（on_event）。type 为字符串事件名，payload 为 JSON。
 * 事件类型对齐 PROTOCOL.md 推送流帧：agent_start / turn_start / message_start /
 * message_update / message_end / tool_execution_start / tool_execution_end /
 * tool_output / turn_end / agent_end / permission_request ... */
typedef struct {
    const char* type;    /* 事件类型（字符串，非枚举——新增不破坏 ABI） */
    const char* payload; /* JSON 载荷 */
} plugin_event_t;

/* ==================== 工具 ==================== */

/* 工具定义（插件注册用）。parameters 为 JSON Schema 字符串（宏 + 字面量）。 */
typedef struct {
    const char* name;        /* 工具名（LLM 调用用） */
    const char* label;       /* UI 显示名 */
    const char* description; /* 发给 LLM 的描述 */
    const char* parameters;  /* 参数 JSON Schema 字符串 */
    int dangerous;           /* 1 = 执行前触发权限检查点 */
} plugin_tool_t;

/* 工具执行结果：status + messages（JSON）。错误通过 status != 0 传达。 */
typedef struct {
    int status;           /* 0 = 成功，非 0 = 错误 */
    const char* messages; /* JSON（数组或字符串） */
} plugin_tool_result_t;

/* ==================== 命令（斜杠命令，通用注册能力） ==================== */

typedef struct {
    const char* name;        /* 斜杠命令名，不带 '/' */
    const char* description;
    plugin_status_t (*handler)(plugin_t*, plugin_core_t*, const char* args_json);
} plugin_command_t;

/* ==================== 协议（type = PLUGIN_TYPE_PROTOCOL） ==================== */

/* 事件接收器：协议插件解析出事件时同步回调（payload 仅在回调内有效） */
typedef void (*plugin_event_sink_t)(void* sink_ctx, const char* type, const char* payload);

/* 构造的请求（插件分配内存，core 用完调 api->free 释放） */
typedef struct {
    const char* url;  /* 完整请求 URL（含 base_url + 路径） */
    const char* body; /* 请求体 JSON */
} plugin_request_t;

/* ==================== core → 插件 API ==================== */

/* core 提供给插件的服务（插件在 init 中持有 plugin_core_t 即可用） */
typedef struct {
    /* 注册工具（tool 插件在 init 中调用） */
    plugin_status_t (*register_tool)(plugin_core_t*, const plugin_tool_t*);
    /* 注册斜杠命令（通用能力，不限 type） */
    plugin_status_t (*register_command)(plugin_core_t*, const plugin_command_t*);
    /* 广播事件到客户端（TUI/gui 推送流） */
    plugin_status_t (*emit)(plugin_core_t*, const char* type, const char* payload_json);
    /* 日志 */
    void (*log)(plugin_core_t*, int level, const char* msg);
    /* 读取配置项（core 已按 env > settings.json > 默认合并注入） */
    const char* (*get_config)(plugin_core_t*, const char* key);
    /* 声明前置插件依赖（嵌套组装，ADR-0004）：name 为被包裹插件的插件名 */
    plugin_status_t (*depends_on)(plugin_core_t*, const char* plugin_name);
} plugin_core_api_t;

struct plugin_core {
    const plugin_core_api_t* api;
    void* ctx; /* core 内部状态（插件不得触碰） */
};

/* ==================== 插件导出的接口表 ==================== */

typedef struct {
    int abi_version;  /* 必须 == PLUGIN_ABI_VERSION，core 加载时强校验 */
    plugin_type_t type;
    const char* name; /* 插件名，须与 plugin.json 一致 */

    /* —— 生命周期（所有插件） —— */
    /* 初始化：core 已注入配置（可经 core->api->get_config 读取），
     * 此时应注册工具/命令、声明前置依赖 */
    plugin_status_t (*init)(plugin_t*, plugin_core_t*);
    void (*destroy)(plugin_t*);
    /* 事件分发：单入口，插件内按 event->type 字符串分发（ADR-0001） */
    void (*on_event)(plugin_t*, const plugin_event_t*);
    /* 释放插件分配的内存（core 用于释放 parse_feed/build_request 产物） */
    void (*free)(plugin_t*, void*);

    /* —— type = PLUGIN_TYPE_TOOL —— */
    plugin_tool_result_t (*execute_tool)(plugin_t*, const char* call_id,
                                         const char* tool_name, const char* params_json);

    /* —— type = PLUGIN_TYPE_PERMISSION —— */
    plugin_permission_t (*decide)(plugin_t*, const char* tool_name,
                                  const char* params_json);

    /* —— type = PLUGIN_TYPE_PROTOCOL —— */
    /* 构造请求：dialog_json 为对话上下文（messages/tools/system/model），
     * 出参 request（内存由插件分配，core 用完调 free 释放） */
    plugin_status_t (*build_request)(plugin_t*, const char* dialog_json,
                                     plugin_request_t* out);
    /* 解析响应流：chunk 为一段 SSE/HTTP 响应体；解析出事件则同步调 sink。
     * chunk == NULL 表示流结束（flush）。 */
    plugin_status_t (*parse_feed)(plugin_t*, const char* chunk,
                                  plugin_event_sink_t sink, void* sink_ctx);

    /* —— type = PLUGIN_TYPE_SESSION —— */
    /* 会话操作由 register_command 注册，命令处理走 command->handler */
} plugin_api_t;

/* 插件库必须导出的入口：创建插件实例，出参接口表 */
typedef plugin_t* (*plugin_create_fn)(const plugin_api_t** out_api);

/* 导出符号名（插件实现须按此导出） */
#define PLUGIN_CREATE_SYM "plugin_create"

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_API_H */
