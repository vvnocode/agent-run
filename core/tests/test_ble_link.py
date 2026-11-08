"""蓝牙链路（经桥接程序）测试：无线方案 §5.2。

守护进程把状态行写到桥接程序的数据报 socket；桥接程序每 2 秒写一次 ble-status.json，
超过 6 秒没更新或报告未连接时视为不可用。
"""
import json
import os
import socket
import tempfile
import unittest

from agent_run.ble_link import BleLink
from agent_run.serial_link import LinkClosed


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


class BleLinkTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="arb-")
        self.sock_path = os.path.join(self.dir, "ble.sock")
        self.status_path = os.path.join(self.dir, "ble-status.json")
        self.bridge = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.bridge.bind(self.sock_path)
        self.bridge.settimeout(1)
        self.clock = FakeClock()
        self.link = BleLink(self.sock_path, self.status_path, clock=self.clock)
        self.link.open()

    def tearDown(self):
        self.link.close()
        self.bridge.close()

    def write_status(self, connected, updated, name="AgentRun-XXXX", fw="0.2.0"):
        with open(self.status_path, "w") as f:
            json.dump({"connected": connected, "name": name, "fw": fw, "updated": updated}, f)

    def test_not_ready_without_status_file(self):
        self.assertIsNone(self.link.handshake())
        self.assertFalse(self.link.write_line("AR1 PING\n"))

    def test_ready_bridge_handshake_and_write(self):
        self.write_status(True, self.clock.t - 1)
        hello = self.link.handshake()
        self.assertEqual((hello["type"], hello["fw"], hello["name"]), ("HELLO", "0.2.0", "AgentRun-XXXX"))
        self.assertTrue(self.link.write_line("AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=0 w=-\n"))
        self.assertEqual(self.bridge.recv(512), b"AR1 S seq=1 cc=1 cx=0 ds=0 done=0 wn=0 w=-\n")
        self.assertEqual(self.link.read_lines(), [])

    def test_stale_or_disconnected_bridge_closes_link(self):
        self.write_status(True, self.clock.t - 7)
        self.assertIsNone(self.link.handshake())
        with self.assertRaises(LinkClosed):
            self.link.read_lines()
        self.write_status(False, self.clock.t)
        self.assertFalse(self.link.write_line("AR1 PING\n"))

    def test_kind(self):
        self.assertEqual(self.link.kind, "ble")


if __name__ == "__main__":
    unittest.main()
