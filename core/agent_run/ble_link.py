"""蓝牙链路：经 AgentRunBridge.app 转发（无线方案 §5）。

Python 标准库没有蓝牙，蓝牙由 Swift 写的桥接程序负责：
- 守护进程把状态行写到桥接程序绑定的数据报 socket（$AGENT_RUN_HOME/run/ble.sock）；
- 桥接程序每 2 秒写一次 $AGENT_RUN_HOME/run/ble-status.json：
  {"connected": bool, "name": "AgentRun-XXXX", "fw": "0.2.0", "updated": 时间戳}
  超过 STALE_SECONDS 没更新，视为桥接程序不在或已卡住。
接口与 SerialLink 一致，守护进程可以不区分地使用两种链路。
"""
import json
import socket
import time

from .serial_link import LinkClosed

# 桥接程序状态文件超过这么久没更新，视为不可用（它每 2 秒写一次）
STALE_SECONDS = 6.0


class BleLink:
    """通过桥接程序与设备通信的链路。"""

    kind = "ble"

    def __init__(self, sock_path, status_path, clock=time.time):
        self.sock_path = sock_path
        self.status_path = status_path
        self._clock = clock
        self._sock = None

    def open(self):
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._sock.setblocking(False)

    def close(self):
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def _bridge_status(self):
        """读桥接程序的状态；不可用时返回 None。"""
        try:
            with open(self.status_path) as f:
                status = json.load(f)
        except (OSError, ValueError):
            return None
        if not isinstance(status, dict) or not status.get("connected"):
            return None
        updated = status.get("updated")
        if not isinstance(updated, (int, float)) or self._clock() - updated > STALE_SECONDS:
            return None
        return status

    def handshake(self, timeout=1.0):
        """桥接程序已连上设备时返回 HELLO 信息（固件版本、设备名），否则返回 None。"""
        status = self._bridge_status()
        if status is None:
            return None
        return {"type": "HELLO", "fw": str(status.get("fw", "?")), "proto": "1", "name": str(status.get("name", ""))}

    def write_line(self, line, timeout=1.0):
        """把一行交给桥接程序；桥接程序不可用或 socket 写失败时返回 False。"""
        if self._sock is None or self._bridge_status() is None:
            return False
        try:
            self._sock.sendto(line.encode("ascii", "replace"), self.sock_path)
        except OSError:
            return False
        return True

    def read_lines(self):
        """设备回包由桥接程序处理，这里只用来发现断开：桥接程序不可用时抛出 LinkClosed。"""
        if self._bridge_status() is None:
            raise LinkClosed()
        return []
