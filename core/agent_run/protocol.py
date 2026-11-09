"""串口协议（设计方案 §6）：主机状态帧编码与设备回包解析。"""
import math
import re

from .usage import NO_LIMIT

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


# A 帧里距重置分钟数的上限（两周），保证字段宽度固定在 5 位以内
MAX_RESET_MINUTES = 20160


def _usage_fields(window, now):
    """把一个限额窗口编码成 (用量, 距重置分钟数) 两个字段。

    暂无数据为 ("-", "-")，无此限额为 ("x", "-")；重置时间已过时用量按 0、重置时间未知
    （专用页面方案 §3.4）。分钟数向上取整，还剩几十秒时不会显示 0 分钟。
    """
    if window is None:
        return "-", "-"
    if window == NO_LIMIT:
        return "x", "-"
    used, resets_at = window
    if resets_at is not None and resets_at <= now:
        return "0", "-"
    percent = max(0, min(100, int(math.floor(used + 0.5))))
    if resets_at is None:
        return str(percent), "-"
    minutes = min(MAX_RESET_MINUTES, int(math.ceil((resets_at - now) / 60.0)))
    return str(percent), str(minutes)


# A 帧里套餐名的最大长度：设备标题行在工具名之后放得下的字符数
MAX_PLAN_CHARS = 10
_PLAN_UNSAFE = re.compile(r"[^A-Za-z0-9 ]")


def _plan_field(plan):
    """把套餐显示名编码成 p 字段的值，例如 "Pro Lite" -> "Pro_Lite"。

    只保留字母、数字与空格，连续空格合并后换成下划线（帧内字段以空格分隔），最长 10 个字符，
    截断处不留下划线。None、空串或只有符号时返回 None，这一帧不带 p。
    """
    if not isinstance(plan, str):
        return None
    value = "_".join(_PLAN_UNSAFE.sub("", plan).split())[:MAX_PLAN_CHARS].rstrip("_")
    return value or None


def encode_agent(tool, counts, five_hour, seven_day, now, plan=None):
    """把单个工具的计数、用量与套餐编码成一行 A 帧（专用页面方案 §5.2、§9），例如：

    AR1 A t=CX wn=0 bg=0 done=3 h=x hr=- w=5 wr=6110 p=Pro_Lite

    counts 为 state.ToolCounts；five_hour、seven_day 为 usage 模块的窗口值；now 为当前 Unix 秒；
    plan 为套餐显示名，未知时不带 p 字段（与不认识 p 的旧版本采集程序发出的帧相同）。
    只含计数、百分比、分钟数与套餐名，不含账号、令牌或任何会话内容。
    """
    h, hr = _usage_fields(five_hour, now)
    w, wr = _usage_fields(seven_day, now)
    line = "AR1 A t=%s wn=%d bg=%d done=%d h=%s hr=%s w=%s wr=%s" % (
        TOOL_CODES[tool],
        _clamp(counts.wn, 99),
        _clamp(counts.bg, 99),
        _clamp(counts.done, 999),
        h, hr, w, wr,
    )
    p = _plan_field(plan)
    if p:
        line += " p=" + p
    return line + "\n"


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
