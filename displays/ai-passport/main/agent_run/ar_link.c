// main/agent_run/ar_link.c —— USB 串口链路实现。
//
// 实现要点来自上游社区文档 docs/reference/y2lin/serial-screenshot-protocol:
// - 读取前必须显式安装 USB-Serial-JTAG 驱动,并把控制台 VFS 切到驱动通道;
// - 接收任务优先级低于 LVGL,读错误时退避,所有空闲路径都要让出 CPU;
// - HELLO 经 stdout 输出,与 ESP_LOG 共用 stdio 锁,避免与日志行交错。
#include "ar_link.h"

#include <stdio.h>

#include "ar_capture.h"
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ar_link";

#define RX_BUFFER_SIZE 256
#define TX_BUFFER_SIZE 1024
#define LINK_TASK_STACK 3072
#define LINK_TASK_PRIO 3        // 低于 LVGL 任务(4)
#define READ_TIMEOUT_MS 50
#define READ_ERROR_BACKOFF_MS 200

static SemaphoreHandle_t s_lock;
static ar_link_snapshot_t s_snap;
static ar_line_buf_t s_line;

const char *ar_link_hello_line(void) {
    return "AR1 HELLO fw=" AR_FW_VERSION " proto=1";
}

// 回复握手;fflush 保证整行立即发出
static void send_hello(void) {
    printf("%s\n", ar_link_hello_line());
    fflush(stdout);
}

void ar_link_publish(const ar_state_t *st, ar_source_t src) {
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.state = *st;
    s_snap.has_state = true;
    s_snap.last_source = src;
    if (src == AR_SRC_BLE) s_snap.last_ble_us = now;
    else s_snap.last_usb_us = now;
    xSemaphoreGive(s_lock);
}

// 处理一整行:状态帧写入快照,PING 回复 HELLO,截屏命令交给主循环,其他行忽略
static void handle_line(const char *line) {
    if (strcmp(line, "FAP_SCREENSHOT_V1") == 0) {
        ar_capture_request();   // 截屏要持有 LVGL 锁并整屏刷新,放到主循环里做
        return;
    }
    ar_state_t st;
    switch (ar_proto_parse_line(line, &st)) {
    case AR_LINE_STATE:
        ar_link_publish(&st, AR_SRC_USB);
        break;
    case AR_LINE_PING:
        send_hello();
        break;
    default:
        break;
    }
}

static void link_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    char line[AR_PROTO_MAX_LINE + 1];
    for (;;) {
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(READ_ERROR_BACKOFF_MS));
            continue;
        }
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(READ_ERROR_BACKOFF_MS));
            continue;
        }
        if (n == 0) continue;   // 读超时本身已经让出了 CPU
        for (int i = 0; i < n; i++) {
            if (ar_line_push(&s_line, (char)buf[i], line, sizeof(line))) handle_line(line);
        }
    }
}

esp_err_t ar_link_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_snap, 0, sizeof(s_snap));
    ar_line_init(&s_line);

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_BUFFER_SIZE,
        .tx_buffer_size = TX_BUFFER_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    // 控制台输出改走驱动通道,日志与协议行共用同一条带缓冲的发送路径
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(link_task, "ar_link", LINK_TASK_STACK, NULL, LINK_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    send_hello();   // 主机若已在等待,开机即可完成握手
    return ESP_OK;
}

void ar_link_get(ar_link_snapshot_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}
