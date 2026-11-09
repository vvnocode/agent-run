// main/agent_run/ar_model.h —— Agent Run! 画面选择与交互规则(纯 C,可宿主测试)。
//
// 把“收到的状态 + 时间”换算成要显示的画面、动画帧间隔、速度条格数、
// 通知与提示音、音量、亮度、设置菜单与背光。规则见设计方案 §7.2、§7.5 与无线方案 §4.4、§7.4。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ar_proto.h"

// 超过这么久没收到合法状态帧,该来源视为离线。蓝牙下主机心跳更稀(5 秒),超时相应放宽
#define AR_LINK_TIMEOUT_USB_MS 10000u
#define AR_LINK_TIMEOUT_BLE_MS 15000u

// 低亮度的背光百分比(设定亮度更低时用设定值)
#define AR_DIM_PERCENT 20

// 插着 USB 主机时:最近一次亮屏事件后多久降到低亮度、多久熄屏(保护屏幕与背光)
#define AR_USB_DIM_AFTER_MS 300000u
#define AR_USB_OFF_AFTER_MS 1800000u

// 电池供电时:亮屏期间一直用低亮度,最近一次亮屏事件后多久熄屏
#define AR_BATTERY_OFF_AFTER_MS 30000u

// 两次“等你处理”提示音的最小间隔
#define AR_CHIME_MIN_INTERVAL_MS 30000u

// 完成提示音与上一次任意提示音的最小间隔,其间的完成合并、不补响
#define AR_DONE_CHIME_MIN_INTERVAL_MS 10000u

// 画面上“完成 +N”提示的显示时长
#define AR_DONE_FLASH_MS 5000u

// 设置菜单无按键多久回首页,最后一次按键起算:一级菜单与二级调节各自计时
#define AR_MENU_LIST_TIMEOUT_MS 10000u
#define AR_MENU_ADJUST_TIMEOUT_MS 3000u

// “未连接”且无亮屏事件持续这么久后关背光(插着 USB 主机时也生效)
#define AR_BACKLIGHT_IDLE_MS 300000u

// 电池供电时,“未连接”且无按键持续这么久就进入深睡(晚于关背光)
#define AR_DEEP_SLEEP_AFTER_MS 1800000u

// 电池供电时,屏幕持续熄灭这么久就关机(进入深睡),在线时也一样
#define AR_POWER_OFF_AFTER_DARK_MS 3600000u

// 同一次按键手势的松手与下一次按下的最大间隔:按键组件的连击判定窗口为 180 ms
// (见 bsp_pins.h 的 BSP_BTN_SHORT_PRESS_MS),再留出轮询与防抖的余量
#define AR_KEY_GESTURE_GAP_MS 250u

// 亮度档位
#define AR_BRIGHTNESS_LEVELS 5
#define AR_BRIGHTNESS_DEFAULT 3

// 音量档位:0 静音、1 低、2 中、3 高;中档与改版前的固定音量相同
#define AR_VOLUME_LEVELS 4
#define AR_VOLUME_DEFAULT 2

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

// 状态帧带来的通知:等你处理增加、有会话完成。每收到一帧新状态调用一次 ar_notice_update()。
typedef struct {
    uint8_t wn;          // 上一帧的等你处理数;掉线不清零,恢复后仍未处理的不重复提醒
    bool has_done;       // 本次在线期间是否已有完成数基准
    uint16_t done;       // 上一帧的今日完成数
} ar_notice_t;

typedef struct {
    bool waiting;        // 等你处理的数量增加了
    uint16_t completed;  // 新完成的轮数,0 表示没有
} ar_notice_ev_t;

void ar_notice_init(ar_notice_t *n);

// 链路离线时调用:恢复后的第一帧完成数只作基准,离线期间累计的完成不再提示
void ar_notice_offline(ar_notice_t *n);

// 与上一帧比较,得出本帧的通知。完成数变小(跨天清零)只更新基准。
ar_notice_ev_t ar_notice_update(ar_notice_t *n, uint8_t wn, uint16_t done);

// 提示音种类
typedef enum {
    AR_CHIME_NONE = 0,
    AR_CHIME_WAITING,   // 等你处理
    AR_CHIME_DONE,      // 有会话完成
} ar_chime_kind_t;

// 提示音节流状态
typedef struct {
    bool has_waiting;
    uint32_t last_waiting_ms;   // 上一次“等你处理”提示音
    bool has_any;
    uint32_t last_any_ms;       // 上一次任意提示音
} ar_chime_t;

void ar_chime_init(ar_chime_t *c);

// 按本帧的通知选择提示音,now_ms 允许回绕:
// - 等你处理优先,两次至少间隔 AR_CHIME_MIN_INTERVAL_MS;
// - 完成与上一次任意提示音至少间隔 AR_DONE_CHIME_MIN_INTERVAL_MS,其间的完成合并、不补响;
// - 静音时不响,也不计入间隔。
ar_chime_kind_t ar_chime_pick(ar_chime_t *c, ar_notice_ev_t ev, uint32_t now_ms, bool muted);

// 音量档位 → 播放音量百分比;越界按最高档
uint8_t ar_volume_percent(uint8_t level);

// 加减音量:到静音与最高档时停住;越界先按最高档处理
uint8_t ar_volume_step(uint8_t level, int delta);

// 亮度档位 → 背光百分比;越界按最高档
uint8_t ar_brightness_percent(uint8_t level);

// 加减亮度:到最低档与最高档时停住;越界先按最高档处理
uint8_t ar_brightness_step(uint8_t level, int delta);

// 设置菜单的层级
typedef enum {
    AR_MENU_CLOSED = 0,   // 首页,菜单未打开
    AR_MENU_LIST,         // 一级菜单:选择设置项
    AR_MENU_ADJUST,       // 二级:调节选中项
} ar_menu_level_t;

// 一级菜单的设置项,按显示顺序
typedef enum {
    AR_MENU_VOLUME = 0,
    AR_MENU_BRIGHTNESS,
    AR_MENU_ITEMS,
} ar_menu_item_t;

// 菜单用到的按键:上、下、单击确定、双击确定(返回上一级)
typedef enum {
    AR_KEY_UP = 0,
    AR_KEY_DOWN,
    AR_KEY_OK,
    AR_KEY_BACK,
} ar_key_t;

typedef struct {
    uint8_t level;   // ar_menu_level_t
    uint8_t item;    // ar_menu_item_t:一级菜单的选中项,二级时为正在调节的项
} ar_menu_t;

void ar_menu_init(ar_menu_t *m);

// 处理一次按键,返回二级里对选中项的调节量(上键 +1、下键 -1),其余情况返回 0:
// - 首页:单击确定打开一级菜单并选中音量,其他键不起作用;
// - 一级:上下键切换选中项(到头停住),单击确定进入二级,双击确定回首页;
// - 二级:上下键调节,单击确定不起作用,双击确定回一级并保留选中项。
int ar_menu_key(ar_menu_t *m, ar_key_t key);

// 距最近一次按键已满所在层的时限(一级 AR_MENU_LIST_TIMEOUT_MS、二级
// AR_MENU_ADJUST_TIMEOUT_MS)时关闭菜单回首页;二级超时也直接回首页
void ar_menu_tick(ar_menu_t *m, uint32_t ms_since_key);

// 背光计算的输入
typedef struct {
    ar_screen_t screen;
    bool on_usb_power;          // 接着 USB 主机
    uint8_t level;              // 用户设定的亮度档位
    uint32_t ms_since_wake;     // 距最近一次亮屏事件(按键、通知、连上电脑、配对确认、开机),从未发生传 UINT32_MAX
    uint32_t ms_disconnected;   // 已处于“未连接”的时长,在线时为 0
} ar_backlight_in_t;

// 熄屏时按键亮屏。按键组件只在单击、双击、长按时发出动作事件,三连击、双击第二下按住
// 超过 180 ms 等手势不发任何动作事件,所以亮屏以“按下”为准;亮屏的那次手势随后产生的
// 动作要吞掉,不执行菜单等功能。
typedef struct {
    bool swallow;               // 正在吞掉亮屏那次手势的动作
    bool has_release;           // 是否记录过松手
    uint32_t last_release_ms;   // 最近一次松手的时刻
} ar_keywake_t;

void ar_keywake_init(ar_keywake_t *k);

// 任意键按下时调用;是否亮屏由调用方处理(任何按下都算亮屏事件)。dark 为 true 表示按下时
// 屏幕熄灭,这次手势的动作将被吞掉。与上次松手相隔不少于 AR_KEY_GESTURE_GAP_MS 时视为新手势,
// 上一手势没有产生动作(如三连击)时留下的吞键状态随之清除。now_ms 允许回绕
void ar_keywake_press(ar_keywake_t *k, bool dark, uint32_t now_ms);

// 任意键松手时调用
void ar_keywake_release(ar_keywake_t *k, uint32_t now_ms);

// 单击、双击、长按到达时调用:返回 true 表示交给菜单等功能处理,
// false 表示它属于亮屏那次手势,应当忽略
bool ar_keywake_action(ar_keywake_t *k);

// 供电方式跟踪。插着 USB 主机与电池供电的熄屏时限不同(30 分钟与 30 秒),
// 拔线瞬间若仍按最近一次亮屏事件计时,亮着的屏幕会立刻熄灭
typedef struct {
    bool known;    // 是否已记录过供电方式
    bool on_usb;   // 上一次的供电方式
} ar_power_t;

void ar_power_init(ar_power_t *p);

// 每个周期调用一次。返回 true 表示调用方应记一次亮屏事件:从 USB 主机供电变为电池供电,
// 且此刻屏幕亮着。屏幕已熄灭时(包括 Mac 睡眠导致 USB 主机离线)不点亮;插上 USB 不在这里点亮,
// 连上电脑另有亮屏事件。第一次调用只记录基准
bool ar_power_update(ar_power_t *p, bool on_usb, bool screen_on);

// 背光百分比,0 表示熄屏。“低亮度”为 AR_DIM_PERCENT 与设定亮度中较低者。
// - 电池供电:亮屏事件后 AR_BATTERY_OFF_AFTER_MS 内用低亮度,之后熄屏;
// - 插着 USB 主机且“未连接”:未连接与无亮屏事件都满 AR_BACKLIGHT_IDLE_MS 时熄屏;
// - 插着 USB 主机:亮屏事件后 AR_USB_DIM_AFTER_MS 内用设定亮度,
//   AR_USB_OFF_AFTER_MS 内用低亮度,之后熄屏。
uint8_t ar_model_backlight_percent(const ar_backlight_in_t *in);

// 深睡判定的输入
typedef struct {
    bool on_usb_power;          // 接着 USB 主机(只认主机在线,充电宝或主机睡眠时为 false)
    bool battery_present;       // 电量计读得到电量;读不到时可能没装电池,只靠 USB 供电
    bool confirming;            // 正在等用户确认配对
    uint32_t ms_disconnected;   // 已处于“未连接”的时长,在线时为 0
    uint32_t ms_since_key;      // 距最近一次按键,从未按过传 UINT32_MAX
    uint32_t ms_dark;           // 屏幕已持续熄灭的时长,亮着时为 0
} ar_sleep_in_t;

// 是否该进入深睡(关机,按任意键唤醒)。前提是没接 USB 主机、电量计正常、不在配对确认,
// 满足其一即睡:
// - “未连接”与无按键都已满 AR_DEEP_SLEEP_AFTER_MS;
// - 屏幕持续熄灭已满 AR_POWER_OFF_AFTER_DARK_MS。在线时也适用:等你处理、有会话完成
//   都会点亮屏幕并重新计时,所以只有长时间没有任何通知和按键才会关机。
bool ar_model_should_deep_sleep(const ar_sleep_in_t *in);
