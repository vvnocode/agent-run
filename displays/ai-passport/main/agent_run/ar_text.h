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

// 工具名(等待列表)
#define AR_TXT_TOOL_CC "Claude Code"
#define AR_TXT_TOOL_CX "Codex"
#define AR_TXT_TOOL_DS "dsh"
