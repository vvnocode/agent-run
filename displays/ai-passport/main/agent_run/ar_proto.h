// main/agent_run/ar_proto.h —— Agent Run! 串口协议解析(纯 C,可宿主测试)。
//
// 协议见 docs/specs/2026-11-08-agent-run-design.md §6(外层仓库)。主机每行发送:
//   AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q
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
