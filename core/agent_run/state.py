"""会话聚合状态机（设计方案 §5.2、§5.4）。

本模块是纯逻辑：不读写文件、不碰 socket，时钟与“本地日期”函数都由调用方注入，
因此可以用假时钟完整测试超时与日切。
"""
import time
from collections import deque, namedtuple

# 通过 hook 上报事件的工具。dsh 需要启用其 hook 插件后才会上报；
# 未收到过 dsh 事件时，dsh 计数仍用会话文件扫描的估算
HOOK_TOOLS = ("claude", "codex", "dsh")

# 诊断用：保留最近这么多条事件（只含白名单字段）
RECENT_EVENTS = 20

# 会话状态
IDLE = "idle"
WORKING = "working"
WAITING = "waiting"
# 后台运行：一轮已结束，但仍有后台任务在跑，任务结束后会话会被唤醒继续。
# 计入工作中，不计完成；超时比工作中长，因为后台任务期间没有任何 hook 事件
BACKGROUND = "background"

# 等你处理的原因代号：A = 等待授权，Q = 等你回答
APPROVE = "A"
QUESTION = "Q"

# 超时回收时限（秒）。工作中 15 分钟大于 Claude Code Bash 工具 10 分钟的上限
WORKING_TIMEOUT = 15 * 60
WAITING_TIMEOUT = 60 * 60
# 后台运行 2 小时没有事件即回收：容纳常见的长回放、长构建，又不让异常退出的会话长期占着跑速
BACKGROUND_TIMEOUT = 2 * 3600
IDLE_TIMEOUT = 24 * 3600

# 可由 config.json 覆盖的映射规则：工具名或通知类型改名时不必改代码
DEFAULT_RULES = {
    # 阻塞式提问：调用期间 Agent 停下等回答，PostToolUse 到达即回答完毕
    #   Claude Code AskUserQuestion、Codex request_user_input、dsh ask_user_question
    "question_tools": ["AskUserQuestion", "request_user_input", "ask_user_question"],
    # 异步提问：工具立刻返回，Agent 按默认继续，问题一直挂着（Codex 桌面端）。
    # 挂起状态保留到用户发新消息、中断、会话结束或超时，不随一轮结束清除
    "async_question_tools": ["request_user_input_async"],
    "approve_tools": ["ExitPlanMode"],
    "notify_approve": ["permission_prompt"],
    "notify_question": ["elicitation_dialog", "elicitation_url_dialog", "agent_needs_input"],
}

# 迁移目标中的两个特殊值：结束一轮（可能计入完成）与直接回到空闲（不计完成）
_STOP = "stop"
_TO_IDLE = "to_idle"

# 对外快照：各工具工作中数量、今日完成数、等你处理总数与按先后排序的等待列表
Snapshot = namedtuple("Snapshot", "cc cx ds done wn waiting")

# 单个工具的计数（专用页面方案 §5.1）：等你处理数、后台运行数、今日完成数。
# 工作中沿用 Snapshot 的 cc / cx / ds，这里不重复
ToolCounts = namedtuple("ToolCounts", "wn bg done")


def _background_count(msg):
    """Stop 事件携带的后台任务数（ar_hook.py 转发的 bg）；缺失或不是整数时按 0 处理。"""
    bg = msg.get("bg")
    if isinstance(bg, bool) or not isinstance(bg, int):
        return 0
    return bg


def local_day(t):
    """返回时间戳 t 在本机时区下的日期字符串，用作“今日完成”的分日键。"""
    return time.strftime("%Y-%m-%d", time.localtime(t))


class _Session:
    """单个会话的运行时记录。"""

    __slots__ = ("state", "reason", "last_ts", "wait_since", "pending_since")

    def __init__(self, ts):
        self.state = IDLE
        self.reason = None          # 等你处理时的原因代号
        self.last_ts = ts           # 最近一条已采纳事件的 hook 时间戳
        self.wait_since = None      # 进入当前等待的时间，用于等待列表排序
        self.pending_since = None   # 异步提问挂起的时间；None 表示没有挂起的提问

    def is_waiting(self):
        """阻塞等待，或有挂起的异步提问，都算“等你处理”。"""
        return self.state == WAITING or self.pending_since is not None

    def waiting_item(self):
        """返回 (排序时间, 原因)。阻塞等待优先，其次是挂起的异步提问。"""
        if self.state == WAITING:
            return self.wait_since, self.reason
        return self.pending_since, QUESTION

    def to_idle(self):
        self.state = IDLE
        self.reason = None
        self.wait_since = None


class Aggregator:
    """按“工具 + session_id”聚合会话状态，并给出快照。"""

    def __init__(self, clock=time.time, day_of=local_day, initial_done=None, rules=None,
                 initial_done_by_tool=None):
        """
        clock:        返回当前时间戳的函数（测试时注入假时钟）
        day_of:       把时间戳换算成本地日期字符串的函数
        initial_done: 守护进程重启时恢复的 (日期, 完成数)；日期不是今天则作废
        rules:        覆盖 DEFAULT_RULES 的映射规则
        initial_done_by_tool: 与 initial_done 同日的按工具完成数 {工具: 数量}；
                      旧版 state.json 没有这一项时为 None，按 0 处理
        """
        self._clock = clock
        self._day_of = day_of
        self._sessions = {}
        self._dsh_working = 0

        merged = dict(DEFAULT_RULES)
        merged.update(rules or {})
        self._question_tools = set(merged["question_tools"])
        self._async_question_tools = set(merged["async_question_tools"])
        self._approve_tools = set(merged["approve_tools"])
        self._dsh_hooks_seen = False   # 收到过 dsh hook 事件后，dsh 计数以事件为准
        self._recent = deque(maxlen=RECENT_EVENTS)
        self._notify_approve = set(merged["notify_approve"])
        self._notify_question = set(merged["notify_question"])

        self._day = day_of(clock())
        self._done = 0
        self._done_by_tool = dict.fromkeys(HOOK_TOOLS, 0)
        if initial_done and initial_done[0] == self._day:
            self._done = int(initial_done[1])
            # 只恢复已知工具的非负整数，文件被改坏时不让异常值进入计数
            for tool, count in (initial_done_by_tool or {}).items():
                if tool in self._done_by_tool and not isinstance(count, bool) and isinstance(count, int):
                    self._done_by_tool[tool] = max(0, count)

    # ------------------------------------------------------------------
    # 输入
    # ------------------------------------------------------------------

    def handle(self, msg):
        """处理一条 hook 事件；快照发生变化时返回 True。"""
        before = self.snapshot()
        self._apply(msg)
        return self.snapshot() != before

    def tick(self):
        """执行超时回收与日切；快照发生变化时返回 True。"""
        before = self.snapshot()
        now = self._clock()
        self._roll_day(now)
        for key, s in list(self._sessions.items()):
            quiet = now - s.last_ts
            if s.pending_since is not None and now - s.pending_since > WAITING_TIMEOUT:
                s.pending_since = None
            if s.state == WORKING and quiet > WORKING_TIMEOUT:
                s.to_idle()
            elif s.state == WAITING and quiet > WAITING_TIMEOUT:
                s.to_idle()
            elif s.state == BACKGROUND and quiet > BACKGROUND_TIMEOUT:
                s.to_idle()
            elif s.state == IDLE and quiet > IDLE_TIMEOUT:
                del self._sessions[key]
        return self.snapshot() != before

    def set_dsh_working(self, count):
        """设置 dsh 工作中的会话数（来自文件扫描）；有变化时返回 True。"""
        count = max(0, int(count))
        changed = count != self._dsh_working
        self._dsh_working = count
        return changed

    # ------------------------------------------------------------------
    # 输出
    # ------------------------------------------------------------------

    def snapshot(self):
        """生成当前快照。等待列表按进入等待的先后排序，同时刻保持插入顺序。"""
        working = {"claude": 0, "codex": 0, "dsh": 0}
        waiting = []
        for (tool, _sid), s in self._sessions.items():
            if s.is_waiting():
                since, reason = s.waiting_item()
                waiting.append((since, tool, reason))
            elif s.state in (WORKING, BACKGROUND):
                working[tool] += 1
        waiting.sort(key=lambda item: item[0])
        items = [(tool, reason) for _, tool, reason in waiting]
        ds = working["dsh"] if self._dsh_hooks_seen else self._dsh_working
        return Snapshot(cc=working["claude"], cx=working["codex"], ds=ds, done=self._done,
                        wn=len(items), waiting=items)

    def tool_counts(self):
        """按工具给出等你处理、后台运行、今日完成三项计数，供专用页面使用。

        统计口径与 snapshot() 一致：挂起的异步提问算等你处理；后台运行的会话
        同时计入 snapshot() 的工作中，这里单独给出其数量。
        """
        wn = dict.fromkeys(HOOK_TOOLS, 0)
        bg = dict.fromkeys(HOOK_TOOLS, 0)
        for (tool, _sid), s in self._sessions.items():
            if s.is_waiting():
                wn[tool] += 1
            elif s.state == BACKGROUND:
                bg[tool] += 1
        return {tool: ToolCounts(wn=wn[tool], bg=bg[tool], done=self._done_by_tool[tool])
                for tool in HOOK_TOOLS}

    def done_by_tool(self):
        """按工具的今日完成数（副本），供守护进程写入 state.json。"""
        return dict(self._done_by_tool)

    def recent_events(self):
        """最近收到的事件（只含时间、工具、事件名、工具名、通知类型），供排查使用。"""
        return list(self._recent)

    def done_record(self):
        """返回 (日期, 完成数)，供守护进程持久化。"""
        return (self._day, self._done)

    def session_count(self):
        """内存中登记的会话数（含空闲），主要供测试使用。"""
        return len(self._sessions)

    # ------------------------------------------------------------------
    # 内部实现
    # ------------------------------------------------------------------

    def _roll_day(self, now):
        """本地日期变化时把今日完成数清零。"""
        today = self._day_of(now)
        if today != self._day:
            self._day = today
            self._done = 0
            self._done_by_tool = dict.fromkeys(HOOK_TOOLS, 0)

    def _apply(self, msg):
        """校验事件并把它作用到对应会话上。"""
        if not isinstance(msg, dict):
            return
        tool = msg.get("tool")
        sid = msg.get("sid")
        event = msg.get("event")
        ts = msg.get("ts")
        if tool not in HOOK_TOOLS or not isinstance(sid, str) or not sid:
            return
        if not isinstance(event, str) or isinstance(ts, bool) or not isinstance(ts, (int, float)):
            return

        record = {"ts": ts, "tool": tool, "event": event}
        for field in ("tool_name", "ntype"):
            if isinstance(msg.get(field), str):
                record[field] = msg[field]
        self._recent.append(record)
        if tool == "dsh":
            self._dsh_hooks_seen = True

        key = (tool, sid)
        s = self._sessions.get(key)
        if s is None:
            # 子代理事件不新建会话；未登记会话的结束事件没有意义
            if msg.get("agent") or event == "SessionEnd":
                return
            s = _Session(ts)
            self._sessions[key] = s
        elif ts < s.last_ts:
            # 异步 hook 可能乱序到达：比已采纳事件更早的事件一律丢弃
            return
        s.last_ts = ts

        if event == "SessionEnd":
            del self._sessions[key]
            return

        # 异步提问：记下挂起时间。Codex 常在提问后几秒就结束这一轮，问题卡片仍挂着，
        # 所以 Stop 不清除；用户回答（作为新一轮的 UserPromptSubmit 到达）或中断时才清除
        if event == "PreToolUse" and msg.get("tool_name") in self._async_question_tools:
            if s.pending_since is None:
                s.pending_since = ts
        elif event in ("UserPromptSubmit", "Interrupt"):
            s.pending_since = None

        target = self._target(event, msg)
        if target is None:
            return  # 只刷新活动时间（如 SessionStart、未关注的通知类型）
        if target == _STOP:
            if _background_count(msg) > 0:
                # 仍有后台任务：会话只是暂停等唤醒，留到最后一次不带后台任务的 Stop 再计完成
                s.state = BACKGROUND
                s.reason = None
                s.wait_since = None
                return
            self._roll_day(self._clock())
            if s.state != IDLE:
                self._done += 1
                self._done_by_tool[tool] += 1
            s.to_idle()
        elif target == _TO_IDLE:
            s.to_idle()
        elif target == WORKING:
            s.state = WORKING
            s.reason = None
            s.wait_since = None
        else:
            # target 为 (WAITING, 原因)；原因不变时保留原进入时间，排序才稳定
            _, reason = target
            if s.state != WAITING or s.reason != reason:
                s.wait_since = ts
            s.state = WAITING
            s.reason = reason

    def _target(self, event, msg):
        """按设计方案 §5.2 的映射表，返回事件对应的迁移目标；None 表示只刷新时间。"""
        if event in ("UserPromptSubmit", "PostToolUse", "PostToolUseFailure", "PermissionDenied"):
            return WORKING
        if event == "PreToolUse":
            name = msg.get("tool_name")
            if name in self._question_tools:
                return (WAITING, QUESTION)
            if name in self._approve_tools:
                return (WAITING, APPROVE)
            return WORKING
        if event == "PermissionRequest":
            return (WAITING, APPROVE)
        if event == "Notification":
            ntype = msg.get("ntype")
            if ntype in self._notify_approve:
                return (WAITING, APPROVE)
            if ntype in self._notify_question:
                return (WAITING, QUESTION)
            return None
        if event == "Stop":
            return _STOP
        if event in ("StopFailure", "Interrupt"):
            return _TO_IDLE
        return None
