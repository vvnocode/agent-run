"""订阅用量采集（专用页面方案 §3、§5.3）。

为每个工具给出两个限额窗口：5 小时与每周。每个窗口取下列三者之一：
- Window(used, resets_at)：已用百分比（0–100）与重置时间（Unix 秒，未知时为 None）；
- NO_LIMIT：读到了用量记录，但账户没有这一窗口（如 Codex prolite 套餐没有 5 小时限额）；
- None：暂无数据。

数据来源：
- Codex：读 ~/.codex/sessions 下最近修改的会话文件末尾，取最后一条 token_count 事件的
  rate_limits。primary / secondary 不固定对应 5 小时与每周，按 window_minutes 归类。
- Claude Code：用钥匙串中 Claude Code 的 OAuth 访问令牌请求订阅用量接口（非公开接口）。
  令牌只在内存里用于本次请求：不写盘、不进日志、不进结果；只发 GET；绝不刷新令牌。
"""
import calendar
import hashlib
import http.client
import json
import logging
import os
import re
import subprocess
import threading
import time
import urllib.error
import urllib.request
from collections import namedtuple
from datetime import datetime, timezone

from . import __version__

log = logging.getLogger("agent-run")

# 单个限额窗口：已用百分比与重置时间（Unix 秒或 None）
Window = namedtuple("Window", "used resets_at")

# 账户没有这一窗口（区别于 None 表示的“暂无数据”）
NO_LIMIT = "none"

# 单个工具的用量：两个窗口、数据时间（Unix 秒）与最近一次失败原因
ToolUsage = namedtuple("ToolUsage", "five_hour seven_day updated error")
EMPTY = ToolUsage(None, None, None, None)

# 窗口长度（分钟）
FIVE_HOUR_MINUTES = 300
WEEK_MINUTES = 10080

# Codex：扫描周期、Stop 后提前扫描的最小间隔、只看最近几天修改过的文件、最多读几个文件、每个读多少字节
CODEX_SCAN_SECONDS = 30.0
CODEX_MIN_GAP_SECONDS = 5.0
CODEX_RECENT_DAYS = 7
CODEX_MAX_FILES = 3
CODEX_TAIL_BYTES = 512 * 1024

# Claude Code：用量接口、钥匙串条目名与各项时限
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CLAUDE_KEYCHAIN_SERVICE = "Claude Code-credentials"
CLAUDE_POLL_SECONDS = 300.0        # 正常每 5 分钟请求一次
CLAUDE_MIN_GAP_SECONDS = 60.0      # Stop 后提前刷新，但两次请求至少间隔 60 秒
CLAUDE_MAX_BACKOFF_SECONDS = 1200.0  # 限流或网络错误按 5、10、20 分钟退避
CLAUDE_STALE_SECONDS = 1800.0      # 超过 30 分钟没有成功更新，数据作废，显示“暂无数据”
CLAUDE_TIMEOUT_SECONDS = 10.0

# 失败原因（显示在 agent-run status 里）
NOT_LOGGED_IN = "钥匙串中没有 Claude Code 的登录令牌（未登录 Claude Code CLI）"
TOKEN_EXPIRED = "Claude Code 的登录令牌已过期，等待 Claude Code 下次运行时刷新"

_FRACTION = re.compile(r"\.(\d+)")


def _is_number(value):
    """是否为数值（排除 bool，JSON 里的 true/false 不算数值）。"""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _parse_time(value):
    """把重置时间换算成 Unix 秒：接受 Unix 秒、Unix 毫秒与 ISO 8601 字符串；无法识别时返回 None。"""
    if _is_number(value):
        return float(value) / 1000.0 if value > 1e11 else float(value)
    if not isinstance(value, str) or not value:
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    # Python 3.9 的 fromisoformat 只认 3 位或 6 位小数，统一补齐或截断到 6 位
    text = _FRACTION.sub(lambda m: "." + (m.group(1) + "000000")[:6], text, count=1)
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return calendar.timegm(parsed.utctimetuple()) + parsed.microsecond / 1e6


# ----------------------------------------------------------------------
# Codex
# ----------------------------------------------------------------------

def classify_codex_limits(rate_limits):
    """把 Codex 的 rate_limits 按窗口长度归类，返回 (5 小时窗口, 每周窗口)。"""
    five = seven = NO_LIMIT
    for key in ("primary", "secondary"):
        entry = rate_limits.get(key)
        if not isinstance(entry, dict) or not _is_number(entry.get("used_percent")):
            continue
        window = Window(float(entry["used_percent"]), _parse_time(entry.get("resets_at")))
        minutes = entry.get("window_minutes")
        if minutes == FIVE_HOUR_MINUTES:
            five = window
        elif minutes == WEEK_MINUTES:
            seven = window
    return five, seven


def _last_codex_event(path, tail_bytes, fallback_ts):
    """读一个会话文件的末尾，返回 (事件时间, 5 小时窗口, 每周窗口)；没有可用事件时返回 None。"""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            offset = max(0, size - tail_bytes)
            f.seek(offset)
            data = f.read()
    except OSError:
        return None
    lines = data.split(b"\n")
    if offset > 0:
        lines = lines[1:]   # 从文件中间开始读时，第一行不完整
    # 从后往前找：最后一条带 rate_limits 的 token_count 就是最新的用量
    for line in reversed(lines):
        if b'"token_count"' not in line:
            continue
        try:
            event = json.loads(line.decode("utf-8"))
        except ValueError:
            continue    # 正在写入的末行可能不完整
        payload = event.get("payload") if isinstance(event, dict) else None
        if not isinstance(payload, dict) or payload.get("type") != "token_count":
            continue
        limits = payload.get("rate_limits")
        if not isinstance(limits, dict) or limits.get("limit_id") not in (None, "codex"):
            continue
        five, seven = classify_codex_limits(limits)
        ts = _parse_time(event.get("timestamp"))
        return (ts if ts is not None else fallback_ts), five, seven
    return None


def read_codex_usage(root, now, days=CODEX_RECENT_DAYS, max_files=CODEX_MAX_FILES, tail_bytes=CODEX_TAIL_BYTES):
    """读 Codex 会话文件中的最新用量；没有找到时返回 None。

    只看最近 days 天修改过的文件，按修改时间取最新 max_files 个，各读末尾 tail_bytes 字节；
    再在这些文件里取事件时间最晚的一条。
    """
    cutoff = now - days * 86400
    candidates = []
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            if not (name.startswith("rollout-") and name.endswith(".jsonl")):
                continue
            path = os.path.join(dirpath, name)
            try:
                mtime = os.stat(path).st_mtime
            except OSError:
                continue
            if mtime >= cutoff:
                candidates.append((mtime, path))
    candidates.sort(reverse=True)

    best = None
    for mtime, path in candidates[:max_files]:
        found = _last_codex_event(path, tail_bytes, mtime)
        if found is not None and (best is None or found[0] > best[0]):
            best = found
    if best is None:
        return None
    return ToolUsage(five_hour=best[1], seven_day=best[2], updated=best[0], error=None)


# ----------------------------------------------------------------------
# Claude Code
# ----------------------------------------------------------------------

def read_claude_credentials():
    """从钥匙串读出 Claude Code 的凭据 JSON 文本；读不到时返回空串。

    用系统自带的 security 命令读取：Claude Code 也是用它写入的，通常不会弹出授权窗口。
    """
    result = subprocess.run(
        ["/usr/bin/security", "find-generic-password", "-s", CLAUDE_KEYCHAIN_SERVICE, "-w"],
        capture_output=True, text=True, timeout=CLAUDE_TIMEOUT_SECONDS,
    )
    return result.stdout if result.returncode == 0 else ""


def parse_claude_credentials(text, now):
    """从凭据 JSON 中取出访问令牌，返回 (令牌, None) 或 (None, 失败原因)。过期的令牌不使用。"""
    try:
        data = json.loads(text)
    except (TypeError, ValueError):
        return None, NOT_LOGGED_IN
    oauth = data.get("claudeAiOauth") if isinstance(data, dict) else None
    token = oauth.get("accessToken") if isinstance(oauth, dict) else None
    if not isinstance(token, str) or not token:
        return None, NOT_LOGGED_IN
    expires = oauth.get("expiresAt")     # Unix 毫秒
    if _is_number(expires) and expires / 1000.0 <= now:
        return None, TOKEN_EXPIRED
    return token, None


def _claude_window(entry):
    """用量接口里的单个窗口：缺失或为 null 表示没有这一窗口，用量字段不是数值表示暂无数据。"""
    if entry is None:
        return NO_LIMIT
    if not isinstance(entry, dict) or not _is_number(entry.get("utilization")):
        return None
    return Window(float(entry["utilization"]), _parse_time(entry.get("resets_at")))


def parse_claude_usage(body):
    """解析用量接口的响应，返回 (5 小时窗口, 每周窗口)；响应不是对象时抛出 ValueError。"""
    if not isinstance(body, dict):
        raise ValueError("用量接口响应不是 JSON 对象")
    return _claude_window(body.get("five_hour")), _claude_window(body.get("seven_day"))


def fetch_claude_usage(token):
    """请求一次用量接口，返回 (HTTP 状态码, 响应体)。网络错误照常抛出。"""
    request = urllib.request.Request(CLAUDE_USAGE_URL, method="GET", headers={
        "Authorization": "Bearer " + token,
        "anthropic-beta": "oauth-2025-04-20",
        "Accept": "application/json",
        "User-Agent": "agent-run/%s" % __version__,
    })
    try:
        with urllib.request.urlopen(request, timeout=CLAUDE_TIMEOUT_SECONDS) as response:
            return response.status, response.read(65536)
    except urllib.error.HTTPError as exc:
        # 只取状态码；错误响应体不保留
        return exc.code, b""


class ClaudePoller:
    """Claude Code 用量的轮询与限流（方案 §5.3）。

    poll() 在后台线程里调用，result() 在守护进程主循环里调用，内部状态用锁保护。
    """

    def __init__(self, read_credentials=read_claude_credentials, fetch=fetch_claude_usage):
        self._read_credentials = read_credentials
        self._fetch = fetch
        self._lock = threading.Lock()
        self._next = 0.0            # 下次请求的最早时间
        self._last_request = None   # 上次真正发出请求的时间
        self._failures = 0          # 连续限流或网络错误的次数，决定退避时长
        self._bad_token = None      # 收到 401/403 的令牌摘要；令牌变化前不再请求
        self._five = None
        self._seven = None
        self._updated = None
        self._error = None

    def due(self, now):
        """是否到了下次请求的时间。"""
        with self._lock:
            return now >= self._next

    def request_refresh(self, now):
        """Claude Code 一轮结束后提前刷新；退避或等新令牌期间不提前。"""
        with self._lock:
            if self._failures or self._bad_token:
                return
            earliest = now if self._last_request is None else max(now, self._last_request + CLAUDE_MIN_GAP_SECONDS)
            self._next = min(self._next, earliest)

    def poll(self, now):
        """读令牌并最多请求一次用量接口，更新结果与下次请求时间。"""
        try:
            text = self._read_credentials()
        except (OSError, subprocess.SubprocessError):
            text = ""
        token, error = parse_claude_credentials(text, now)
        if token is None:
            self._wait(now, error)
            return
        # 令牌只以摘要形式留在内存里，用来判断“令牌是否已经换过”
        digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
        with self._lock:
            if digest == self._bad_token:
                self._next = now + CLAUDE_POLL_SECONDS
                return
            self._bad_token = None
            self._last_request = now
        try:
            status, body = self._fetch(token)
        except (OSError, ValueError, http.client.HTTPException) as exc:
            # 异常信息里可能带有请求内容，只记录异常类型
            self._fail(now, "网络错误（%s）" % type(exc).__name__)
            return
        if status in (401, 403):
            with self._lock:
                self._bad_token = digest
            self._wait(now, "用量接口返回 HTTP %d，等待 Claude Code 更新登录令牌" % status)
            return
        if status != 200:
            self._fail(now, "用量接口返回 HTTP %d" % status)
            return
        try:
            five, seven = parse_claude_usage(json.loads(body.decode("utf-8")))
        except (UnicodeDecodeError, ValueError):
            self._fail(now, "用量接口响应无法解析")
            return
        with self._lock:
            recovered = self._error is not None or self._updated is None
            self._five, self._seven, self._updated, self._error = five, seven, now, None
            self._failures = 0
            self._next = now + CLAUDE_POLL_SECONDS
        if recovered:
            log.info("Claude Code 用量已更新")

    def result(self, now):
        """当前结果；超过 30 分钟没有成功更新时，两个窗口都按暂无数据处理。"""
        with self._lock:
            fresh = self._updated is not None and now - self._updated <= CLAUDE_STALE_SECONDS
            five, seven = (self._five, self._seven) if fresh else (None, None)
            return ToolUsage(five, seven, self._updated, self._error)

    def _wait(self, now, error):
        """不属于退避的失败（未登录、令牌过期或失效）：按正常周期再检查。"""
        self._set_error(error)
        with self._lock:
            self._failures = 0
            self._next = now + CLAUDE_POLL_SECONDS

    def _fail(self, now, reason):
        """限流、网络错误或响应异常：按 5、10、20 分钟退避。"""
        with self._lock:
            self._failures += 1
            delay = min(CLAUDE_POLL_SECONDS * (2 ** (self._failures - 1)), CLAUDE_MAX_BACKOFF_SECONDS)
            self._next = now + delay
        self._set_error("%s，%d 分钟后重试" % (reason, delay // 60))

    def _set_error(self, error):
        """记录失败原因；原因变化时写一条日志，避免每 5 分钟重复刷屏。"""
        with self._lock:
            changed = error != self._error
            self._error = error
        if changed:
            log.info("Claude Code 用量未更新：%s", error)


# ----------------------------------------------------------------------
# 汇总
# ----------------------------------------------------------------------

class UsageCollector:
    """汇总两个工具的用量：Codex 在主循环里扫描，Claude Code 在后台线程里请求。"""

    def __init__(self, codex_root=None, claude=None, clock=time.time, threaded=True):
        """
        codex_root: Codex 会话目录，默认 ~/.codex/sessions
        claude:     Claude Code 轮询器（测试时注入替身）
        clock:      时钟函数
        threaded:   True 时 Claude Code 请求在后台线程里执行；False 时在 step() 里同步执行（测试用）
        """
        self._codex_root = codex_root or os.path.expanduser("~/.codex/sessions")
        self._claude = claude if claude is not None else ClaudePoller()
        self._clock = clock
        self._threaded = threaded
        self._codex = None
        self._next_codex = 0.0
        self._last_codex_scan = None
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        """启动后台线程（threaded 为 True 时）。"""
        if self._threaded and self._thread is None:
            self._thread = threading.Thread(target=self._worker, name="agent-run-usage", daemon=True)
            self._thread.start()

    def close(self):
        """通知后台线程退出；线程是守护线程，进程退出时不等待正在进行的请求。"""
        self._stop.set()

    def step(self, now):
        """主循环每轮调用：到期时扫描 Codex 会话文件。"""
        if now >= self._next_codex:
            self._last_codex_scan = now
            self._next_codex = now + CODEX_SCAN_SECONDS
            self._codex = read_codex_usage(self._codex_root, now)
        if not self._threaded and self._claude.due(now):
            self._claude.poll(now)

    def on_stop(self, tool, now):
        """某个工具一轮结束：用量可能刚变化，提前刷新。"""
        if tool == "claude":
            self._claude.request_refresh(now)
        elif tool == "codex":
            earliest = now if self._last_codex_scan is None else max(now, self._last_codex_scan + CODEX_MIN_GAP_SECONDS)
            self._next_codex = min(self._next_codex, earliest)

    def get(self, tool):
        """返回某个工具的 ToolUsage；不支持的工具返回全空。"""
        if tool == "claude":
            return self._claude.result(self._clock()) or EMPTY
        if tool == "codex":
            return self._codex or EMPTY
        return EMPTY

    def _worker(self):
        """后台线程：每秒检查一次是否该请求 Claude Code 用量。"""
        while not self._stop.is_set():
            now = self._clock()
            if self._claude.due(now):
                self._claude.poll(now)
            self._stop.wait(1.0)
