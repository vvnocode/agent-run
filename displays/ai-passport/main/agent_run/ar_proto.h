// main/agent_run/ar_proto.h —— Agent Run! 串口协议解析(纯 C,可宿主测试)。
//
// 协议见 docs/specs/2026-11-08-agent-run-design.md §6(外层仓库)。主机每行发送:
//   AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q tm=1342   tm 可选,见 -screen-dnd-design.md §8
//   AR1 A t=CX wn=0 bg=0 done=3 h=x hr=- w=5 wr=6110      工具专用页面,见 -agent-pages-design.md §5.2
//   AR1 PING
// 本模块只做“字节 → 行 → 结构体”,不依赖 ESP-IDF / LVGL。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 单行最大字节数(不含行尾);超长的行整行丢弃,与主机端 MAX_LINE 一致
#define AR_PROTO_MAX_LINE 160

// 状态帧中最多携带的等待项数;wn 字段仍是总数
#define AR_PROTO_MAX_WAIT 3

// 工具代号
typedef enum {
    AR_TOOL_CC = 0,   // Claude Code
    AR_TOOL_CX,       // Codex
    AR_TOOL_DS,       // dsh
} ar_tool_t;

// 等你处理的原因
typedef enum {
    AR_REASON_APPROVE = 0,   // A:等待授权
    AR_REASON_QUESTION,      // Q:等你回答
} ar_reason_t;

typedef struct {
    uint8_t tool;     // ar_tool_t
    uint8_t reason;   // ar_reason_t
} ar_wait_item_t;

// 有专用页面的工具数:Claude Code 与 Codex,下标即 ar_tool_t(AR_TOOL_CC、AR_TOOL_CX)
#define AR_AGENT_TOOLS 2

// A 帧里用量字段的两个特殊值:"-" 暂无数据,"x" 账户没有这一限额窗口
#define AR_USAGE_UNKNOWN 255u
#define AR_USAGE_NO_LIMIT 254u

// A 帧里距重置分钟数:"-" 为未知;数值上限为两周
#define AR_RESET_UNKNOWN 0xFFFFu
#define AR_RESET_MAX_MIN 20160u

// A 帧里套餐名(p 字段)的最大字符数,与主机端 protocol.MAX_PLAN_CHARS 一致
#define AR_PLAN_MAX 10

// 状态帧没有 tm 字段时的本地时间;tm 的取值为当日分钟数 0–1439
#define AR_TIME_UNKNOWN 0xFFFFu
#define AR_TIME_MAX_MIN 1439u

// 一个工具专用页面的数据(A 帧)
typedef struct {
    uint8_t tool;     // AR_TOOL_CC 或 AR_TOOL_CX
    uint8_t wn;       // 等你处理
    uint8_t bg;       // 后台运行
    uint16_t done;    // 今日完成
    uint8_t h;        // 5 小时窗口已用百分比 0–100,或 AR_USAGE_UNKNOWN / AR_USAGE_NO_LIMIT
    uint16_t hr;      // 5 小时窗口距重置分钟数,或 AR_RESET_UNKNOWN
    uint8_t w;        // 每周窗口已用百分比,取值同 h
    uint16_t wr;      // 每周窗口距重置分钟数,取值同 hr
    char plan[AR_PLAN_MAX + 1];   // 套餐名,如 "Pro Lite";帧里没有 p 字段时为空串
} ar_agent_t;

// 一帧完整状态
typedef struct {
    uint16_t seq;
    uint8_t cc;        // Claude Code 工作中
    uint8_t cx;        // Codex 工作中
    uint8_t ds;        // dsh 工作中
    uint16_t done;     // 今日完成
    uint8_t wn;        // 等你处理总数
    uint8_t w_count;   // w[] 中有效项数,<= AR_PROTO_MAX_WAIT 且 <= wn
    ar_wait_item_t w[AR_PROTO_MAX_WAIT];
    uint16_t tm;       // 主机本地时间的当日分钟数 0–1439;帧里没有 tm 字段时为 AR_TIME_UNKNOWN
} ar_state_t;

// 一行的解析结果
typedef enum {
    AR_LINE_IGNORED = 0,   // 非本协议的行(日志等)或格式错误的帧
    AR_LINE_STATE,         // 合法状态帧,已写入 out
    AR_LINE_PING,          // 握手请求,需回复 HELLO
} ar_line_kind_t;

// 解析一行(不含行尾)。只有返回 AR_LINE_STATE 时才写 out;任何字段缺失或越界都整帧丢弃,
// 未知键忽略(便于主机端向后兼容地加字段)。
ar_line_kind_t ar_proto_parse_line(const char *line, ar_state_t *out);

// 解析一行 A 帧(不含行尾)。合法时写 out 并返回 true;不是 A 帧、工具代号不是 CC / CX、
// 任何字段缺失或越界都返回 false 且不写 out。套餐 p 是可选字段,出现时也须合法。未知键忽略。
// ar_proto_parse_line() 不认 A 帧,
// 调用方在它返回 AR_LINE_IGNORED 后再试本函数。
bool ar_proto_parse_agent(const char *line, ar_agent_t *out);

// 行装配器:逐字节喂入串口数据,遇到 \r 或 \n 时产出一行。
typedef struct {
    char buf[AR_PROTO_MAX_LINE + 1];
    uint16_t len;
    bool overflow;   // 本行已超长,丢弃到下一个行尾
} ar_line_buf_t;

void ar_line_init(ar_line_buf_t *lb);

// 喂入一个字节。产出完整行时把它(以 NUL 结尾)拷到 out 并返回 true;
// out_size 至少为 AR_PROTO_MAX_LINE + 1。空行与超长行不产出。
bool ar_line_push(ar_line_buf_t *lb, char c, char *out, size_t out_size);
