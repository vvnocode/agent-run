// main/agent_run/ar_model.h —— Agent Run! 画面选择与交互规则(纯 C,可宿主测试)。
//
// 把“收到的状态 + 时间”换算成要显示的画面、动画帧间隔、速度条格数、
// 通知与提示音、再提醒、音量、亮度、熄屏时间、勿扰与本地时间、设置菜单与背光,
// 工具专用页面的切换与文字,以及按电池电压推断的外部供电。
// 规则见设计方案 §7.2、§7.5、无线方案 §4.4、§7.4、专用页面方案 §4、屏幕方案(-screen-dnd-design.md)
// 与供电方案(-power-detect-design.md)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ar_proto.h"

// 超过这么久没收到合法状态帧,该来源视为离线。蓝牙下主机心跳更稀(5 秒),超时相应放宽
#define AR_LINK_TIMEOUT_USB_MS 10000u
#define AR_LINK_TIMEOUT_BLE_MS 15000u

// 低亮度的背光百分比(设定亮度更低时用设定值)
#define AR_DIM_PERCENT 20

// 接着电源(USB 主机或充电器)时:最近一次亮屏事件后最晚多久降到低亮度;熄屏时间短于它的两倍时,
// 改在熄屏时间的一半调暗。熄屏时间由用户设定,见 ar_usb_off_ms()
#define AR_USB_DIM_AFTER_MS 300000u

// 熄屏时间的设定选项(存的是选项序号)。电池供电时亮屏期间一直用低亮度。
// 电池供电:10 秒、30 秒、1 分、3 分;接电源:5 分、10 分、30 分、1 小时、不熄屏。
// 默认值与改版前的固定时限相同(30 秒、30 分钟)
#define AR_BATTERY_OFF_OPTIONS 4
#define AR_BATTERY_OFF_DEFAULT 1
#define AR_USB_OFF_OPTIONS 5
#define AR_USB_OFF_DEFAULT 2
// “不熄屏”对应的时限
#define AR_OFF_NEVER UINT32_MAX

// 等你处理持续这么久没处理就再提醒一次(响一声并亮屏),每次新增等你处理后最多再提醒
// AR_REMIND_MAX 次。见 ar_remind_update()
#define AR_REMIND_AFTER_MS 180000u
#define AR_REMIND_MAX 2

// 夜间勿扰的时段(当日分钟数):22:00 起,到次日 08:00 止(不含 08:00)
#define AR_NIGHT_START_MIN (22u * 60u)
#define AR_NIGHT_END_MIN (8u * 60u)

// 两次“等你处理”提示音的最小间隔
#define AR_CHIME_MIN_INTERVAL_MS 30000u

// 完成提示音与上一次任意提示音的最小间隔,其间的完成合并、不补响
#define AR_DONE_CHIME_MIN_INTERVAL_MS 10000u

// 画面上“完成 +N”提示的显示时长
#define AR_DONE_FLASH_MS 5000u

// 设置菜单无按键多久回首页,最后一次按键起算:一级菜单与二级调节各自计时
#define AR_MENU_LIST_TIMEOUT_MS 10000u
#define AR_MENU_ADJUST_TIMEOUT_MS 3000u

// “未连接”且无亮屏事件持续这么久后关背光(接着电源时也生效)
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

// 工具专用页面(专用页面方案 §4.1)。页面顺序:首页 → Claude Code → Codex → 首页
typedef enum {
    AR_PAGE_HOME = 0,   // 首页:宠物与计数
    AR_PAGE_CC,         // Claude Code 专用页面
    AR_PAGE_CX,         // Codex 专用页面
    AR_PAGE_COUNT,
} ar_page_t;

// 用量达到这个百分比时,专用页面的进度条改用警示色
#define AR_USAGE_ALERT_PERCENT 80u

// 切页:delta 为正(下键)前进、为负(上键)后退,循环;越界的 page 按首页算
uint8_t ar_page_step(uint8_t page, int delta);

// 实际显示的页面:未连接、等你处理覆盖专用页面(显示首页的对应画面),其余时候为用户所在页面。
// 配对确认与设置菜单是浮层,由界面直接盖在上面,不在这里处理
uint8_t ar_page_shown(uint8_t page, ar_screen_t screen);

// 上下键是否用于切页:设置菜单打开、配对确认、未连接、等你处理时都不切页
bool ar_page_keys_active(ar_screen_t screen, bool menu_open, bool pairing);

// 距重置时间的文字,写入 buf(总以 NUL 结尾):不足 1 小时为“N 分钟后重置”,不足 1 天为
// “N 小时 M 分后重置”,否则为“N 天 M 小时后重置”;AR_RESET_UNKNOWN 为“重置时间未知”
void ar_reset_text(uint16_t minutes, char *buf, size_t size);

// 用量是否达到警示线;暂无数据、无此限额不算
bool ar_usage_alert(uint8_t percent);

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

// 等你处理的再提醒状态
typedef struct {
    bool active;          // 正有等你处理在计时
    uint8_t count;        // 本轮已再提醒的次数
    uint32_t since_ms;    // 上一次提醒(新增或再提醒)的时刻
} ar_remind_t;

void ar_remind_init(ar_remind_t *r);

// 每个周期调用一次,返回 true 表示该再提醒一次。wn 为当前等你处理数(链路不在线时传 0),
// fresh 为本周期是否有新增等你处理(ar_notice_ev_t.waiting),now_ms 允许回绕:
// - wn 为 0:清空,不提醒;
// - 新增、或首次发现已在等待:从此刻重新计时、重新计次,不提醒(新增由普通提示音负责);
// - 距上一次提醒满 AR_REMIND_AFTER_MS 且本轮次数未满 AR_REMIND_MAX:提醒并重新计时。
// 处理了一部分(数量减少但不为 0)不算新增,沿用原来的计时与计次
bool ar_remind_update(ar_remind_t *r, uint8_t wn, bool fresh, uint32_t now_ms);

// 音量档位 → 播放音量百分比;越界按最高档
uint8_t ar_volume_percent(uint8_t level);

// 加减音量:到静音与最高档时停住;越界先按最高档处理
uint8_t ar_volume_step(uint8_t level, int delta);

// 亮度档位 → 背光百分比;越界按最高档
uint8_t ar_brightness_percent(uint8_t level);

// 加减亮度:到最低档与最高档时停住;越界先按最高档处理
uint8_t ar_brightness_step(uint8_t level, int delta);

// 熄屏时间选项 → 毫秒;越界按最后一项。接电源的最后一项“不熄屏”返回 AR_OFF_NEVER
uint32_t ar_battery_off_ms(uint8_t option);
uint32_t ar_usb_off_ms(uint8_t option);

// 加减选项序号(共 count 项):到头停住;越界先按最后一项处理
uint8_t ar_option_step(uint8_t option, uint8_t count, int delta);

// 开关类设置项的调节:上键(delta > 0)开启、下键关闭,delta 为 0 时不变。
// 不做来回翻转,上下键连按被识别成双击时结果也确定
bool ar_toggle_step(bool on, int delta);

// 本地时间:主机在状态帧 tm 字段里同步的当日分钟数,之后按本机毫秒计数走时
typedef struct {
    bool known;          // 是否对过时
    uint16_t minute;     // 对时那一刻的当日分钟数
    uint32_t at_ms;      // 对时那一刻(收到那一帧)的本机毫秒数
} ar_clock_t;

void ar_clock_init(ar_clock_t *c);

// 用主机的当日分钟数对时;minute 为 AR_TIME_UNKNOWN 或超过 AR_TIME_MAX_MIN 时不改变。
// 每收到一帧都可以调用,以最新一帧为准
void ar_clock_sync(ar_clock_t *c, uint16_t minute, uint32_t at_ms);

// 取 now_ms 时刻的当日分钟数写入 *minute;还没对过时返回 false。now_ms 允许回绕
bool ar_clock_minute(const ar_clock_t *c, uint32_t now_ms, uint16_t *minute);

// 当日分钟数是否落在夜间时段 [AR_NIGHT_START_MIN, 24:00) ∪ [0, AR_NIGHT_END_MIN)
bool ar_is_night(uint16_t minute);

// 夜间勿扰此刻的状态,供二级页面的说明使用
typedef enum {
    AR_NIGHT_NO_TIME = 0,   // 还没对过时,夜间勿扰不生效
    AR_NIGHT_ON,            // 在夜间时段内
    AR_NIGHT_OFF,           // 不在夜间时段内
} ar_night_t;

ar_night_t ar_night_state(const ar_clock_t *c, uint32_t now_ms);

// 勿扰是否生效:手动勿扰开着,或夜间勿扰开着且已知此刻在夜间时段内。
// 生效期间不响任何提示音,通知与连上电脑不点亮屏幕(屏幕方案 §6.2)
bool ar_dnd_active(bool manual, bool night_enabled, const ar_clock_t *c, uint32_t now_ms);

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
    AR_MENU_BATTERY_OFF,   // 电池熄屏时间
    AR_MENU_USB_OFF,       // 接电源熄屏时间
    AR_MENU_DND,           // 勿扰(手动开关)
    AR_MENU_NIGHT,         // 夜间勿扰
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
    bool on_power;              // 接着电源:USB 主机在线,或按电池电压推断接着充电器,见 ar_extpower_update()
    uint8_t level;              // 用户设定的亮度档位
    uint32_t ms_since_wake;     // 距最近一次亮屏事件(按键、通知、连上电脑、配对确认、开机),从未发生传 UINT32_MAX
    uint32_t ms_disconnected;   // 已处于“未连接”的时长,在线时为 0
    uint8_t battery_off;        // 电池供电的熄屏时间选项,见 ar_battery_off_ms()
    uint8_t usb_off;            // 接电源的熄屏时间选项,见 ar_usb_off_ms()
    bool locked;                // 用户手动熄屏,到下一次亮屏事件为止
} ar_backlight_in_t;

// 手动熄屏(长按确定键)是否生效:设置菜单打开或正在配对确认时不响应
bool ar_lock_allowed(bool menu_open, bool pairing);

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

// 外部供电推断。板上没有 VBUS 或充电状态检测引脚,芯片只知道 USB 主机是否在线,
// 接充电器、充电宝或主机睡眠时都看不出接着电源。这里靠电量计的电池电压推断:
// 本板的电源路径在接着电源时由充电器带负载,电池只承担充电电流,所以插拔都会让电压在一个采样点内跳变。
// 2026-11-10 真机实测(电量 97%,每 500 ms 一点):插上 +75..+94 mV;充满停充 -42..-49 mV(仍接着电源);
// 停充后拔掉 -16..-20 mV(只剩负载转回电池);其余时间相邻点相差不超过 3 mV。详见供电方案 §3
#define AR_PWR_SAMPLE_MS 500u      // 采样周期,调用方按这个节拍调用 ar_extpower_update()
#define AR_PWR_STEP_MV 10          // 插拔判定的最小跳变,连续两点成立才算
#define AR_PWR_CV_MV 4195          // 充电器恒压段的电压下限:实测恒压段 4203..4221,电池单独供电不超过 4150
#define AR_PWR_TERM_MAX_MV 55      // 恒压段停充的最大降幅:实测 42..49;达到它视为恒压段拔掉(还要加上负载那一份)
#define AR_PWR_SOC_FALL 2          // 判定接着电源、USB 主机离线,电量却比最高值低了这么多:其实在耗电池
#define AR_PWR_HISTORY 14          // 保留的采样点数:最近两点 + 之前 6 秒作基准

// 推断出的供电状态
typedef enum {
    AR_PWR_BATTERY = 0,    // 电池供电
    AR_PWR_CHARGING,       // 接着电源,正在充电(插上的跳变,或电压到了恒压段)
    AR_PWR_IDLE,           // 接着电源,没在充电(充满停充,或 USB 主机在线时)
} ar_pwr_state_t;

// 设备自身负载在本采样周期内的变化。改背光会让电池电压跳几毫伏到十几毫伏,提示音会让它短暂下陷,
// 都要和插拔区分开
typedef enum {
    AR_LOAD_STEADY = 0,    // 负载没变
    AR_LOAD_CHANGED,       // 负载变了(背光变化):电池供电时丢掉旧基准,以新电平重新开始;接着电源时
                           // 负载由充电器带,电池电压不受影响,照常判断
    AR_LOAD_TRANSIENT,     // 短暂负载(提示音):这一点不计入
} ar_load_t;

typedef struct {
    int16_t mv[AR_PWR_HISTORY];   // 最近的电压采样,mv[n-1] 最新
    uint8_t n;                    // 有效采样点数
    uint8_t state;                // ar_pwr_state_t
    int8_t peak_soc;              // 判定接着电源以来的最高电量,-1 表示还没有读数
    bool plugged;                 // 本次采样认出了插上的跳变,调用方据此亮屏
} ar_extpower_t;

void ar_extpower_init(ar_extpower_t *p);

// 每 AR_PWR_SAMPLE_MS 调用一次,返回是否接着外部电源。mv、soc 为电量计读数,读失败传负数;
// usb_host 为 USB 主机在线。规则(最近两点都要满足,避免单点毛刺):
// - USB 主机在线:一定接着电源;离线时不改判定,电压没有降(Mac 睡眠、USB 挂起)就仍算接着电源;
// - 两点都比之前 6 秒的最高值高出 AR_PWR_STEP_MV:插上,进入充电,并置 plugged。已判定接着电源时
//   也这样处理:判定可能因漏判拔掉而过时,调用方要按 plugged 亮屏,不能只看返回值的变化;
// - 电池供电时两点都达到 AR_PWR_CV_MV:电池单独供电达不到这个电压,进入充电并置 plugged
//   (开机时已在恒压充电,或插上的跳变被负载变化盖过);
// - 接着电源时,两点都比之前 2 秒的中位数低 AR_PWR_STEP_MV:从恒压段降、且降幅小于
//   AR_PWR_TERM_MAX_MV 是充满停充,仍接着电源;否则是拔掉(USB 主机在线时除外)。电池供电时的
//   下陷是负载引起的,不改判定、不重置基准,回升时才不会被当成插上;
// - 兜底:判定接着电源、USB 主机离线,电量比判定以来的最高值低 AR_PWR_SOC_FALL,改判电池供电。
// 判定改变或认出插上后只留最近两点作新基准,同一次跳变不会重复触发;基准有 1 点就开始判断,
// 背光变化、判定改变后不久的插拔不会混进基准
bool ar_extpower_update(ar_extpower_t *p, int mv, int soc, bool usb_host, ar_load_t load);

// 供电方式跟踪,用来在插拔时补记亮屏事件
typedef struct {
    bool known;      // 是否已记录过供电方式
    bool on_power;   // 上一次是否接着电源
} ar_power_t;

void ar_power_init(ar_power_t *p);

// 每个周期调用一次。返回 true 表示调用方应记一次亮屏事件:
// - 接上电源(USB 主机或充电器):不论屏幕亮灭、是否手动熄屏、是否勿扰都点亮,插电是用户自己的操作。
//   这里只看判定的变化;判定已是接着电源时的插上由 ar_extpower_t.plugged 报告,调用方同样亮屏;
// - 断开电源且此刻屏幕亮着:电池熄屏时限短得多(默认 30 秒),按最近一次亮屏事件计时会让屏幕立刻熄灭。
// 第一次调用只记录基准:开机本身就算亮屏事件
bool ar_power_update(ar_power_t *p, bool on_power, bool screen_on);

// 背光百分比,0 表示熄屏。“低亮度”为 AR_DIM_PERCENT 与设定亮度中较低者。
// - 手动熄屏后:到下一次亮屏事件前一直熄灭;
// - 电池供电:亮屏事件后在电池熄屏时间内用低亮度,之后熄屏;
// - 接着电源且“未连接”:未连接与无亮屏事件都满 AR_BACKLIGHT_IDLE_MS 时熄屏(不受熄屏设置影响);
// - 接着电源:亮屏事件后先用设定亮度,满 AR_USB_DIM_AFTER_MS 与熄屏时间一半中较短者
//   降到低亮度,满接电源熄屏时间熄屏;“不熄屏”时一直保持低亮度。
uint8_t ar_model_backlight_percent(const ar_backlight_in_t *in);

// 深睡判定的输入
typedef struct {
    bool on_power;              // 接着电源(USB 主机在线,或推断接着充电器),见 ar_extpower_update()
    bool battery_present;       // 电量计读得到电量;读不到时可能没装电池,只靠 USB 供电
    bool confirming;            // 正在等用户确认配对
    uint32_t ms_disconnected;   // 已处于“未连接”的时长,在线时为 0
    uint32_t ms_since_key;      // 距最近一次按键,从未按过传 UINT32_MAX
    uint32_t ms_dark;           // 屏幕已持续熄灭的时长,亮着时为 0
} ar_sleep_in_t;

// 是否该进入深睡(关机,按任意键唤醒)。前提是没接电源、电量计正常、不在配对确认,
// 满足其一即睡:
// - “未连接”与无按键都已满 AR_DEEP_SLEEP_AFTER_MS;
// - 屏幕持续熄灭已满 AR_POWER_OFF_AFTER_DARK_MS。在线时也适用:等你处理、有会话完成
//   都会点亮屏幕并重新计时,所以只有长时间没有任何通知和按键才会关机。
bool ar_model_should_deep_sleep(const ar_sleep_in_t *in);
