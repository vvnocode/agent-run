// main/agent_run/ar_ble.c —— 蓝牙链路实现(NimBLE 外设)。
//
// 安全模型(无线方案 §4.2,实施时采用 Just Works + 设备上确认):
// - 主机连上后由设备发起安全流程,使用 LE Secure Connections 的 Just Works 配对并绑定;
// - 没有已确认的绑定时,新配对完成后在屏幕上请用户按 OK 确认,确认前收到的数据一律丢弃;
// - 已有确认过的绑定时,只接受用已有绑定加密的连接;新出现的配对一律删除并断开;
// - “是否已确认”另存一个 NVS 标记,防止确认前断电后,未确认的绑定在重启后被当成已确认。
// NimBLE 回调运行在 NimBLE 主机任务中,只做有界的工作;状态用互斥锁保护。
#include "ar_ble.h"

#include <stdio.h>
#include <string.h>

#include "ar_link.h"
#include "ar_pair.h"
#include "ar_proto.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// NimBLE 自带的 NVS 绑定存储;ESP-IDF 没有在公开头文件中声明它
void ble_store_config_init(void);

static const char *TAG = "ar_ble";
static const char *NVS_NAMESPACE = "agent_run";
static const char *NVS_KEY_PAIRED = "paired";

// Nordic UART 布局的 128 位 UUID(字节按小端序书写)
// 6e400001-b5a3-f393-e0a9-e50e24dcca9e
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

// 请求的连接参数:间隔 450-600 ms、无从机延迟、监督超时 6 秒,满足 Apple 对外设参数的约束
#define CONN_ITVL_MIN 360        // 单位 1.25 ms
#define CONN_ITVL_MAX 480
#define CONN_SUPERVISION 600     // 单位 10 ms

static SemaphoreHandle_t s_lock;
static ar_pair_t s_pair;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_trusted;              // 当前连接已确认,数据可以采纳
static ble_addr_t s_pending_addr;   // 待确认连接的对端身份地址
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static char s_name[16];
static ar_line_buf_t s_line;        // 只在 NimBLE 主机任务中使用
static bool s_running;              // 主机任务已启动,ar_ble_stop() 据此决定是否需要停止
static volatile bool s_stopping;    // 正在停止协议栈:主机任务据此不再重新广播

static int gap_event(struct ble_gap_event *event, void *arg);

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ------------------------------------------------------------------------
// 绑定与确认标记
// ------------------------------------------------------------------------

static int bond_count(void) {
    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0) return 0;
    return n;
}

static void save_paired(bool paired) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, NVS_KEY_PAIRED, paired ? 1 : 0) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool load_paired(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, NVS_KEY_PAIRED, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v != 0;
}

// 拒绝一条连接:删掉它刚产生的绑定,然后断开
static void reject_conn(uint16_t conn, const ble_addr_t *addr) {
    ESP_LOGI(TAG, "拒绝连接 %u", conn);
    ble_store_util_delete_peer(addr);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void request_conn_params(uint16_t conn) {
    struct ble_gap_upd_params p = {
        .itvl_min = CONN_ITVL_MIN,
        .itvl_max = CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(conn, &p);
    if (rc != 0) ESP_LOGW(TAG, "请求连接参数失败: %d", rc);
}

// ------------------------------------------------------------------------
// GATT
// ------------------------------------------------------------------------

// 按当前 MTU 分片,用 TX 通知发出一整行(含行尾)
static void notify_line(uint16_t conn, const char *line) {
    char buf[AR_PROTO_MAX_LINE + 2];
    int len = snprintf(buf, sizeof(buf), "%s\n", line);
    if (len <= 0) return;
    uint16_t mtu = ble_att_mtu(conn);
    int chunk = mtu > 3 ? mtu - 3 : 20;
    for (int off = 0; off < len; off += chunk) {
        int n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + off, (uint16_t)n);
        if (!om || ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) break;   // 失败时 mbuf 已被释放
    }
}

static void handle_line(uint16_t conn, const char *line) {
    ar_state_t st;
    ar_agent_t agent;
    switch (ar_proto_parse_line(line, &st)) {
    case AR_LINE_STATE:
        ar_link_publish(&st, AR_SRC_BLE);
        break;
    case AR_LINE_PING:
        notify_line(conn, ar_link_hello_line());
        break;
    default:
        // 工具专用页面的 A 帧,与 USB 来源同样处理
        if (ar_proto_parse_agent(line, &agent)) ar_link_publish_agent(&agent);
        break;
    }
}

// RX:主机写入的字节流。未确认的连接直接丢弃
static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool trusted = s_trusted && conn == s_conn;
    xSemaphoreGive(s_lock);
    if (!trusted) return 0;

    uint8_t buf[256];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    char line[AR_PROTO_MAX_LINE + 1];
    for (uint16_t i = 0; i < len; i++) {
        if (ar_line_push(&s_line, (char)buf[i], line, sizeof(line))) handle_line(conn, line);
    }
    return 0;
}

// TX:只用于通知;NimBLE 要求每个特征都有 access_cb,读取时返回两个识别字节
static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, "AR", 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def CHARACTERISTICS[] = {
    {
        .uuid = &RX_UUID.u,
        .access_cb = rx_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &TX_UUID.u,
        .access_cb = tx_access,
        .val_handle = &s_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
    },
    { 0 },
};

static const struct ble_gatt_svc_def SERVICES[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC_UUID.u, .characteristics = CHARACTERISTICS },
    { 0 },
};

// ------------------------------------------------------------------------
// GAP
// ------------------------------------------------------------------------

static void advertise(void) {
    if (s_stopping) return;   // 停止广播时触发的事件不再重启广播,否则协议栈已停用会报错
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败: %d", rc);
        return;
    }
    // 128 位服务 UUID 放进扫描响应,主广播包留给名字
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = &SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败: %d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(211);   // Apple 建议的广播间隔档位之一
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(319);
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "开始广播失败: %d", rc);
}

// 记下当前连接,新连接时清掉上一条连接的信任状态;返回是否是新连接。调用方持有 s_lock。
// NimBLE 把连接建立事件推迟到读完对端版本之后才上报,已绑定的主机重连时常常先完成加密,
// 所以连接建立和加密两个事件都会调用这里:同一条连接只初始化一次,
// 后到的连接建立事件不能清掉加密时已经建立的信任
static bool link_up(uint16_t conn) {
    if (s_conn == conn) return false;
    s_conn = conn;
    s_trusted = false;
    return true;
}

// 加密建立:区分“已确认的老主机重连”与“新配对”,规则见 ar_pair_encrypted()
static void on_enc_change(uint16_t conn, int status) {
    if (status != 0) {
        ESP_LOGW(TAG, "加密失败(%d),断开;如果电脑删除过配对,请长按上键 5 秒清除设备上的配对", status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    const int bonds = bond_count();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool fresh = link_up(conn);   // 加密可能先于连接建立事件到达
    const ar_pair_action_t act =
        ar_pair_encrypted(&s_pair, conn, bonds, desc.sec_state.bonded, now_ms());
    if (act == AR_PAIR_ACT_ACCEPT) {
        s_trusted = true;   // 用已确认的绑定加密:就是那台电脑
    } else {
        s_pending_addr = desc.peer_id_addr;
    }
    xSemaphoreGive(s_lock);
    if (fresh) ar_line_init(&s_line);

    if (act == AR_PAIR_ACT_REJECT) reject_conn(conn, &desc.peer_id_addr);
    else if (act == AR_PAIR_ACT_ACCEPT) request_conn_params(conn);
    else ESP_LOGI(TAG, "新配对,等待在设备上确认");
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool fresh = link_up(event->connect.conn_handle);
        xSemaphoreGive(s_lock);
        if (fresh) ar_line_init(&s_line);
        ESP_LOGI(TAG, "主机已连接 %u", event->connect.conn_handle);
        // 由设备发起安全流程:老主机用已有绑定加密,新主机开始配对。
        // 主机已经抢先完成加密时不再发起,免得它把安全请求当成要重新配对
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0 || !desc.sec_state.encrypted) {
            ble_gap_security_initiate(event->connect.conn_handle);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ar_pair_disconnected(&s_pair, event->disconnect.conn.conn_handle);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_trusted = false;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "主机已断开,原因 0x%x", event->disconnect.reason);
        advertise();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        on_enc_change(event->enc_change.conn_handle, event->enc_change.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // 有绑定的主机发起重新配对(通常是电脑删除过配对):删除旧绑定,按新配对走确认
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ar_pair_cleared(&s_pair);
        xSemaphoreGive(s_lock);
        save_paired(false);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        // 本设备声明无输入输出,只用 Just Works;其他配对方式一律拒绝
        struct ble_sm_io io;
        memset(&io, 0, sizeof(io));
        io.action = event->passkey.params.action;
        if (io.action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = 0;
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "蓝牙地址初始化失败: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE 复位,原因 %d", reason);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();   // 直到 nimble_port_stop() 才返回
    nimble_port_freertos_deinit();
}

// ------------------------------------------------------------------------
// 对外接口
// ------------------------------------------------------------------------

esp_err_t ar_ble_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // 广播名取蓝牙 MAC 末两字节,开机即可显示,不必等协议栈同步
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "AgentRun-%02X%02X", mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(SERVICES);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败: %d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    // 有绑定却没有确认标记(例如确认前断电):清掉这些未确认的绑定
    int bonds = bond_count();
    bool paired = load_paired();
    if (bonds > 0 && !paired) {
        ESP_LOGW(TAG, "发现 %d 个未确认的绑定,已清除", bonds);
        ble_store_clear();
        bonds = 0;
    }
    if (bonds == 0 && paired) {
        save_paired(false);
        paired = false;
    }
    ar_pair_init(&s_pair, paired ? bonds : 0);
    ar_line_init(&s_line);

    nimble_port_freertos_init(host_task);
    s_running = true;
    ESP_LOGI(TAG, "蓝牙已启动:%s,%s", s_name, s_pair.bonded ? "已配对" : "等待配对");
    return ESP_OK;
}

void ar_ble_get_status(ar_ble_status_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->connected = s_conn != BLE_HS_CONN_HANDLE_NONE;
    out->trusted = s_trusted;
    out->bonded = s_pair.bonded;
    out->confirming = ar_pair_confirming(&s_pair);
    xSemaphoreGive(s_lock);
    memcpy(out->name, s_name, sizeof(out->name));
}

void ar_ble_pair_key(bool ok) {
    if (!s_lock) return;
    // 绑定数在 s_lock 外读取(读取要拿 NimBLE 的锁);待确认的新绑定此时已经写入
    const int bonds = bond_count();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t conn = s_pair.conn;
    const ble_addr_t addr = s_pending_addr;
    ar_pair_action_t act = ar_pair_key(&s_pair, ok);
    if (act == AR_PAIR_ACT_ACCEPT) {
        ar_pair_confirmed(&s_pair, bonds);
        s_trusted = conn == s_conn;
    }
    xSemaphoreGive(s_lock);

    if (act == AR_PAIR_ACT_ACCEPT) {
        save_paired(true);
        ESP_LOGI(TAG, "已确认配对");
        request_conn_params(conn);
    } else if (act == AR_PAIR_ACT_REJECT) {
        reject_conn(conn, &addr);
    }
}

void ar_ble_tick(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t conn = s_pair.conn;
    const ble_addr_t addr = s_pending_addr;
    ar_pair_action_t act = ar_pair_tick(&s_pair, now_ms());
    xSemaphoreGive(s_lock);
    if (act == AR_PAIR_ACT_REJECT) reject_conn(conn, &addr);
}

void ar_ble_clear_bonds(void) {
    if (!s_lock) return;
    ble_store_clear();
    save_paired(false);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ar_pair_cleared(&s_pair);
    s_trusted = false;
    const uint16_t conn = s_conn;
    xSemaphoreGive(s_lock);
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "已清除全部配对");
}

esp_err_t ar_ble_stop(void) {
    if (!s_running) return ESP_OK;
    s_stopping = true;
    // nimble_port_stop() 阻塞到主机任务退出 nimble_port_run(),之后才能释放控制器
    if (nimble_port_stop() != 0) return ESP_FAIL;
    s_running = false;
    return nimble_port_deinit();
}
