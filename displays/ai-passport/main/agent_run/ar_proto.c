// main/agent_run/ar_proto.c —— 串口协议解析实现。见 ar_proto.h。
#include "ar_proto.h"

#include <string.h>

// 必填字段的位掩码:全部出现才算合法帧
enum {
    F_SEQ = 1 << 0,
    F_CC = 1 << 1,
    F_CX = 1 << 2,
    F_DS = 1 << 3,
    F_DONE = 1 << 4,
    F_WN = 1 << 5,
    F_W = 1 << 6,
    F_ALL = (1 << 7) - 1,
};

// 解析 [start, end) 范围内的十进制无符号整数,不允许空串、符号与超过 max 的值。
static bool parse_uint(const char *start, const char *end, uint32_t max, uint32_t *out) {
    if (start >= end || end - start > 5) return false;
    uint32_t value = 0;
    for (const char *p = start; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value > max) return false;
    *out = value;
    return true;
}

// 解析等待列表:"-" 表示空,否则为最多 3 项、逗号分隔的 "XX:R"。
static bool parse_wait_list(const char *start, const char *end, ar_state_t *st) {
    st->w_count = 0;
    if (end - start == 1 && *start == '-') return true;
    const char *p = start;
    while (p < end) {
        // 每项固定 4 个字符:两位工具代号、冒号、一位原因
        if (end - p < 4 || p[2] != ':') return false;
        if (st->w_count >= AR_PROTO_MAX_WAIT) return false;
        ar_wait_item_t item;
        if (p[0] == 'C' && p[1] == 'C') item.tool = AR_TOOL_CC;
        else if (p[0] == 'C' && p[1] == 'X') item.tool = AR_TOOL_CX;
        else if (p[0] == 'D' && p[1] == 'S') item.tool = AR_TOOL_DS;
        else return false;
        if (p[3] == 'A') item.reason = AR_REASON_APPROVE;
        else if (p[3] == 'Q') item.reason = AR_REASON_QUESTION;
        else return false;
        st->w[st->w_count++] = item;
        p += 4;
        if (p == end) break;
        if (*p != ',') return false;
        p++;
        if (p == end) return false;   // 末尾多余的逗号
    }
    return true;
}

// 解析一个 key=value 记号,成功时在 seen 中置位。未知键视为成功(忽略)。
static bool parse_field(const char *start, const char *end, ar_state_t *st, uint32_t *seen) {
    const char *eq = memchr(start, '=', (size_t)(end - start));
    if (!eq) return false;
    size_t key_len = (size_t)(eq - start);
    const char *v = eq + 1;
    uint32_t n = 0;

#define KEY_IS(lit) (key_len == sizeof(lit) - 1 && memcmp(start, lit, key_len) == 0)
    if (KEY_IS("seq")) {
        if (!parse_uint(v, end, 65535, &n)) return false;
        st->seq = (uint16_t)n;
        *seen |= F_SEQ;
    } else if (KEY_IS("cc")) {
        if (!parse_uint(v, end, 99, &n)) return false;
        st->cc = (uint8_t)n;
        *seen |= F_CC;
    } else if (KEY_IS("cx")) {
        if (!parse_uint(v, end, 99, &n)) return false;
        st->cx = (uint8_t)n;
        *seen |= F_CX;
    } else if (KEY_IS("ds")) {
        if (!parse_uint(v, end, 99, &n)) return false;
        st->ds = (uint8_t)n;
        *seen |= F_DS;
    } else if (KEY_IS("done")) {
        if (!parse_uint(v, end, 999, &n)) return false;
        st->done = (uint16_t)n;
        *seen |= F_DONE;
    } else if (KEY_IS("wn")) {
        if (!parse_uint(v, end, 99, &n)) return false;
        st->wn = (uint8_t)n;
        *seen |= F_WN;
    } else if (KEY_IS("w")) {
        if (!parse_wait_list(v, end, st)) return false;
        *seen |= F_W;
    }
#undef KEY_IS
    return true;
}

ar_line_kind_t ar_proto_parse_line(const char *line, ar_state_t *out) {
    if (!line || strncmp(line, "AR1 ", 4) != 0) return AR_LINE_IGNORED;
    size_t len = strlen(line);
    if (len > AR_PROTO_MAX_LINE) return AR_LINE_IGNORED;

    const char *p = line + 4;
    const char *end = line + len;
    if (strcmp(p, "PING") == 0) return AR_LINE_PING;
    if (strncmp(p, "S ", 2) != 0) return AR_LINE_IGNORED;
    p += 2;

    // 先解析到临时结构,全部合法后再写回调用方,避免半帧污染当前状态
    ar_state_t st;
    memset(&st, 0, sizeof(st));
    uint32_t seen = 0;
    while (p < end) {
        const char *tok_end = memchr(p, ' ', (size_t)(end - p));
        if (!tok_end) tok_end = end;
        if (tok_end == p) return AR_LINE_IGNORED;   // 连续空格
        if (!parse_field(p, tok_end, &st, &seen)) return AR_LINE_IGNORED;
        p = tok_end < end ? tok_end + 1 : end;
    }
    if (seen != F_ALL) return AR_LINE_IGNORED;
    if (st.w_count > st.wn) return AR_LINE_IGNORED;
    *out = st;
    return AR_LINE_STATE;
}

void ar_line_init(ar_line_buf_t *lb) {
    lb->len = 0;
    lb->overflow = false;
    lb->buf[0] = '\0';
}

bool ar_line_push(ar_line_buf_t *lb, char c, char *out, size_t out_size) {
    if (c == '\r' || c == '\n') {
        bool ready = !lb->overflow && lb->len > 0 && out_size > lb->len;
        if (ready) {
            memcpy(out, lb->buf, lb->len);
            out[lb->len] = '\0';
        }
        lb->len = 0;
        lb->overflow = false;
        return ready;
    }
    if (lb->overflow) return false;
    if (lb->len >= AR_PROTO_MAX_LINE) {
        lb->overflow = true;   // 丢弃到下一个行尾
        lb->len = 0;
        return false;
    }
    lb->buf[lb->len++] = c;
    return false;
}
