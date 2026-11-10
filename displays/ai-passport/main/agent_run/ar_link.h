// main/agent_run/ar_link.h —— 主机链路:USB 串口接收,并汇总 USB 与蓝牙两路收到的状态帧与 A 帧。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ar_proto.h"
#include "esp_err.h"

// 固件版本,随 HELLO 回报给主机
#define AR_FW_VERSION "0.5.0"

// 状态帧来源
typedef enum {
    AR_SRC_NONE = 0,
    AR_SRC_USB,
    AR_SRC_BLE,
} ar_source_t;

// 链路状态快照
typedef struct {
    ar_state_t state;          // 两路中最近到达的一帧合法状态
    bool has_state;            // 开机后是否收到过合法状态
    ar_source_t last_source;   // 最近一帧的来源
    int64_t last_usb_us;       // 最近一帧 USB 状态的到达时间(esp_timer 微秒),0 表示从未收到
    int64_t last_ble_us;       // 最近一帧蓝牙状态的到达时间,0 表示从未收到
    ar_agent_t agent[AR_AGENT_TOOLS];   // 每个工具最近一帧 A 帧,下标为 ar_tool_t(CC、CX)
    bool has_agent[AR_AGENT_TOOLS];     // 开机后是否收到过该工具的 A 帧
} ar_link_snapshot_t;

// 安装 USB-Serial-JTAG 驱动、启动接收任务,并主动发送一次 HELLO
esp_err_t ar_link_start(void);

// 记录一帧来自 src 的合法状态(线程安全;蓝牙模块在 NimBLE 任务里调用)
void ar_link_publish(const ar_state_t *st, ar_source_t src);

// 记录一帧 A 帧(线程安全)。A 帧只在内容变化时发送,不计入链路是否在线的判断
void ar_link_publish_agent(const ar_agent_t *agent);

// 取当前链路状态(线程安全)
void ar_link_get(ar_link_snapshot_t *out);

// HELLO 行的内容(不含行尾),供 USB 与蓝牙共用
const char *ar_link_hello_line(void);
