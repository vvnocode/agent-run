// main/agent_run/ar_text.h —— Agent Run! 界面上的全部中文文案。
//
// 中文字体 ar_font_cjk14 只包含这里出现的汉字(加可打印 ASCII),由
// tools/gen_agent_run_font.sh 从本文件提取字符清单生成。修改文案后必须重新生成字体,
// tests/test_ar_font_coverage.py 会检查字体是否覆盖本文件的全部字符。
// 界面代码里不要直接写中文字面量,统一引用这里的宏。
#pragma once

// 跑速状态
#define AR_TXT_SLEEP "睡觉中"
#define AR_TXT_WALK "慢走中"
#define AR_TXT_TROT "小跑中"
#define AR_TXT_RUN "狂奔中"
#define AR_TXT_DASH "冲刺中"
#define AR_TXT_UNIT_RUNNING "个会话在跑"
#define AR_TXT_SPEED "速度"

// 底栏
#define AR_TXT_DONE_TODAY "今日完成"
#define AR_TXT_WAITING_SHORT "等你"
#define AR_TXT_DONE_FLASH_FMT "完成 +%u"

// 等你处理
#define AR_TXT_WAITING_TITLE "个会话等你处理"
#define AR_TXT_REASON_APPROVE "等待授权"
#define AR_TXT_REASON_QUESTION "等你回答"
#define AR_TXT_OTHERS_FMT "另有 %u 个在跑"
#define AR_TXT_MENU_HINT "确定键设置"

// 未连接
#define AR_TXT_DISCONNECTED "未连接电脑"
#define AR_TXT_DISCONNECTED_HINT "插上 USB 或靠近已配对的电脑"
#define AR_TXT_PAIR_READY_FMT "可配对：%s"

// 蓝牙配对
#define AR_TXT_PAIR_TITLE "配对这台电脑？"
#define AR_TXT_PAIR_HINT "按 OK 确认，按下键取消"
#define AR_TXT_PAIR_CLEARED "已清除配对"

// 设置菜单:一级列出设置项,二级调节
#define AR_TXT_MENU_TITLE "设置"
#define AR_TXT_MENU_VOLUME "音量"
#define AR_TXT_MENU_BRIGHTNESS "亮度"
#define AR_TXT_MENU_LIST_HINT "确定进入 · 双击返回"
#define AR_TXT_MENU_ADJUST_HINT "上下调节 · 双击返回"
#define AR_TXT_VOLUME_MUTE "静音"
#define AR_TXT_VOLUME_LOW "低"
#define AR_TXT_VOLUME_MID "中"
#define AR_TXT_VOLUME_HIGH "高"

// 设置菜单:熄屏时间与勿扰(屏幕方案 §4、§6、§7)
#define AR_TXT_MENU_BATTERY_OFF "电池熄屏"
#define AR_TXT_MENU_USB_OFF "接电脑熄屏"
#define AR_TXT_MENU_DND "勿扰"
#define AR_TXT_MENU_NIGHT "夜间勿扰"
#define AR_TXT_MENU_TOGGLE_HINT "上下切换 · 双击返回"
// 熄屏时间各选项的显示文字,顺序与 ar_battery_off_ms()、ar_usb_off_ms() 的选项一致
#define AR_TXT_BATTERY_OFF_NAMES { "10 秒", "30 秒", "1 分", "3 分" }
#define AR_TXT_USB_OFF_NAMES { "5 分", "10 分", "30 分", "1 小时", "不熄屏" }
#define AR_TXT_BATTERY_OFF_RANGE "10 秒 – 3 分"
#define AR_TXT_USB_OFF_RANGE "5 分 – 不熄屏"
#define AR_TXT_ON "开启"
#define AR_TXT_OFF "关闭"
#define AR_TXT_DND_DESC "不响铃，通知不点亮屏幕"
#define AR_TXT_NIGHT_DESC "每天 22:00 – 08:00"
#define AR_TXT_NIGHT_ACTIVE "现在生效中"
#define AR_TXT_NIGHT_INACTIVE "现在未生效"
#define AR_TXT_NIGHT_NO_TIME "连上电脑同步时间后生效"

// 工具专用页面(专用页面方案 §4.2)
#define AR_TXT_USAGE_FIVE_HOUR "5 小时用量"
#define AR_TXT_USAGE_WEEK "每周用量"
#define AR_TXT_USAGE_NO_DATA "暂无数据"
#define AR_TXT_USAGE_NO_LIMIT "无此限额"
#define AR_TXT_RESET_MIN_FMT "%u 分钟后重置"
#define AR_TXT_RESET_HOUR_FMT "%u 小时 %u 分后重置"
#define AR_TXT_RESET_DAY_FMT "%u 天 %u 小时后重置"
#define AR_TXT_RESET_UNKNOWN "重置时间未知"
#define AR_TXT_TILE_WORKING "工作中"
#define AR_TXT_TILE_BACKGROUND "后台运行"
#define AR_TXT_PAGE_HINT "上下键切换"

// 工具名(等待列表)
#define AR_TXT_TOOL_CC "Claude Code"
#define AR_TXT_TOOL_CX "Codex"
#define AR_TXT_TOOL_DS "dsh"
