// main/agent_run/ar_capture.h —— FAP_SCREENSHOT_V1 串口截屏。
//
// 电脑经 USB 串口发送一行 "FAP_SCREENSHOT_V1",设备回一行
// "FAP_SCREENSHOT_V1 240 320 RGB565LE 153600",随后紧跟 153600 字节小端 RGB565 像素。
// 这是 AI Passport 社区上架要求的截屏协议,本项目也用它生成 README 里的真机画面。
#pragma once

// 在显示上注册刷新钩子。需在 LVGL 初始化之后、持有 bsp_lvgl_lock() 时调用。
void ar_capture_init(void);

// 请求一次截屏(串口接收任务收到命令时调用,可在任意任务中调用)
void ar_capture_request(void);

// 主循环中调用:有请求时完成一次截屏并发出。调用时不得持有 LVGL 锁。
void ar_capture_service(void);
