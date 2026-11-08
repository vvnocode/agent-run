// main/agent_run/ar_pair.h —— 蓝牙配对确认规则(纯 C,可宿主测试)。
//
// 见无线方案 §4.2:没有绑定时,新主机配对要在设备上按 OK 确认;已有绑定时只接受那台主机,
// 其他主机的配对一律拒绝;长按上键清除绑定后才能换电脑。
// 本模块只做决策,不调用 NimBLE;蓝牙代码按返回的动作执行接受或拒绝。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 待确认超过这么久没有按键,按拒绝处理
#define AR_PAIR_TIMEOUT_MS 30000u

typedef enum {
    AR_PAIR_ACT_NONE = 0,   // 不需要动作(等待用户确认,或无事发生)
    AR_PAIR_ACT_ACCEPT,     // 接受这次配对
    AR_PAIR_ACT_REJECT,     // 拒绝:删除这次产生的绑定并断开
} ar_pair_action_t;

typedef struct {
    bool bonded;           // 已有确认过的绑定主机
    bool confirming;       // 正在等用户确认
    uint16_t conn;         // 待确认的连接句柄
    uint32_t code;         // 数字比较码,0 表示没有(Just Works)
    uint32_t started_ms;   // 进入待确认的时间
    int known_bonds;       // 已确认的绑定数:加密后绑定数超过它,说明这条连接刚完成新配对
} ar_pair_t;

// bonds:开机时保留下来的已确认绑定数,0 表示还没有确认过的主机
void ar_pair_init(ar_pair_t *p, int bonds);

// 有主机请求配对(数字比较),或 Just Works 配对刚完成时调用。
// peer_is_trusted:对方身份地址与已确认的绑定主机一致。
ar_pair_action_t ar_pair_request(ar_pair_t *p, uint16_t conn, bool peer_is_trusted, uint32_t code,
                                 uint32_t now_ms);

// 连接加密建立后调用,区分已确认主机的重连与新配对。
// bonds:此刻的绑定数(NimBLE 先写入新配对的绑定,再上报加密);link_bonded:连接用绑定的密钥加密。
// 返回 ACCEPT 表示已确认的主机用原有绑定重连,可以直接信任;其余情况按 ar_pair_request() 处理。
// 判断只依赖已确认的绑定数,不依赖连接建立事件:NimBLE 可能先上报加密、后上报连接建立。
ar_pair_action_t ar_pair_encrypted(ar_pair_t *p, uint16_t conn, int bonds, bool link_bonded,
                                   uint32_t now_ms);

// 待确认时的按键:ok 为 true 表示按了 OK,false 表示按了取消
ar_pair_action_t ar_pair_key(ar_pair_t *p, bool ok);

// 周期调用,处理确认超时
ar_pair_action_t ar_pair_tick(ar_pair_t *p, uint32_t now_ms);

// 连接断开:如果是待确认的那条连接,取消确认
void ar_pair_disconnected(ar_pair_t *p, uint16_t conn);

// 用户接受且配对完成后调用,之后只接受这台主机。bonds:此时的绑定数(含刚确认的这个)
void ar_pair_confirmed(ar_pair_t *p, int bonds);

// 绑定已清除
void ar_pair_cleared(ar_pair_t *p);

bool ar_pair_confirming(const ar_pair_t *p);
