"""守护进程调度测试：事件入口、心跳、重连、状态文件与演示模式。

用假时钟和假链路逐步驱动 Daemon.step()，不依赖真实串口。
"""
import json
import os
import socket
import tempfile
import time
import unittest

from agent_run.daemon import DEMO_STEP_SECONDS, Daemon
from agent_run.usage import EMPTY, NO_LIMIT, ToolUsage, Window


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


class FakeLink:
    """记录写出的行；可模拟握手失败与写失败。"""

    instances = []
    kind = "usb"

    def __init__(self, path):
        self.path = path
        self.lines = []          # 状态帧等非 A 帧
        self.agent_lines = []    # 工具专用页面的 A 帧
        self.incoming = []       # 设备回包，下一次 read_lines 时交出
        self.closed = False
        self.fail_write = False
        self.hello = True
        FakeLink.instances.append(self)

    def open(self):
        pass

    def handshake(self, timeout=1.0):
        return {"type": "HELLO", "fw": "test", "proto": "1"} if self.hello else None

    def write_line(self, line):
        if self.fail_write:
            return False
        (self.agent_lines if line.startswith("AR1 A ") else self.lines).append(line)
        return True

    def read_lines(self):
        lines, self.incoming = self.incoming, []
        return lines

    def close(self):
        self.closed = True


class FakeUsage:
    """用量采集替身：按工具返回预设用量，记录 Stop 通知与生命周期调用。"""

    def __init__(self):
        self.data = {}
        self.stops = []
        self.started = False
        self.closed = False

    def start(self):
        self.started = True

    def close(self):
        self.closed = True

    def step(self, now):
        pass

    def on_stop(self, tool, now):
        self.stops.append((tool, now))

    def get(self, tool):
        return self.data.get(tool, EMPTY)


class FakeBle:
    """模拟经桥接程序的蓝牙链路：ready 控制桥接程序是否报告已连接。"""

    instances = []
    ready = True
    kind = "ble"

    def __init__(self):
        self.lines = []
        self.agent_lines = []
        self.closed = False
        FakeBle.instances.append(self)

    def open(self):
        pass

    def handshake(self, timeout=1.0):
        return {"type": "HELLO", "fw": "0.2.0", "proto": "1", "name": "AgentRun-XXXX"} if FakeBle.ready else None

    def write_line(self, line):
        if not FakeBle.ready:
            return False
        (self.agent_lines if line.startswith("AR1 A ") else self.lines).append(line)
        return True

    def read_lines(self):
        if not FakeBle.ready:
            from agent_run.serial_link import LinkClosed
            raise LinkClosed()
        return []

    def close(self):
        self.closed = True


def frame_fields(line):
    """把 'AR1 S k=v ...' 解析成字典，便于断言。"""
    parts = line.strip().split(" ")[2:]
    return dict(p.split("=", 1) for p in parts)


class DaemonTest(unittest.TestCase):
    def setUp(self):
        FakeLink.instances = []
        self.home = tempfile.mkdtemp(prefix="ar-")
        self.clock = FakeClock(1000.0)
        self.port = ["/dev/fake0"]
        self.dsh_root = tempfile.mkdtemp()
        self.usage = FakeUsage()
        self.daemon = self.make_daemon()

    def make_daemon(self):
        d = Daemon(
            home=self.home,
            clock=self.clock,
            link_factory=FakeLink,
            port_finder=lambda: self.port[0],
            dsh_root=self.dsh_root,
            day_of=lambda t: "2026-11-08",
            usage=self.usage,
        )
        d.start()
        return d

    def tearDown(self):
        self.daemon.close()

    def send(self, msg):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            client.sendto(json.dumps(msg).encode(), os.path.join(self.home, "run", "ar.sock"))
        finally:
            client.close()
        time.sleep(0.01)

    def link(self):
        return FakeLink.instances[-1]

    def test_event_triggers_immediate_frame(self):
        self.daemon.step()
        self.assertEqual(len(self.link().lines), 1)
        self.send({"tool": "claude", "event": "UserPromptSubmit", "sid": "s1", "ts": 1000.5})
        self.daemon.step()
        fields = frame_fields(self.link().lines[-1])
        self.assertEqual(fields["cc"], "1")

    def test_heartbeat_every_two_seconds(self):
        self.daemon.step()
        sent = len(self.link().lines)
        self.clock.t += 1.9
        self.daemon.step()
        self.assertEqual(len(self.link().lines), sent)
        self.clock.t += 0.2
        self.daemon.step()
        self.assertEqual(len(self.link().lines), sent + 1)
        seqs = [int(frame_fields(l)["seq"]) for l in self.link().lines]
        self.assertEqual(seqs, sorted(seqs))

    def test_no_port_means_no_link_and_retry_every_three_seconds(self):
        self.port[0] = None
        self.daemon.step()
        self.assertEqual(FakeLink.instances, [])
        self.port[0] = "/dev/fake0"
        self.clock.t += 2.0
        self.daemon.step()
        self.assertEqual(FakeLink.instances, [])
        self.clock.t += 1.1
        self.daemon.step()
        self.assertEqual(len(FakeLink.instances), 1)

    def test_write_failure_drops_link_and_reconnects(self):
        self.daemon.step()
        first = self.link()
        first.fail_write = True
        self.clock.t += 2.1
        self.daemon.step()
        self.assertTrue(first.closed)
        self.clock.t += 3.1
        self.daemon.step()
        self.assertIsNot(self.link(), first)
        self.assertEqual(len(self.link().lines), 1)

    def test_failed_handshake_is_not_used(self):
        original = FakeLink.handshake
        FakeLink.handshake = lambda self, timeout=1.0: None
        try:
            self.daemon.step()
        finally:
            FakeLink.handshake = original
        self.assertTrue(self.link().closed)
        self.assertEqual(self.link().lines, [])

    def test_status_file_reflects_snapshot(self):
        self.send({"tool": "codex", "event": "PermissionRequest", "sid": "x", "ts": 1000.5})
        self.daemon.step()
        with open(os.path.join(self.home, "run", "status.json")) as f:
            status = json.load(f)
        self.assertEqual(status["wn"], 1)
        self.assertEqual(status["waiting"], [["codex", "A"]])
        self.assertTrue(status["link"])

    def test_status_file_lists_recent_events(self):
        self.send({"tool": "codex", "event": "PreToolUse", "sid": "x", "ts": 1000.5,
                   "tool_name": "request_user_input_async"})
        self.daemon.step()
        with open(os.path.join(self.home, "run", "status.json")) as f:
            status = json.load(f)
        self.assertEqual(status["recent"][-1]["tool_name"], "request_user_input_async")
        self.assertNotIn("sid", status["recent"][-1])

    def test_done_count_survives_restart(self):
        self.send({"tool": "claude", "event": "UserPromptSubmit", "sid": "s1", "ts": 1000.5})
        self.send({"tool": "claude", "event": "Stop", "sid": "s1", "ts": 1000.6})
        self.daemon.step()
        self.daemon.close()
        self.daemon = self.make_daemon()
        self.daemon.step()
        self.assertEqual(frame_fields(self.link().lines[-1])["done"], "1")

    def test_dsh_scan_feeds_ds_count(self):
        path = os.path.join(self.dsh_root, "--p--", "session-1")
        os.makedirs(path)
        record = os.path.join(path, "session.v4.jsonl.zstd")
        with open(record, "wb") as f:
            f.write(b"x")
        os.utime(record, (self.clock.t, self.clock.t))
        self.daemon.step()
        self.assertEqual(frame_fields(self.link().lines[-1])["ds"], "1")

    def test_demo_cycles_screens_then_pauses_for_disconnect(self):
        self.daemon.step()
        self.send({"ctl": "demo"})
        self.daemon.step()
        totals = []
        for _ in range(6):
            fields = frame_fields(self.link().lines[-1])
            totals.append((int(fields["cc"]) + int(fields["cx"]) + int(fields["ds"]), int(fields["wn"])))
            self.clock.t += DEMO_STEP_SECONDS
            self.daemon.step()
        # 依次为：睡觉、慢走、小跑、狂奔、冲刺、等你处理
        self.assertEqual([t for t, _ in totals[:5]], [0, 1, 3, 5, 8])
        self.assertGreater(totals[5][1], 0)
        # 之后进入“未连接”演示段：不再发送任何帧
        sent = len(self.link().lines)
        self.clock.t += 2.1
        self.daemon.step()
        self.assertEqual(len(self.link().lines), sent)

    # ---- 工具专用页面（专用页面方案 §5） -------------------------------------

    def agent_frames(self):
        """按工具代号取最近一次发送的 A 帧字段。"""
        frames = {}
        for line in self.link().agent_lines:
            fields = frame_fields(line)
            frames[fields["t"]] = fields
        return frames

    def test_agent_frames_sent_for_each_tool_on_connect(self):
        self.daemon.step()
        self.assertEqual(self.link().agent_lines, [
            "AR1 A t=CC wn=0 bg=0 done=0 h=- hr=- w=- wr=-\n",
            "AR1 A t=CX wn=0 bg=0 done=0 h=- hr=- w=- wr=-\n",
        ])
        self.assertTrue(self.usage.started)

    def test_agent_frames_carry_tool_counts_and_usage(self):
        self.usage.data["codex"] = ToolUsage(NO_LIMIT, Window(5.0, self.clock.t + 6110 * 60), 990.0, None)
        self.send({"tool": "codex", "event": "UserPromptSubmit", "sid": "x1", "ts": 1000.1})
        self.send({"tool": "codex", "event": "Stop", "sid": "x1", "ts": 1000.2})
        self.send({"tool": "claude", "event": "UserPromptSubmit", "sid": "c1", "ts": 1000.3})
        self.send({"tool": "claude", "event": "PermissionRequest", "sid": "c1", "ts": 1000.4})
        self.daemon.step()
        self.assertEqual(self.link().agent_lines[-1], "AR1 A t=CX wn=0 bg=0 done=1 h=x hr=- w=5 wr=6110\n")
        self.assertEqual(self.agent_frames()["CC"]["wn"], "1")

    def test_agent_frames_carry_plan(self):
        self.usage.data["codex"] = ToolUsage(NO_LIMIT, Window(5.0, self.clock.t + 6110 * 60), 990.0, None, "Pro Lite")
        self.daemon.step()
        self.assertEqual(self.link().agent_lines[-1], "AR1 A t=CX wn=0 bg=0 done=0 h=x hr=- w=5 wr=6110 p=Pro_Lite\n")
        self.assertNotIn("p", self.agent_frames()["CC"])             # 没有套餐数据时不带 p

    def test_agent_frames_resent_only_when_content_changes(self):
        self.usage.data["claude"] = ToolUsage(Window(20.0, self.clock.t + 3600), None, 990.0, None)
        self.daemon.step()
        sent = len(self.link().agent_lines)
        self.clock.t += 30
        self.daemon.step()
        self.assertEqual(len(self.link().agent_lines), sent)        # 心跳不重发 A 帧
        self.clock.t += 31
        self.daemon.step()                                           # 距重置分钟数变了
        self.assertEqual(len(self.link().agent_lines), sent + 1)
        self.assertEqual(self.agent_frames()["CC"]["hr"], "59")
        self.send({"tool": "codex", "event": "PermissionRequest", "sid": "x", "ts": 1061.5})
        self.daemon.step()
        self.assertEqual(len(self.link().agent_lines), sent + 2)
        self.assertTrue(self.link().agent_lines[-1].startswith("AR1 A t=CX wn=1 "))

    def test_device_hello_resends_agent_frames(self):
        self.daemon.step()
        sent = len(self.link().agent_lines)
        self.link().incoming = ["AR1 HELLO fw=0.4.0 proto=1"]
        self.daemon.step()
        self.assertEqual(len(self.link().agent_lines), sent + 2)

    def test_stop_events_trigger_usage_refresh(self):
        self.send({"tool": "claude", "event": "UserPromptSubmit", "sid": "c1", "ts": 1000.1})
        self.send({"tool": "claude", "event": "Stop", "sid": "c1", "ts": 1000.2})
        self.daemon.step()
        self.assertEqual(self.usage.stops, [("claude", 1000.0)])

    def test_status_file_lists_agents(self):
        resets = self.clock.t + 600
        self.usage.data["claude"] = ToolUsage(Window(23.5, resets), None, 990.0, "用量接口返回 HTTP 429", "Max 5x")
        self.send({"tool": "claude", "event": "UserPromptSubmit", "sid": "c1", "ts": 1000.1})
        self.send({"tool": "claude", "event": "Stop", "sid": "c1", "ts": 1000.2, "bg": 1})
        self.daemon.step()
        with open(os.path.join(self.home, "run", "status.json")) as f:
            agents = json.load(f)["agents"]
        self.assertEqual(agents["claude"], {
            "wn": 0, "bg": 1, "done": 0,
            "five_hour": {"used": 23.5, "resets_at": resets}, "seven_day": None,
            "usage_updated": 990.0, "usage_error": "用量接口返回 HTTP 429", "plan": "Max 5x",
        })
        self.assertEqual(agents["codex"]["five_hour"], None)
        self.assertIsNone(agents["codex"]["plan"])

    def test_tool_done_survives_restart(self):
        self.send({"tool": "codex", "event": "UserPromptSubmit", "sid": "x1", "ts": 1000.5})
        self.send({"tool": "codex", "event": "Stop", "sid": "x1", "ts": 1000.6})
        self.daemon.step()
        with open(os.path.join(self.home, "state.json")) as f:
            self.assertEqual(json.load(f)["done_by_tool"], {"claude": 0, "codex": 1, "dsh": 0})
        self.daemon.close()
        self.assertTrue(self.usage.closed)
        self.daemon = self.make_daemon()
        self.daemon.step()
        self.assertEqual(self.agent_frames()["CX"]["done"], "1")

    def test_demo_sends_sample_agent_frames(self):
        self.daemon.step()
        self.send({"ctl": "demo"})
        self.daemon.step()
        frames = self.agent_frames()
        self.assertGreaterEqual(int(frames["CC"]["h"]), 80)     # 一个 ≥80% 的窗口
        self.assertEqual(frames["CX"]["h"], "x")                 # 一个“无此限额”的窗口
        self.assertNotEqual(frames["CX"]["w"], "-")
        self.assertTrue(frames["CC"]["p"] and frames["CX"]["p"])  # 示例套餐


class DaemonBleTest(unittest.TestCase):
    """USB 优先、没有 USB 时走蓝牙、USB 插回后切回（无线方案 §5.2）。"""

    def setUp(self):
        FakeLink.instances = []
        FakeBle.instances = []
        FakeBle.ready = True
        self.home = tempfile.mkdtemp(prefix="ar-")
        self.clock = FakeClock(1000.0)
        self.port = [None]
        self.daemon = Daemon(
            home=self.home,
            clock=self.clock,
            link_factory=FakeLink,
            port_finder=lambda: self.port[0],
            ble_factory=FakeBle,
            day_of=lambda t: "2026-11-08",
        )
        self.daemon.start()

    def tearDown(self):
        self.daemon.close()

    def status(self):
        with open(os.path.join(self.home, "run", "status.json")) as f:
            return json.load(f)

    def test_uses_ble_when_no_usb_port(self):
        self.daemon.step()
        self.assertEqual(len(FakeBle.instances), 1)
        self.assertEqual(len(FakeBle.instances[0].lines), 1)
        self.assertEqual(self.status()["link_type"], "ble")

    def test_ble_heartbeat_is_five_seconds(self):
        self.daemon.step()
        ble = FakeBle.instances[0]
        self.clock.t += 2.1
        self.daemon.step()
        self.assertEqual(len(ble.lines), 1)
        self.clock.t += 3.0
        self.daemon.step()
        self.assertEqual(len(ble.lines), 2)

    def test_ble_drops_when_bridge_disconnects_and_recovers(self):
        self.daemon.step()
        FakeBle.ready = False
        self.clock.t += 0.5
        self.daemon.step()
        self.assertTrue(FakeBle.instances[0].closed)
        self.assertFalse(self.status()["link"])
        FakeBle.ready = True
        self.clock.t += 3.1
        self.daemon.step()
        self.assertEqual(len(FakeBle.instances), 2)
        self.assertTrue(self.status()["link"])

    def test_switches_back_to_usb_when_plugged_in(self):
        self.daemon.step()
        ble = FakeBle.instances[0]
        self.port[0] = "/dev/fake0"
        self.clock.t += 3.1
        self.daemon.step()
        self.assertTrue(ble.closed)
        self.assertEqual(len(FakeLink.instances), 1)
        self.assertEqual(len(FakeLink.instances[0].lines), 1)
        self.assertEqual(self.status()["link_type"], "usb")

    def test_usb_preferred_over_ble(self):
        self.port[0] = "/dev/fake0"
        self.daemon.step()
        self.assertEqual(FakeBle.instances, [])
        self.assertEqual(len(FakeLink.instances[0].lines), 1)


if __name__ == "__main__":
    unittest.main()
