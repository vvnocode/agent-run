"""串口协议（设计方案 §6）：主机状态帧编码与设备回包解析。"""

# 单行最大字节数（含行尾），设备端按同一上限丢弃超长行
MAX_LINE = 160

# 工具代号：上串口的只有代号，不含任何项目信息
TOOL_CODES = {"claude": "CC", "codex": "CX", "dsh": "DS"}

# 等待列表最多发送的项数；wn 字段仍是总数
MAX_WAITING_ITEMS = 3


def _clamp(value, upper):
    """把计数限制在 0..upper，防止设备端字段越界整帧丢弃。"""
    return max(0, min(upper, int(value)))


def encode_state(snap, seq):
    """把快照编码成一行状态帧，例如：

    AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q
    """
    items = []
    for tool, reason in snap.waiting:
        if len(items) >= MAX_WAITING_ITEMS:
            break
        code = TOOL_CODES.get(tool)
        if code and reason in ("A", "Q"):
            items.append("%s:%s" % (code, reason))
    waiting = ",".join(items) if items else "-"
    return "AR1 S seq=%d cc=%d cx=%d ds=%d done=%d wn=%d w=%s\n" % (
        int(seq) % 65536,
        _clamp(snap.cc, 99),
        _clamp(snap.cx, 99),
        _clamp(snap.ds, 99),
        _clamp(snap.done, 999),
        _clamp(snap.wn, 99),
        waiting,
    )


def encode_ping():
    """握手请求；设备收到后回 AR1 HELLO。"""
    return "AR1 PING\n"


def parse_device_line(line):
    """解析设备输出的一行。只认 AR1 前缀，日志等其他行返回 None。

    "AR1 HELLO fw=0.1.0 proto=1" -> {"type": "HELLO", "fw": "0.1.0", "proto": "1"}
    """
    parts = line.strip().split()
    if len(parts) < 2 or parts[0] != "AR1":
        return None
    out = {"type": parts[1]}
    for part in parts[2:]:
        if "=" in part:
            key, value = part.split("=", 1)
            out[key] = value
    return out
