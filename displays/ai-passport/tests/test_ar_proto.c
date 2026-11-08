// tests/test_ar_proto.c —— Agent Run! 串口协议解析的宿主测试。
#include <assert.h>
#include <string.h>

#include "agent_run/ar_proto.h"

// 把整段字节流喂给行装配器,返回产出的行数,最后一行留在 last
static int feed(ar_line_buf_t *lb, const char *bytes, char *last) {
    char out[AR_PROTO_MAX_LINE + 1];
    int lines = 0;
    for (const char *p = bytes; *p; p++) {
        if (ar_line_push(lb, *p, out, sizeof(out))) {
            strcpy(last, out);
            lines++;
        }
    }
    return lines;
}

static void test_valid_frame(void) {
    ar_state_t st;
    assert(ar_proto_parse_line("AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q", &st) == AR_LINE_STATE);
    assert(st.seq == 17 && st.cc == 3 && st.cx == 1 && st.ds == 1 && st.done == 21 && st.wn == 2);
    assert(st.w_count == 2);
    assert(st.w[0].tool == AR_TOOL_CC && st.w[0].reason == AR_REASON_APPROVE);
    assert(st.w[1].tool == AR_TOOL_CX && st.w[1].reason == AR_REASON_QUESTION);
}

static void test_empty_wait_list_and_unknown_keys(void) {
    ar_state_t st;
    assert(ar_proto_parse_line("AR1 S seq=0 cc=0 cx=0 ds=0 done=0 wn=0 w=-", &st) == AR_LINE_STATE);
    assert(st.w_count == 0);
    assert(ar_proto_parse_line("AR1 S seq=1 q=9 cc=1 cx=0 ds=0 done=0 wn=0 w=- future=x", &st) == AR_LINE_STATE);
    assert(st.cc == 1);
    // 主机只发前 3 项,wn 可以更大
    assert(ar_proto_parse_line("AR1 S seq=2 cc=0 cx=0 ds=0 done=0 wn=5 w=CC:A,CX:A,DS:Q", &st) == AR_LINE_STATE);
    assert(st.wn == 5 && st.w_count == 3 && st.w[2].tool == AR_TOOL_DS);
}

static void test_invalid_frames_do_not_touch_output(void) {
    ar_state_t st;
    memset(&st, 0xAB, sizeof(st));
    const char *bad[] = {
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=0",               // 缺 w
        "AR1 S seq=1 cc=100 cx=0 ds=0 done=0 wn=0 w=-",         // cc 越界
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=1000 wn=0 w=-",        // done 越界
        "AR1 S seq=70000 cc=1 cx=0 ds=0 done=0 wn=0 w=-",       // seq 越界
        "AR1 S seq=1 cc=-1 cx=0 ds=0 done=0 wn=0 w=-",          // 负号
        "AR1 S seq=1 cc= cx=0 ds=0 done=0 wn=0 w=-",            // 空值
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=1 w=CC:A,CX:Q",   // 列表项多于 wn
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=4 w=CC:A,CX:A,CC:Q,CX:Q",  // 超过 3 项
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=1 w=ZZ:A",        // 未知工具
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=1 w=CC:X",        // 未知原因
        "AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=1 w=CC:A,",       // 末尾逗号
        "AR1 S seq=1  cc=1 cx=0 ds=0 done=0 wn=0 w=-",          // 连续空格
        "AR1 S seq=1 cc cx=0 ds=0 done=0 wn=0 w=-",             // 缺等号
        "AR1 X seq=1",                                          // 未知类型
        "I (123) main: AR1 S seq=1",                            // 日志行
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(ar_proto_parse_line(bad[i], &st) == AR_LINE_IGNORED);
    }
    assert(st.seq == 0xABAB);   // 整帧丢弃时不改写输出
}

static void test_ping(void) {
    ar_state_t st;
    assert(ar_proto_parse_line("AR1 PING", &st) == AR_LINE_PING);
    assert(ar_proto_parse_line("AR1 PING extra", &st) == AR_LINE_IGNORED);
}

static void test_line_assembler(void) {
    ar_line_buf_t lb;
    char last[AR_PROTO_MAX_LINE + 1] = {0};
    ar_line_init(&lb);
    // \r\n、单独的 \r 与 \n 都算行尾,空行不产出
    assert(feed(&lb, "AR1 PING\r\n\r\nI (1) log\rAR1 S", last) == 2);
    assert(strcmp(last, "I (1) log") == 0);
    // 不完整的行留在缓冲里,收到行尾后才产出
    assert(feed(&lb, " seq=1\n", last) == 1);
    assert(strcmp(last, "AR1 S seq=1") == 0);
}

static void test_overlong_line_is_dropped_until_next_terminator(void) {
    ar_line_buf_t lb;
    char last[AR_PROTO_MAX_LINE + 1] = {0};
    char longline[AR_PROTO_MAX_LINE + 20];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    ar_line_init(&lb);
    assert(feed(&lb, longline, last) == 0);
    assert(feed(&lb, "tail\nAR1 PING\n", last) == 1);
    assert(strcmp(last, "AR1 PING") == 0);
}

int main(void) {
    test_valid_frame();
    test_empty_wait_list_and_unknown_keys();
    test_invalid_frames_do_not_touch_output();
    test_ping();
    test_line_assembler();
    test_overlong_line_is_dropped_until_next_terminator();
    return 0;
}
