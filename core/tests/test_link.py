"""串口链路测试：用伪终端（pty）模拟设备（设计方案 §6）。"""
import os
import threading
import time
import unittest

from agent_run.serial_link import SerialLink


class FakeDevice:
    """持有 pty 主端，模拟设备的读写。"""

    def __init__(self):
        self.master, self.slave = os.openpty()
        self.path = os.ttyname(self.slave)

    def read_line(self, timeout=2.0):
        """从主端读到一个 \\n 结尾的行。"""
        deadline = time.monotonic() + timeout
        buf = b""
        os.set_blocking(self.master, False)
        while time.monotonic() < deadline:
            try:
                chunk = os.read(self.master, 256)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            buf += chunk
            if b"\n" in buf:
                return buf.split(b"\n", 1)[0].decode() + "\n"
        return None

    def write(self, data):
        os.write(self.master, data)

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


class SerialLinkTest(unittest.TestCase):
    def setUp(self):
        self.dev = FakeDevice()
        self.link = SerialLink(self.dev.path)
        self.link.open()

    def tearDown(self):
        self.link.close()
        self.dev.close()

    def test_handshake_skips_log_lines(self):
        def device():
            line = self.dev.read_line()
            if line == "AR1 PING\n":
                self.dev.write(b"I (10) boot: hello\r\nAR1 HELLO fw=0.1.0 proto=1\r\n")

        t = threading.Thread(target=device)
        t.start()
        hello = self.link.handshake(timeout=1.0)
        t.join()
        self.assertEqual(hello, {"type": "HELLO", "fw": "0.1.0", "proto": "1"})

    def test_handshake_times_out_when_device_silent(self):
        start = time.monotonic()
        self.assertIsNone(self.link.handshake(timeout=0.3))
        self.assertLess(time.monotonic() - start, 1.0)

    def test_write_line_reaches_device(self):
        self.assertTrue(self.link.write_line("AR1 S seq=1 cc=0 cx=0 ds=0 done=0 wn=0 w=-\n"))
        self.assertEqual(self.dev.read_line(), "AR1 S seq=1 cc=0 cx=0 ds=0 done=0 wn=0 w=-\n")

    def test_read_lines_buffers_partial_lines(self):
        self.dev.write(b"AR1 HEL")
        time.sleep(0.05)
        self.assertEqual(self.link.read_lines(), [])
        self.dev.write(b"LO fw=1 proto=1\nI (1) x\r\n")
        time.sleep(0.05)
        self.assertEqual(self.link.read_lines(), ["AR1 HELLO fw=1 proto=1", "I (1) x"])


if __name__ == "__main__":
    unittest.main()
