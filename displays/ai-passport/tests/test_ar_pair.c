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
    ar_pair_confirmed(&p);
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

int main(void) {
    test_first_pairing_needs_confirmation();
    test_cancel_and_timeout_reject();
    test_bonded_device_rejects_strangers();
    test_clearing_bond_allows_new_host();
    test_disconnect_during_confirmation();
    return 0;
}
