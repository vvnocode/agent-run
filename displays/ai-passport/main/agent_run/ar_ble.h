// main/agent_run/ar_ble.h —— 蓝牙链路(NimBLE 外设):接收主机状态帧,处理配对确认。
//
// GATT 服务沿用 Nordic UART 的 UUID:RX(6e400002)由主机写入 v1 协议字节流,
// TX(6e400003)用通知回复 HELLO。见无线方案 §4。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 蓝牙状态,供界面显示
typedef struct {
    bool connected;      // 有主机连着
    bool trusted;        // 连着的主机已确认,数据会被采纳
    bool bonded;         // 已有确认过的绑定主机
    bool confirming;     // 正在等用户在设备上确认配对
    char name[16];       // 广播名,如 AgentRun-XXXX(X 为蓝牙 MAC 末两字节)
} ar_ble_status_t;

// 初始化 NimBLE 并开始广播。需在 NVS 初始化之后调用。
esp_err_t ar_ble_start(void);

void ar_ble_get_status(ar_ble_status_t *out);

// 待确认时的按键:ok 为 true 接受,false 拒绝
void ar_ble_pair_key(bool ok);

// 周期调用(主循环),处理确认超时
void ar_ble_tick(void);

// 清除全部绑定并断开当前连接,之后可以配对新的电脑
void ar_ble_clear_bonds(void);
