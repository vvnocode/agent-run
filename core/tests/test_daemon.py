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
        self.lines = []
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
        self.lines.append(line)
        return True

    def read_lines(self):
        return []

    def close(self):
        self.closed = True


class FakeBle:
    """模拟经桥接程序的蓝牙链路：ready 控制桥接程序是否报告已连接。"""

    instances = []
    ready = True
    kind = "ble"

    def __init__(self):
        self.lines = []
        self.closed = False
        FakeBle.instances.append(self)

    def open(self):
        pass

    def handshake(self, timeout=1.0):
        return {"type": "HELLO", "fw": "0.2.0", "proto": "1", "name": "AgentRun-XXXX"} if FakeBle.ready else None

    def write_line(self, line):
        if not FakeBle.ready:
            return False
        self.lines.append(line)
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
        self.daemon = self.make_daemon()

    def make_daemon(self):
        d = Daemon(
            home=self.home,
            clock=self.clock,
            link_factory=FakeLink,
            port_finder=lambda: self.port[0],
            dsh_root=self.dsh_root,
            day_of=lambda t: "2026-11-08",
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
