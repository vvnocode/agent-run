// main/agent_run/ar_pair.c —— 蓝牙配对确认规则实现。见 ar_pair.h。
#include "ar_pair.h"

void ar_pair_init(ar_pair_t *p, int bonds) {
    p->bonded = bonds > 0;
    p->known_bonds = bonds;
    p->confirming = false;
    p->conn = 0;
    p->code = 0;
    p->started_ms = 0;
}

ar_pair_action_t ar_pair_request(ar_pair_t *p, uint16_t conn, bool peer_is_trusted, uint32_t code,
                                 uint32_t now_ms) {
    if (p->bonded) {
        // 已绑定:只有那台主机可以重新配对,其他主机直接拒绝,不打扰用户
        return peer_is_trusted ? AR_PAIR_ACT_ACCEPT : AR_PAIR_ACT_REJECT;
    }
    if (p->confirming && p->conn != conn) {
        return AR_PAIR_ACT_REJECT;   // 同一时间只确认一台
    }
    p->confirming = true;
    p->conn = conn;
    p->code = code;
    p->started_ms = now_ms;
    return AR_PAIR_ACT_NONE;
}

ar_pair_action_t ar_pair_encrypted(ar_pair_t *p, uint16_t conn, int bonds, bool link_bonded,
                                   uint32_t now_ms) {
    // 用绑定的密钥加密、且没有多出新绑定:是已确认的主机用原有绑定重连(或刷新密钥)。
    // 基线取已确认的绑定数而不是连接建立时的绑定数:NimBLE 可能先上报加密、后上报连接建立,
    // 重启后第一条连接如果拿未初始化的基线比较,会把重连误判成新配对并删掉已确认的绑定
    if (p->bonded && link_bonded && bonds <= p->known_bonds) return AR_PAIR_ACT_ACCEPT;
    return ar_pair_request(p, conn, false, 0, now_ms);
}

ar_pair_action_t ar_pair_key(ar_pair_t *p, bool ok) {
    if (!p->confirming) return AR_PAIR_ACT_NONE;
    p->confirming = false;
    return ok ? AR_PAIR_ACT_ACCEPT : AR_PAIR_ACT_REJECT;
}

ar_pair_action_t ar_pair_tick(ar_pair_t *p, uint32_t now_ms) {
    if (!p->confirming) return AR_PAIR_ACT_NONE;
    // 无符号差值,毫秒计数回绕时仍然正确
    if ((uint32_t)(now_ms - p->started_ms) < AR_PAIR_TIMEOUT_MS) return AR_PAIR_ACT_NONE;
    p->confirming = false;
    return AR_PAIR_ACT_REJECT;
}

void ar_pair_disconnected(ar_pair_t *p, uint16_t conn) {
    if (p->confirming && p->conn == conn) p->confirming = false;
}

void ar_pair_confirmed(ar_pair_t *p, int bonds) {
    p->known_bonds = bonds;
    p->bonded = true;
    p->confirming = false;
}

void ar_pair_cleared(ar_pair_t *p) {
    p->bonded = false;
    p->known_bonds = 0;
    p->confirming = false;
}

bool ar_pair_confirming(const ar_pair_t *p) {
    return p->confirming;
}
