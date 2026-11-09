"""会话聚合状态机测试。

覆盖设计方案 §5.2 / §5.4 的事件映射、超时回收、本地日切、乱序事件与子代理处理。
"""
import unittest

from agent_run.state import Aggregator


class FakeClock:
    """可手动推进的时钟，供超时测试使用。"""

    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


def ev(tool, event, sid, ts, **extra):
    """构造一条与 ar_hook.py 转发格式一致的事件。"""
    msg = {"tool": tool, "event": event, "sid": sid, "ts": ts}
    msg.update(extra)
    return msg


class AggregatorTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock(1000.0)
        self.day = ["2026-11-08"]
        self.agg = Aggregator(clock=self.clock, day_of=lambda t: self.day[0])

    # ---- 基本流转 -------------------------------------------------------

    def test_prompt_marks_working_and_stop_counts_done(self):
        a = self.agg
        a.handle(ev("claude", "SessionStart", "s1", 1000.0))
        self.assertEqual(a.snapshot().cc, 0)
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1001.0))
        self.assertEqual(a.snapshot().cc, 1)
        a.handle(ev("claude", "Stop", "s1", 1002.0))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (0, 1))

    def test_stop_without_work_does_not_count(self):
        a = self.agg
        a.handle(ev("claude", "SessionStart", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0))
        self.assertEqual(a.snapshot().done, 0)

    def test_tools_are_counted_separately(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "c1", 1000.0))
        a.handle(ev("claude", "UserPromptSubmit", "c2", 1000.1))
        a.handle(ev("codex", "UserPromptSubmit", "x1", 1000.2))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.cx, snap.ds), (2, 1, 0))

    def test_same_sid_in_different_tools_are_different_sessions(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "same", 1000.0))
        a.handle(ev("codex", "UserPromptSubmit", "same", 1000.1))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.cx), (1, 1))

    # ---- 等你处理 -------------------------------------------------------

    def test_claude_permission_request_waits_then_resumes(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "PermissionRequest", "s1", 1001.0, tool_name="Bash"))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.wn), (0, 1))
        self.assertEqual(snap.waiting, [("claude", "A")])
        a.handle(ev("claude", "PostToolUse", "s1", 1002.0, tool_name="Bash"))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.wn), (1, 0))

    def test_claude_question_and_plan_tools_wait(self):
        a = self.agg
        a.handle(ev("claude", "PreToolUse", "q", 1000.0, tool_name="AskUserQuestion"))
        a.handle(ev("claude", "PreToolUse", "p", 1000.1, tool_name="ExitPlanMode"))
        self.assertEqual(a.snapshot().waiting, [("claude", "Q"), ("claude", "A")])

    def test_ordinary_pre_tool_use_is_working(self):
        a = self.agg
        a.handle(ev("claude", "PreToolUse", "s1", 1000.0, tool_name="Read"))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.wn), (1, 0))

    def test_claude_notification_types(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "a", 1000.0))
        a.handle(ev("claude", "Notification", "a", 1000.1, ntype="permission_prompt"))
        a.handle(ev("claude", "UserPromptSubmit", "b", 1000.2))
        a.handle(ev("claude", "Notification", "b", 1000.3, ntype="agent_needs_input"))
        a.handle(ev("claude", "UserPromptSubmit", "c", 1000.4))
        a.handle(ev("claude", "Notification", "c", 1000.5, ntype="idle_prompt"))
        snap = a.snapshot()
        self.assertEqual(snap.waiting, [("claude", "A"), ("claude", "Q")])
        # idle_prompt 不改变状态，c 仍在工作中
        self.assertEqual(snap.cc, 1)

    def test_codex_permission_and_interrupt(self):
        a = self.agg
        a.handle(ev("codex", "UserPromptSubmit", "x", 1000.0))
        a.handle(ev("codex", "PermissionRequest", "x", 1001.0))
        self.assertEqual(a.snapshot().waiting, [("codex", "A")])
        a.handle(ev("codex", "Stop", "x", 1002.0))
        snap = a.snapshot()
        self.assertEqual((snap.wn, snap.done), (0, 1))
        a.handle(ev("codex", "UserPromptSubmit", "x", 1003.0))
        a.handle(ev("codex", "Interrupt", "x", 1004.0))
        snap = a.snapshot()
        self.assertEqual((snap.cx, snap.done), (0, 1))

    def test_stop_failure_goes_idle_without_counting(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "StopFailure", "s1", 1001.0))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (0, 0))

    def test_session_end_removes_waiting_session(self):
        a = self.agg
        a.handle(ev("claude", "PermissionRequest", "s1", 1000.0))
        a.handle(ev("claude", "SessionEnd", "s1", 1001.0))
        self.assertEqual(a.snapshot().wn, 0)
        self.assertEqual(a.session_count(), 0)

    def test_waiting_list_order_and_limit(self):
        a = self.agg
        for i, tool in enumerate(["claude", "codex", "claude", "codex", "claude"]):
            a.handle(ev(tool, "PermissionRequest", "s%d" % i, 1000.0 + i))
        snap = a.snapshot()
        self.assertEqual(snap.wn, 5)
        # snapshot 保留完整列表，按进入等待的先后排序；截断由协议层负责
        self.assertEqual([t for t, _ in snap.waiting], ["claude", "codex", "claude", "codex", "claude"])

    def test_session_start_on_working_session_keeps_state(self):
        # 自动压缩会在一轮中途触发 SessionStart，不能把工作中的会话打回空闲
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "SessionStart", "s1", 1001.0))
        self.assertEqual(a.snapshot().cc, 1)

    # ---- 子代理、乱序与非法输入 -----------------------------------------

    def test_subagent_event_does_not_create_session(self):
        a = self.agg
        a.handle(ev("claude", "PreToolUse", "s1", 1000.0, tool_name="Read", agent=True))
        self.assertEqual(a.session_count(), 0)

    def test_subagent_event_applies_to_known_parent(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "PermissionRequest", "s1", 1001.0, agent=True))
        self.assertEqual(a.snapshot().waiting, [("claude", "A")])

    def test_out_of_order_event_is_ignored(self):
        # 异步 hook 可能晚到：Stop 之后才到的旧 PostToolUse 不能把会话拉回工作中
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1002.0))
        changed = a.handle(ev("claude", "PostToolUse", "s1", 1001.5))
        self.assertFalse(changed)
        self.assertEqual(a.snapshot().cc, 0)

    def test_invalid_messages_are_ignored(self):
        a = self.agg
        self.assertFalse(a.handle({"tool": "claude", "event": "UserPromptSubmit", "ts": 1.0}))
        self.assertFalse(a.handle(ev("vim", "UserPromptSubmit", "s1", 1000.0)))
        self.assertFalse(a.handle({"tool": "claude", "event": "Stop", "sid": "s1"}))
        self.assertEqual(a.session_count(), 0)

    def test_handle_reports_change_only_when_snapshot_changes(self):
        a = self.agg
        self.assertTrue(a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0)))
        self.assertFalse(a.handle(ev("claude", "PostToolUse", "s1", 1001.0)))

    # ---- 超时回收与日切 -------------------------------------------------

    def test_working_times_out_after_15_minutes(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        self.clock.t = 1000.0 + 15 * 60 - 1
        self.assertFalse(a.tick())
        self.assertEqual(a.snapshot().cc, 1)
        self.clock.t = 1000.0 + 15 * 60 + 1
        self.assertTrue(a.tick())
        self.assertEqual(a.snapshot().cc, 0)

    def test_waiting_times_out_after_60_minutes(self):
        a = self.agg
        a.handle(ev("claude", "PermissionRequest", "s1", 1000.0))
        self.clock.t = 1000.0 + 59 * 60
        a.tick()
        self.assertEqual(a.snapshot().wn, 1)
        self.clock.t = 1000.0 + 61 * 60
        a.tick()
        self.assertEqual(a.snapshot().wn, 0)

    def test_idle_sessions_are_dropped_after_24_hours(self):
        a = self.agg
        a.handle(ev("claude", "SessionStart", "s1", 1000.0))
        self.clock.t = 1000.0 + 24 * 3600 + 1
        a.tick()
        self.assertEqual(a.session_count(), 0)

    def test_done_resets_on_new_local_day(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0))
        self.assertEqual(a.snapshot().done, 1)
        self.day[0] = "2026-11-09"
        self.assertTrue(a.tick())
        self.assertEqual(a.snapshot().done, 0)

    def test_initial_done_is_restored_for_same_day_only(self):
        same = Aggregator(clock=self.clock, day_of=lambda t: "2026-11-08", initial_done=("2026-11-08", 5))
        self.assertEqual(same.snapshot().done, 5)
        other = Aggregator(clock=self.clock, day_of=lambda t: "2026-11-08", initial_done=("2026-11-07", 5))
        self.assertEqual(other.snapshot().done, 0)
        self.assertEqual(same.done_record(), ("2026-11-08", 5))

    # ---- 后台任务(Claude Code Stop 载荷的 background_tasks) ----------------

    def test_stop_with_background_tasks_keeps_working_without_counting(self):
        # 一轮结束时仍有后台任务在跑：会话只是暂停等唤醒，继续计入工作中，不计完成
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0, bg=1))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (1, 0))

    def test_background_session_counts_done_once_when_finally_stopped(self):
        # 后台任务结束后会话被唤醒，可能再起后台任务；最后一次不带后台任务的 Stop 才计完成
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0, bg=1))
        a.handle(ev("claude", "PostToolUse", "s1", 1500.0, tool_name="Bash"))
        a.handle(ev("claude", "Stop", "s1", 1501.0, bg=2))
        self.assertEqual(a.snapshot().done, 0)
        a.handle(ev("claude", "Stop", "s1", 2000.0, bg=0))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (0, 1))

    def test_background_session_outlives_working_timeout(self):
        # 后台回放常跑 40–50 分钟且期间没有任何事件，不能按工作中的 15 分钟回收
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0, bg=1))
        self.clock.t = 1001.0 + 50 * 60
        a.tick()
        self.assertEqual(a.snapshot().cc, 1)

    def test_background_session_times_out_after_2_hours(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "s1", 1000.0))
        a.handle(ev("claude", "Stop", "s1", 1001.0, bg=1))
        self.clock.t = 1001.0 + 2 * 3600 - 1
        self.assertFalse(a.tick())
        self.clock.t = 1001.0 + 2 * 3600 + 1
        self.assertTrue(a.tick())
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (0, 0))

    def test_invalid_background_count_is_ignored(self):
        # 非整数（含布尔值）的 bg 视同没有后台任务，按普通 Stop 处理
        a = self.agg
        for i, bad in enumerate(("2", True, -1)):
            sid = "s%d" % i
            a.handle(ev("claude", "UserPromptSubmit", sid, 1000.0 + i))
            a.handle(ev("claude", "Stop", sid, 1000.5 + i, bg=bad))
        snap = a.snapshot()
        self.assertEqual((snap.cc, snap.done), (0, 3))

    # ---- 异步提问(Codex 桌面端 request_user_input_async) -------------------

    def test_async_question_stays_pending_while_agent_keeps_working(self):
        # 异步提问的工具调用立刻返回,Agent 按默认选项继续;问题在界面上一直挂着
        a = self.agg
        a.handle(ev("codex", "UserPromptSubmit", "x", 1000.0))
        a.handle(ev("codex", "PreToolUse", "x", 1001.0, tool_name="request_user_input_async"))
        a.handle(ev("codex", "PostToolUse", "x", 1001.2, tool_name="request_user_input_async"))
        a.handle(ev("codex", "PreToolUse", "x", 1005.0, tool_name="Bash"))
        snap = a.snapshot()
        self.assertEqual(snap.waiting, [("codex", "Q")])
        self.assertEqual(snap.cx, 0)   # 挂着提问的会话算“等你处理”,不再算跑速

    def test_async_question_survives_turn_end_until_user_replies(self):
        # 2026-11-08 实测:Codex 提问后 4 秒就结束了这一轮(Stop),问题卡片仍挂着,
        # 用户 4 分钟后回答,回答作为新一轮的 UserPromptSubmit 到达
        a = self.agg
        a.handle(ev("codex", "UserPromptSubmit", "x", 1000.0))
        a.handle(ev("codex", "PreToolUse", "x", 1003.0, tool_name="request_user_input_async"))
        a.handle(ev("codex", "PostToolUse", "x", 1003.2, tool_name="request_user_input_async"))
        a.handle(ev("codex", "Stop", "x", 1007.0))
        snap = a.snapshot()
        self.assertEqual((snap.waiting, snap.done), ([("codex", "Q")], 1))
        a.handle(ev("codex", "UserPromptSubmit", "x", 1270.0))
        snap = a.snapshot()
        self.assertEqual((snap.wn, snap.cx), (0, 1))

    def test_async_question_cleared_by_interrupt(self):
        a = self.agg
        a.handle(ev("codex", "PreToolUse", "x", 1000.0, tool_name="request_user_input_async"))
        a.handle(ev("codex", "Interrupt", "x", 1005.0))
        self.assertEqual(a.snapshot().wn, 0)

    def test_async_question_times_out(self):
        a = self.agg
        a.handle(ev("codex", "PreToolUse", "x", 1000.0, tool_name="request_user_input_async"))
        a.handle(ev("codex", "PostToolUse", "x", 1000.2, tool_name="request_user_input_async"))
        self.clock.t = 1000.0 + 59 * 60
        a.tick()
        self.assertEqual(a.snapshot().wn, 1)
        self.clock.t = 1000.0 + 61 * 60
        a.tick()
        self.assertEqual(a.snapshot().wn, 0)

    def test_blocking_question_tools_of_codex_and_dsh(self):
        a = self.agg
        a.handle(ev("codex", "PreToolUse", "x", 1000.0, tool_name="request_user_input"))
        a.handle(ev("dsh", "PreToolUse", "d", 1000.1, tool_name="ask_user_question"))
        self.assertEqual(a.snapshot().waiting, [("codex", "Q"), ("dsh", "Q")])
        a.handle(ev("dsh", "PostToolUse", "d", 1002.0, tool_name="ask_user_question"))
        snap = a.snapshot()
        self.assertEqual(snap.waiting, [("codex", "Q")])
        self.assertEqual(snap.ds, 1)

    # ---- dsh -----------------------------------------------------------

    def test_dsh_count_is_set_from_scanner(self):
        a = self.agg
        self.assertTrue(a.set_dsh_working(2))
        self.assertFalse(a.set_dsh_working(2))
        self.assertEqual(a.snapshot().ds, 2)

    def test_dsh_hook_events_replace_file_scan(self):
        # 收到过 dsh 的 hook 事件后,dsh 计数以事件为准,不再用文件扫描的估算
        a = self.agg
        a.set_dsh_working(3)
        a.handle(ev("dsh", "UserPromptSubmit", "d1", 1000.0))
        self.assertEqual(a.snapshot().ds, 1)
        a.set_dsh_working(5)
        self.assertEqual(a.snapshot().ds, 1)

    # ---- 按工具的计数（专用页面方案 §5.1） --------------------------------

    def test_tool_counts_split_waiting_background_and_done(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "c1", 1000.0))
        a.handle(ev("claude", "PermissionRequest", "c1", 1001.0, tool_name="Bash"))
        a.handle(ev("claude", "UserPromptSubmit", "c2", 1002.0))
        a.handle(ev("claude", "Stop", "c2", 1003.0, bg=2))          # 后台运行
        a.handle(ev("codex", "UserPromptSubmit", "x1", 1004.0))
        a.handle(ev("codex", "Stop", "x1", 1005.0))                 # 完成 1 次
        a.handle(ev("codex", "UserPromptSubmit", "x2", 1006.0))
        a.handle(ev("codex", "Stop", "x2", 1007.0))                 # 完成 2 次
        counts = a.tool_counts()
        self.assertEqual((counts["claude"].wn, counts["claude"].bg, counts["claude"].done), (1, 1, 0))
        self.assertEqual((counts["codex"].wn, counts["codex"].bg, counts["codex"].done), (0, 0, 2))
        self.assertEqual((counts["dsh"].wn, counts["dsh"].bg, counts["dsh"].done), (0, 0, 0))
        # 总数仍与快照一致
        self.assertEqual(a.snapshot().done, 2)

    def test_async_question_counts_as_waiting_for_its_tool(self):
        a = self.agg
        a.handle(ev("codex", "UserPromptSubmit", "x1", 1000.0))
        a.handle(ev("codex", "PreToolUse", "x1", 1001.0, tool_name="request_user_input_async"))
        a.handle(ev("codex", "Stop", "x1", 1002.0))
        self.assertEqual(a.tool_counts()["codex"].wn, 1)

    def test_tool_done_resets_on_new_local_day(self):
        a = self.agg
        a.handle(ev("claude", "UserPromptSubmit", "c1", 1000.0))
        a.handle(ev("claude", "Stop", "c1", 1001.0))
        self.assertEqual(a.done_by_tool()["claude"], 1)
        self.day[0] = "2026-11-09"
        a.tick()
        self.assertEqual(a.done_by_tool(), {"claude": 0, "codex": 0, "dsh": 0})
        self.assertEqual(a.tool_counts()["claude"].done, 0)

    def test_tool_done_restored_for_same_day_only(self):
        saved = {"claude": 4, "codex": 1, "bogus": 9, "dsh": "x"}
        same = Aggregator(clock=self.clock, day_of=lambda t: "2026-11-08",
                          initial_done=("2026-11-08", 5), initial_done_by_tool=saved)
        # 未知工具与非整数值丢弃
        self.assertEqual(same.done_by_tool(), {"claude": 4, "codex": 1, "dsh": 0})
        other = Aggregator(clock=self.clock, day_of=lambda t: "2026-11-08",
                           initial_done=("2026-11-07", 5), initial_done_by_tool=saved)
        self.assertEqual(other.done_by_tool(), {"claude": 0, "codex": 0, "dsh": 0})

    def test_old_state_file_without_tool_done_starts_from_zero(self):
        a = Aggregator(clock=self.clock, day_of=lambda t: "2026-11-08", initial_done=("2026-11-08", 5))
        self.assertEqual(a.snapshot().done, 5)
        self.assertEqual(a.done_by_tool(), {"claude": 0, "codex": 0, "dsh": 0})

    # ---- 诊断 -----------------------------------------------------------

    def test_recent_events_keep_only_whitelisted_fields(self):
        a = self.agg
        for i in range(30):
            a.handle(ev("claude", "PreToolUse", "s1", 1000.0 + i, tool_name="Read"))
        a.handle(ev("codex", "Notification", "x", 2000.0, ntype="idle_prompt"))
        recent = a.recent_events()
        self.assertEqual(len(recent), 20)
        self.assertEqual(recent[-1], {"ts": 2000.0, "tool": "codex", "event": "Notification", "ntype": "idle_prompt"})


if __name__ == "__main__":
    unittest.main()
