"""串口协议编码与设备回包解析测试（设计方案 §6）。"""
import unittest

from agent_run.protocol import MAX_LINE, encode_agent, encode_ping, encode_state, parse_device_line
from agent_run.state import Snapshot, ToolCounts
from agent_run.usage import NO_LIMIT, Window


def snap(cc=0, cx=0, ds=0, done=0, waiting=None):
    waiting = waiting or []
    return Snapshot(cc=cc, cx=cx, ds=ds, done=done, wn=len(waiting), waiting=waiting)


class EncodeStateTest(unittest.TestCase):
    def test_matches_spec_example(self):
        s = snap(cc=3, cx=1, ds=1, done=21, waiting=[("claude", "A"), ("codex", "Q")])
        self.assertEqual(
            encode_state(s, 17),
            "AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q\n",
        )

    def test_no_waiting_uses_dash(self):
        self.assertTrue(encode_state(snap(), 0).endswith(" wn=0 w=-\n"))

    def test_waiting_list_truncated_to_three_but_total_kept(self):
        waiting = [("claude", "A"), ("codex", "A"), ("claude", "Q"), ("codex", "Q"), ("claude", "A")]
        line = encode_state(snap(waiting=waiting), 1)
        self.assertIn(" wn=5 ", line)
        self.assertTrue(line.endswith(" w=CC:A,CX:A,CC:Q\n"))

    def test_values_are_clamped(self):
        s = snap(cc=150, cx=-3, ds=100, done=5000)
        line = encode_state(s, 70000)
        self.assertIn("seq=%d " % (70000 % 65536), line)
        self.assertIn(" cc=99 cx=0 ds=99 done=999 ", line)

    def test_dsh_code(self):
        line = encode_state(snap(waiting=[("dsh", "Q")]), 2)
        self.assertTrue(line.endswith(" w=DS:Q\n"))

    def test_line_never_exceeds_limit(self):
        waiting = [("claude", "A")] * 99
        s = Snapshot(cc=99, cx=99, ds=99, done=999, wn=99, waiting=waiting)
        self.assertLessEqual(len(encode_state(s, 65535).encode("ascii")), MAX_LINE)


class EncodeAgentTest(unittest.TestCase):
    """工具专用页面的 A 帧（专用页面方案 §5.2）。"""

    NOW = 1794192000

    def encode(self, tool="claude", counts=ToolCounts(wn=0, bg=0, done=0), h=None, w=None, plan=None):
        return encode_agent(tool, counts, h, w, self.NOW, plan)

    def test_matches_spec_example(self):
        line = self.encode("codex", ToolCounts(wn=0, bg=0, done=3), NO_LIMIT, Window(5.0, self.NOW + 6110 * 60))
        self.assertEqual(line, "AR1 A t=CX wn=0 bg=0 done=3 h=x hr=- w=5 wr=6110\n")

    def test_plan_is_appended_with_underscores_for_spaces(self):
        line = self.encode("codex", ToolCounts(wn=0, bg=0, done=3), NO_LIMIT, Window(5.0, self.NOW + 6110 * 60),
                           plan="Pro Lite")
        self.assertEqual(line, "AR1 A t=CX wn=0 bg=0 done=3 h=x hr=- w=5 wr=6110 p=Pro_Lite\n")
        self.assertTrue(self.encode(plan="Max 20x").endswith(" wr=- p=Max_20x\n"))

    def test_plan_is_sanitized_and_cut_to_ten_characters(self):
        # 只保留字母、数字与单个空格；超过 10 个字符截断，截断处不留下划线
        self.assertTrue(self.encode(plan="  Pro  Lite! ").endswith(" p=Pro_Lite\n"))
        self.assertTrue(self.encode(plan="Self Serve Business Prolite").endswith(" p=Self_Serve\n"))
        self.assertTrue(self.encode(plan="Ninechars XY").endswith(" p=Ninechars\n"))

    def test_unknown_plan_is_omitted(self):
        for plan in (None, "", "!!!"):
            self.assertTrue(self.encode(plan=plan).endswith(" wr=-\n"), plan)

    def test_unknown_usage_is_dash(self):
        line = self.encode(counts=ToolCounts(wn=1, bg=2, done=4))
        self.assertEqual(line, "AR1 A t=CC wn=1 bg=2 done=4 h=- hr=- w=- wr=-\n")

    def test_percent_is_rounded_and_clamped(self):
        later = self.NOW + 3600
        self.assertIn(" h=23 ", self.encode(h=Window(23.4, later)))
        self.assertIn(" h=24 ", self.encode(h=Window(23.6, later)))
        self.assertIn(" h=100 ", self.encode(h=Window(180.0, later)))
        self.assertIn(" h=0 ", self.encode(h=Window(-5.0, later)))

    def test_minutes_until_reset_round_up(self):
        # 还剩 30 秒时显示 1 分钟，避免还没重置就显示 0 分钟
        self.assertIn(" hr=1 ", self.encode(h=Window(10.0, self.NOW + 30)))
        self.assertIn(" hr=60 ", self.encode(h=Window(10.0, self.NOW + 3600)))
        self.assertTrue(self.encode(w=Window(10.0, self.NOW + 3601)).endswith(" w=10 wr=61\n"))

    def test_minutes_are_clamped_to_two_weeks(self):
        self.assertIn(" hr=20160 ", self.encode(h=Window(1.0, self.NOW + 30 * 86400)))

    def test_passed_reset_counts_as_zero_with_unknown_time(self):
        # 方案 §3.4：重置时间已过，用量按 0%，重置时间未知
        self.assertIn(" h=0 hr=- ", self.encode(h=Window(85.0, self.NOW - 1)))
        self.assertIn(" h=0 hr=- ", self.encode(h=Window(85.0, self.NOW)))

    def test_unknown_reset_time_keeps_percent(self):
        self.assertIn(" h=12 hr=- ", self.encode(h=Window(12.0, None)))

    def test_counts_are_clamped(self):
        line = self.encode(counts=ToolCounts(wn=150, bg=-1, done=5000))
        self.assertIn(" wn=99 bg=0 done=999 ", line)

    def test_line_never_exceeds_limit(self):
        big = Window(100.0, self.NOW + 99 * 86400)
        line = self.encode("codex", ToolCounts(wn=99, bg=99, done=999), big, big, plan="W" * 200)
        self.assertLessEqual(len(line.encode("ascii")), MAX_LINE)


class DeviceLineTest(unittest.TestCase):
    def test_ping(self):
        self.assertEqual(encode_ping(), "AR1 PING\n")

    def test_parse_hello(self):
        self.assertEqual(
            parse_device_line("AR1 HELLO fw=0.1.0 proto=1\r"),
            {"type": "HELLO", "fw": "0.1.0", "proto": "1"},
        )

    def test_log_lines_are_ignored(self):
        self.assertIsNone(parse_device_line("I (1234) agent_run: link up"))
        self.assertIsNone(parse_device_line(""))
        self.assertIsNone(parse_device_line("AR1"))


if __name__ == "__main__":
    unittest.main()
