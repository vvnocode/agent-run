// main/agent_run/ar_ui.h —— Agent Run! 界面(LVGL)。
//
// 所有函数都必须在持有 bsp_lvgl_lock() 时调用。布局按 240×320 屏幕稿 1:1 绘制,
// 见 Design 画布「Agent Run! 屏幕稿」。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ar_model.h"
#include "ar_proto.h"

// 界面需要的全部输入
typedef struct {
    ar_screen_t screen;
    ar_state_t state;      // screen 不是 DISCONNECTED 时有效
    uint8_t link_source;   // 标题栏链路图标:ar_source_t(无、USB、蓝牙)
    uint8_t volume;        // 音量档位(0 为静音),标题栏喇叭图标与设置菜单
    uint8_t brightness;    // 亮度档位,设置菜单
    uint8_t battery_off;   // 电池熄屏时间选项,设置菜单
    uint8_t usb_off;       // 接电源熄屏时间选项,设置菜单
    bool dnd;              // 勿扰(手动)开关,设置菜单
    bool night;            // 夜间勿扰开关,设置菜单
    uint8_t night_state;   // ar_night_t:夜间勿扰此刻的状态,夜间勿扰二级页面的说明
    bool dnd_active;       // 勿扰此刻生效,标题栏显示月牙
    uint8_t menu_level;    // ar_menu_level_t:设置菜单的层级,AR_MENU_CLOSED 表示不显示
    uint8_t menu_item;     // ar_menu_item_t:一级的选中项,二级时为正在调节的项
    uint16_t done_flash;   // 刚完成的轮数,大于 0 时普通画面显示“完成 +N”
    int battery_soc;       // 电量百分比,-1 表示读不到
    bool pairing;          // 正在等用户确认蓝牙配对(优先于其他画面)
    bool pair_cleared;     // 刚清除配对,在“未连接”画面提示
    bool ble_bonded;       // 已有确认过的蓝牙主机
    char ble_name[16];     // 蓝牙广播名
    uint8_t page;          // ar_page_t:要显示的页面,已按 ar_page_shown() 换算过画面优先级
    bool has_agent;        // page 为专用页面时:是否收到过该工具的 A 帧
    bool agent_flash;      // page 为专用页面时:该工具刚有会话完成,“今日完成”格用强调色
    ar_agent_t agent;      // page 为专用页面且 has_agent 时有效
} ar_view_t;

// 创建界面并加载为当前 screen
void ar_ui_create(void);

// 按 view 更新布局、文字与数字(调用方只在内容变化时调用)
void ar_ui_update(const ar_view_t *view);

// 重画宠物场景的第 tick 帧:精灵按 tick 奇偶换帧,地面与装饰按 tick 滚动
void ar_ui_animate(ar_screen_t screen, uint32_t tick);
