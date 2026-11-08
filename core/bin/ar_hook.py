#!/usr/bin/env python3
"""Agent Run! 的 hook 入口（设计方案 §5.2）。

由 Claude Code / Codex 以 `python3 ar_hook.py <claude|codex>` 调用：
从 stdin 读取 hook JSON，只保留白名单字段，作为一个 Unix 数据报发给 agent-run 守护进程。

契约：
- 无论成功失败都以退出码 0 结束，不向 stdout / stderr 输出任何内容；
- 守护进程不在、JSON 异常、socket 写满时静默丢弃；
- 提示词、路径、工具参数等一律不转发。

本文件刻意不依赖 agent_run 包，保证被各种工作目录调用时都能快速启动。
"""
import json
import os
import socket
import sys
import time

# 认可的工具名，对应命令行第一个参数（dsh 通过其 Claude Code 兼容 hook 插件调用）
TOOLS = ("claude", "codex", "dsh")

# 字段长度上限：异常长的值直接整条丢弃
MAX_FIELD = 200

# 单条数据报上限（字节）
MAX_DATAGRAM = 2048

# stdin 读取上限：hook 载荷可能带大段工具输出，只读前 1 MB 足够解析出所需字段
MAX_STDIN = 1 << 20


def _short_str(value):
    """value 是非空且不超长的字符串时返回它，否则返回 None。"""
    if isinstance(value, str) and value and len(value) <= MAX_FIELD:
        return value
    return None


def reduce_payload(payload, tool, now):
    """把 hook 载荷裁剪成守护进程需要的最小字段集；缺少关键字段时返回 None。"""
    if not isinstance(payload, dict):
        return None
    sid = _short_str(payload.get("session_id"))
    event = _short_str(payload.get("hook_event_name"))
    if sid is None or event is None:
        return None
    msg = {"tool": tool, "event": event, "sid": sid, "ts": now}
    tool_name = _short_str(payload.get("tool_name"))
    if tool_name is not None:
        msg["tool_name"] = tool_name
    ntype = _short_str(payload.get("notification_type"))
    if ntype is not None:
        msg["ntype"] = ntype
    # 子代理事件只标记来源，不转发子代理的 id 与类型
    if payload.get("agent_id"):
        msg["agent"] = True
    return msg


def _socket_path():
    """守护进程事件入口：$AGENT_RUN_HOME/run/ar.sock，默认 ~/.agent-run。"""
    home = os.environ.get("AGENT_RUN_HOME") or os.path.expanduser("~/.agent-run")
    return os.path.join(home, "run", "ar.sock")


def main(argv):
    # 时间戳取进程启动时刻，尽量贴近事件发生时间，供守护进程判断乱序
    now = time.time()
    try:
        tool = argv[1] if len(argv) > 1 else ""
        if tool not in TOOLS:
            return 0
        raw = sys.stdin.buffer.read(MAX_STDIN)
        msg = reduce_payload(json.loads(raw.decode("utf-8")), tool, now)
        if msg is None:
            return 0
        data = json.dumps(msg, separators=(",", ":")).encode("utf-8")
        if len(data) > MAX_DATAGRAM:
            return 0
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            sock.settimeout(0.1)
            sock.sendto(data, _socket_path())
        finally:
            sock.close()
    except Exception:
        # 任何异常都不能影响 Agent：静默结束
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
