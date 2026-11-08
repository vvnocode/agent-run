// main/agent_run/ar_model.h —— Agent Run! 画面选择与交互规则(纯 C,可宿主测试)。
//
// 把“收到的状态 + 时间”换算成要显示的画面、动画帧间隔、速度条格数、
// 提示音是否该响、亮度与背光。规则见设计方案 §7.2、§7.5。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ar_proto.h"

// 超过这么久没收到合法状态帧,该来源视为离线。蓝牙下主机心跳更稀(5 秒),超时相应放宽
#define AR_LINK_TIMEOUT_USB_MS 10000u
#define AR_LINK_TIMEOUT_BLE_MS 15000u

// 电池供电时,状态这么久没变化就把背光降到 AR_DIM_PERCENT
#define AR_DIM_AFTER_MS 30000u
#define AR_DIM_PERCENT 20

// 两次提示音的最小间隔
#define AR_CHIME_MIN_INTERVAL_MS 30000u

// 进入“未连接”或最后一次按键后,背光保持的时间
#define AR_BACKLIGHT_IDLE_MS 300000u

// 亮度档位
#define AR_BRIGHTNESS_LEVELS 5
#define AR_BRIGHTNESS_DEFAULT 3

typedef enum {
    AR_SCREEN_DISCONNECTED = 0,   // 未连接电脑
    AR_SCREEN_WAITING,            // 等你处理(优先于跑速)
    AR_SCREEN_SLEEP,              // 0 个工作中
    AR_SCREEN_WALK,               // 1
    AR_SCREEN_TROT,               // 2-3
    AR_SCREEN_RUN,                // 4-6
    AR_SCREEN_DASH,               // >= 7
} ar_screen_t;

// 三个工具“工作中”的合计
unsigned ar_model_working_total(const ar_state_t *s);

// 链路是否在线:USB 或蓝牙任一来源在各自的超时内收到过合法帧。
// 参数为距该来源最近一帧的毫秒数,从未收到时传 UINT32_MAX。
bool ar_model_link_alive(uint32_t usb_since_ms, uint32_t ble_since_ms);

// 选择画面;链路不在线时显示“未连接”。
ar_screen_t ar_model_screen(const ar_state_t *s, bool alive);

// 每种画面的动画帧间隔:跑得越快换帧越快
uint16_t ar_model_frame_interval_ms(ar_screen_t screen);

// 速度条点亮的格数(0-4)
uint8_t ar_model_speed_bars(ar_screen_t screen);

// 提示音状态:等你处理的数量增加时响一次,两次至少间隔 30 秒,静音时不响
typedef struct {
    uint8_t last_wn;
    bool has_chimed;
    uint32_t last_chime_ms;
} ar_chime_t;

void ar_chime_init(ar_chime_t *c);

// 每收到一帧调用一次;返回 true 表示现在应播放提示音。now_ms 允许回绕。
bool ar_chime_update(ar_chime_t *c, uint8_t wn, uint32_t now_ms, bool muted);

// 亮度档位 → 背光百分比;越界按最高档
uint8_t ar_brightness_percent(uint8_t level);

// 调整亮度档位并限制在合法范围
uint8_t ar_brightness_step(uint8_t level, int delta);

// 背光计算的输入
typedef struct {
    ar_screen_t screen;
    bool on_usb_power;          // 接着 USB 主机(此时不省电)
    uint8_t level;              // 用户设定的亮度档位
    uint32_t ms_since_change;   // 距画面内容最近一次变化
    uint32_t ms_since_key;      // 距最近一次按键,从未按过传 UINT32_MAX
    uint32_t ms_disconnected;   // 已处于“未连接”的时长,在线时为 0
} ar_backlight_in_t;

// 背光百分比,0 表示关闭:
// - “未连接”超过 5 分钟且 5 分钟内无按键时关闭;
// - 电池供电、不在“等你处理”、30 秒内无按键且内容 30 秒没变化时降到 AR_DIM_PERCENT;
// - 其余情况用设定亮度。
uint8_t ar_model_backlight_percent(const ar_backlight_in_t *in);
