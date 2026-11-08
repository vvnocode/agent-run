// tests/test_ar_pair.c —— Agent Run! 蓝牙配对确认规则的宿主测试(无线方案 §4.2)。
#include <assert.h>

#include "agent_run/ar_pair.h"

static void test_first_pairing_needs_confirmation(void) {
    ar_pair_t p;
    ar_pair_init(&p, false);
    assert(!ar_pair_confirming(&p));
    // 没有绑定时,新主机的配对请求进入待确认,屏幕显示比较码
    assert(ar_pair_request(&p, 1, false, 123456, 1000) == AR_PAIR_ACT_NONE);
    assert(ar_pair_confirming(&p));
    assert(p.code == 123456 && p.conn == 1);
    // 按 OK 接受,之后视为已绑定
    assert(ar_pair_key(&p, true) == AR_PAIR_ACT_ACCEPT);
    assert(!ar_pair_confirming(&p));
    ar_pair_confirmed(&p, 1);
    assert(p.bonded);
}

static void test_cancel_and_timeout_reject(void) {
    ar_pair_t p;
    ar_pair_init(&p, false);
    ar_pair_request(&p, 2, false, 0, 1000);   // 0 表示 Just Works,没有比较码
    assert(ar_pair_key(&p, false) == AR_PAIR_ACT_REJECT);
    assert(!ar_pair_confirming(&p) && !p.bonded);

    ar_pair_request(&p, 3, false, 0, 5000);
    assert(ar_pair_tick(&p, 5000 + AR_PAIR_TIMEOUT_MS - 1) == AR_PAIR_ACT_NONE);
    assert(ar_pair_tick(&p, 5000 + AR_PAIR_TIMEOUT_MS) == AR_PAIR_ACT_REJECT);
    assert(!ar_pair_confirming(&p));
    // 超时后再按键不再产生动作
    assert(ar_pair_key(&p, true) == AR_PAIR_ACT_NONE);
}

static void test_bonded_device_rejects_strangers(void) {
    ar_pair_t p;
    ar_pair_init(&p, true);
    assert(ar_pair_request(&p, 4, false, 654321, 1000) == AR_PAIR_ACT_REJECT);
    assert(!ar_pair_confirming(&p));
    // 已绑定的那台电脑重新配对(例如它删掉了配对记录)时直接接受
    assert(ar_pair_request(&p, 5, true, 111111, 2000) == AR_PAIR_ACT_ACCEPT);
}

static void test_clearing_bond_allows_new_host(void) {
    ar_pair_t p;
    ar_pair_init(&p, true);
    ar_pair_cleared(&p);
    assert(!p.bonded);
    assert(ar_pair_request(&p, 6, false, 0, 1000) == AR_PAIR_ACT_NONE);
    assert(ar_pair_confirming(&p));
}

static void test_disconnect_during_confirmation(void) {
    ar_pair_t p;
    ar_pair_init(&p, false);
    ar_pair_request(&p, 7, false, 0, 1000);
    ar_pair_disconnected(&p, 8);   // 其他连接断开,不影响
    assert(ar_pair_confirming(&p));
    ar_pair_disconnected(&p, 7);   // 待确认的连接断开,取消确认
    assert(!ar_pair_confirming(&p));
    assert(ar_pair_key(&p, true) == AR_PAIR_ACT_NONE);
}

// 设备重启后已确认的主机立刻重连:NimBLE 可能先上报加密、后上报连接建立。
// 判断不能依赖连接建立时记下的绑定数,否则重连会被当成新配对,已确认的绑定被删掉
static void test_bonded_host_reconnects_after_reboot(void) {
    ar_pair_t p;
    ar_pair_init(&p, 1);   // 开机时保留 1 个已确认的绑定
    assert(ar_pair_encrypted(&p, 1, 1, true, 1000) == AR_PAIR_ACT_ACCEPT);
    assert(p.bonded && !ar_pair_confirming(&p));
}

// 确认配对后同一连接再次加密(例如主机刷新密钥):仍是那台主机,不能当成新配对
static void test_reencryption_after_confirmation(void) {
    ar_pair_t p;
    ar_pair_init(&p, 0);
    // 新配对:NimBLE 先写入绑定(绑定数变为 1),再上报加密
    assert(ar_pair_encrypted(&p, 2, 1, true, 1000) == AR_PAIR_ACT_NONE);
    assert(ar_pair_confirming(&p));
    assert(ar_pair_key(&p, true) == AR_PAIR_ACT_ACCEPT);
    ar_pair_confirmed(&p, 1);
    assert(ar_pair_encrypted(&p, 2, 1, true, 5000) == AR_PAIR_ACT_ACCEPT);
}

// 已绑定时其他主机完成了新配对(绑定数多出一个)或没有用绑定加密:直接拒绝,不打扰用户
static void test_bonded_device_rejects_new_pairing(void) {
    ar_pair_t p;
    ar_pair_init(&p, 1);
    assert(ar_pair_encrypted(&p, 3, 2, true, 1000) == AR_PAIR_ACT_REJECT);
    assert(ar_pair_encrypted(&p, 4, 1, false, 2000) == AR_PAIR_ACT_REJECT);
    assert(!ar_pair_confirming(&p));
    // 拒绝时新绑定已被删除,原来的主机重连照常信任
    assert(ar_pair_encrypted(&p, 5, 1, true, 3000) == AR_PAIR_ACT_ACCEPT);
}

int main(void) {
    test_bonded_host_reconnects_after_reboot();
    test_reencryption_after_confirmation();
    test_bonded_device_rejects_new_pairing();
    test_first_pairing_needs_confirmation();
    test_cancel_and_timeout_reject();
    test_bonded_device_rejects_strangers();
    test_clearing_bond_allows_new_host();
    test_disconnect_during_confirmation();
    return 0;
}
