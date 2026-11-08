"""串口协议编码与设备回包解析测试（设计方案 §6）。"""
import unittest

from agent_run.protocol import MAX_LINE, encode_ping, encode_state, parse_device_line
from agent_run.state import Snapshot


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
