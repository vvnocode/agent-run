"""agent-run 守护进程（设计方案 §4、§5、§6）。

职责：
1. 在 $AGENT_RUN_HOME/run/ar.sock 接收 hook 事件与控制命令（Unix 数据报）；
2. 定时执行超时回收、日切与 dsh 扫描；
3. 维护到设备的串口链路：握手、状态变化立刻发送、每 2 秒心跳、断开后每 3 秒重连；
4. 按工具发送专用页面的 A 帧（计数与订阅用量），内容变化时才发送（专用页面方案 §5.2）；
5. 写 run/status.json 供 `agent-run status` 查看，写 state.json 保存今日完成数。

Daemon.step() 执行一轮调度，所有时间都取自注入的时钟，便于用假时钟测试。
"""
import json
import logging
import os
import select
import socket
import time

from .dsh import count_working
from .protocol import encode_agent, encode_state, parse_device_line
from .serial_link import LinkClosed, SerialLink
from .state import Aggregator, Snapshot, ToolCounts, local_day
from .usage import EMPTY, NO_LIMIT, Window

log = logging.getLogger("agent-run")

# 调度周期（秒）。蓝牙下心跳放宽到 5 秒，设备端蓝牙超时相应为 15 秒
HEARTBEAT_SECONDS = 2.0
HEARTBEAT_BLE_SECONDS = 5.0
RETRY_SECONDS = 3.0
TICK_SECONDS = 1.0
DSH_SCAN_SECONDS = 5.0
STATUS_REFRESH_SECONDS = 5.0

# dsh 会话记录在多少秒内有写入算工作中
DSH_WINDOW_SECONDS = 20

# 演示模式：六个画面各停留 5 秒，再停发 12 秒让设备显示“未连接”
DEMO_STEP_SECONDS = 5.0
DEMO_PAUSE_SECONDS = 12.0
DEMO_SCREENS = [
    Snapshot(cc=0, cx=0, ds=0, done=12, wn=0, waiting=[]),                       # 睡觉
    Snapshot(cc=1, cx=0, ds=0, done=13, wn=0, waiting=[]),                       # 慢走
    Snapshot(cc=2, cx=1, ds=0, done=16, wn=0, waiting=[]),                       # 小跑
    Snapshot(cc=3, cx=1, ds=1, done=21, wn=0, waiting=[]),                       # 狂奔
    Snapshot(cc=4, cx=2, ds=2, done=34, wn=0, waiting=[]),                       # 冲刺
    Snapshot(cc=2, cx=1, ds=0, done=21, wn=2, waiting=[("claude", "A"), ("codex", "Q")]),  # 等你处理
]

# 有专用页面的工具，按设备上的页面顺序（专用页面方案 §4.1）
AGENT_TOOLS = ("claude", "codex")

# 演示模式下专用页面的示例数据：(计数, 5 小时窗口, 每周窗口)，窗口为 (已用百分比, 距重置秒数)。
# 含一个 ≥80% 的窗口（进度条变橙色）和一个“无此限额”的窗口（专用页面方案 §5.5）
DEMO_AGENTS = {
    "claude": (ToolCounts(wn=1, bg=1, done=8), (86.0, 2 * 3600 + 15 * 60), (41.0, 3 * 86400 + 5 * 3600)),
    "codex": (ToolCounts(wn=1, bg=0, done=13), NO_LIMIT, (23.0, 6 * 86400 + 2 * 3600)),
}

# 每轮最多处理的数据报数，防止事件风暴饿死链路维护
_MAX_DATAGRAMS_PER_STEP = 256


def default_home():
    """运行数据目录：$AGENT_RUN_HOME，默认 ~/.agent-run。"""
    return os.environ.get("AGENT_RUN_HOME") or os.path.expanduser("~/.agent-run")


def _write_json_atomic(path, data):
    """先写临时文件再改名，避免读取方看到半截内容。"""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, ensure_ascii=False)
    os.replace(tmp, path)


def _window_json(window):
    """把用量窗口写成 status.json 里的值：{"used", "resets_at"}、"none" 或 null。"""
    if window is None or window == NO_LIMIT:
        return window
    return {"used": window.used, "resets_at": window.resets_at}


class _NoUsage:
    """未配置用量采集时的占位（测试与旧调用方）：所有窗口都是暂无数据。"""

    def start(self):
        pass

    def close(self):
        pass

    def step(self, now):
        pass

    def on_stop(self, tool, now):
        pass

    def get(self, tool):
        return EMPTY


def _read_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


class Daemon:
    def __init__(self, home=None, clock=time.time, link_factory=SerialLink, port_finder=None,
                 dsh_root=None, day_of=local_day, rules=None, ble_factory=None, usage=None):
        """
        home:         运行数据目录
        clock:        时钟函数
        link_factory: 以端口路径构造 USB 链路对象的工厂（测试时注入假链路）
        port_finder:  返回设备端口路径或 None 的函数
        ble_factory:  构造蓝牙链路对象的工厂；None 表示不使用蓝牙
        dsh_root:     dsh 会话根目录；None 表示不扫描
        day_of:       时间戳到本地日期的换算
        rules:        事件映射规则覆盖
        usage:        订阅用量采集（usage.UsageCollector）；None 表示不采集，专用页面显示暂无数据
        """
        self.home = home or default_home()
        self.run_dir = os.path.join(self.home, "run")
        self.sock_path = os.path.join(self.run_dir, "ar.sock")
        self.status_path = os.path.join(self.run_dir, "status.json")
        self.state_path = os.path.join(self.home, "state.json")
        self._clock = clock
        self._link_factory = link_factory
        self._ble_factory = ble_factory
        self._next_usb_check = 0.0
        self._port_finder = port_finder or (lambda: None)
        self._dsh_root = dsh_root
        self._day_of = day_of
        self._rules = rules
        self._usage = usage if usage is not None else _NoUsage()

        self._sock = None
        self._agg = None
        self._link = None
        self._port = None
        self._seq = 0
        self._last_sent = None
        self._last_agent = {}       # 每个工具上一次发出的 A 帧，内容变化才重发
        self._last_send_at = 0.0
        self._next_retry = 0.0
        self._next_tick = 0.0
        self._next_dsh = 0.0
        self._last_status = None
        self._last_status_at = 0.0
        self._last_done = None
        self._demo_start = None

    # ------------------------------------------------------------------
    # 生命周期
    # ------------------------------------------------------------------

    def start(self):
        """创建运行目录、绑定事件 socket，并恢复今日完成数。"""
        os.makedirs(self.run_dir, exist_ok=True)
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)  # 上次异常退出留下的旧 socket 文件
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._sock.bind(self.sock_path)
        self._sock.setblocking(False)

        saved = _read_json(self.state_path, {})
        initial = None
        by_tool = None
        if isinstance(saved, dict) and "day" in saved and "done" in saved:
            initial = (saved["day"], saved["done"])
            # 旧版 state.json 没有 done_by_tool，按 0 处理
            if isinstance(saved.get("done_by_tool"), dict):
                by_tool = saved["done_by_tool"]
        self._agg = Aggregator(clock=self._clock, day_of=self._day_of, initial_done=initial, rules=self._rules,
                               initial_done_by_tool=by_tool)
        self._last_done = self._done_state()
        self._usage.start()

    def close(self):
        """关闭链路与 socket，删除 socket 文件。"""
        self._drop_link("守护进程退出")
        self._usage.close()
        if self._sock is not None:
            self._sock.close()
            self._sock = None
            try:
                os.unlink(self.sock_path)
            except OSError:
                pass

    def run_forever(self):
        """前台循环：有事件时立刻处理，否则每 0.25 秒推进一次调度。"""
        log.info("agent-run 守护进程启动，数据目录 %s", self.home)
        try:
            while True:
                self.step()
                select.select([self._sock], [], [], 0.25)
        finally:
            self.close()

    # ------------------------------------------------------------------
    # 调度
    # ------------------------------------------------------------------

    def step(self):
        """执行一轮调度。"""
        now = self._clock()
        self._drain_socket(now)

        if now >= self._next_tick:
            self._agg.tick()
            self._next_tick = now + TICK_SECONDS
        if self._dsh_root and now >= self._next_dsh:
            self._agg.set_dsh_working(count_working(self._dsh_root, now, window=DSH_WINDOW_SECONDS))
            self._next_dsh = now + DSH_SCAN_SECONDS

        self._usage.step(now)

        self._maybe_connect(now)
        self._read_device()

        snap, paused = self._effective_snapshot(now)
        if self._link is not None and not paused:
            heartbeat = HEARTBEAT_BLE_SECONDS if self._link_kind() == "ble" else HEARTBEAT_SECONDS
            due = now - self._last_send_at >= heartbeat
            if snap != self._last_sent or due:
                self._send(snap, now)
        if self._link is not None and not paused:
            self._send_agents(now)

        self._write_status(now)
        self._persist_done()

    def _drain_socket(self, now):
        """读出所有排队的数据报：控制命令或 hook 事件。"""
        for _ in range(_MAX_DATAGRAMS_PER_STEP):
            try:
                data = self._sock.recv(4096)
            except (BlockingIOError, InterruptedError):
                return
            try:
                msg = json.loads(data.decode("utf-8"))
            except ValueError:
                continue
            if not isinstance(msg, dict):
                continue
            if "ctl" in msg:
                self._handle_ctl(msg, now)
            else:
                self._agg.handle(msg)
                # 一轮结束后用量可能刚变化，让用量采集提前刷新
                if msg.get("event") == "Stop":
                    self._usage.on_stop(msg.get("tool"), now)

    def _handle_ctl(self, msg, now):
        """控制命令：目前只有 demo。"""
        if msg.get("ctl") == "demo":
            self._demo_start = now
            log.info("进入演示模式")

    def _effective_snapshot(self, now):
        """返回 (要发送的快照, 是否停发)。演示模式下用预设画面替代真实状态。"""
        if self._demo_start is not None:
            elapsed = now - self._demo_start
            index = int(elapsed // DEMO_STEP_SECONDS)
            if index < len(DEMO_SCREENS):
                return DEMO_SCREENS[index], False
            if elapsed < len(DEMO_SCREENS) * DEMO_STEP_SECONDS + DEMO_PAUSE_SECONDS:
                return None, True  # 停发心跳，让设备超时显示“未连接”
            self._demo_start = None
            self._last_sent = None
            self._last_agent = {}
            log.info("演示模式结束")
        return self._agg.snapshot(), False

    # ------------------------------------------------------------------
    # 链路
    # ------------------------------------------------------------------

    def _link_kind(self):
        return getattr(self._link, "kind", "usb") if self._link is not None else None

    def _try_usb(self):
        """查找 USB 端口并握手；成功返回 (链路, 端口, HELLO)，否则返回 None。"""
        path = self._port_finder()
        if not path:
            return None
        link = self._link_factory(path)
        hello = None
        try:
            link.open()
            hello = link.handshake(timeout=1.0)
        except OSError as exc:
            log.info("打开 %s 失败：%s", path, exc)
        if not hello:
            link.close()
            return None
        return link, path, hello

    def _try_ble(self):
        """桥接程序已连上设备时返回 (链路, 设备名, HELLO)，否则返回 None。"""
        if self._ble_factory is None:
            return None
        link = self._ble_factory()
        link.open()
        hello = link.handshake(timeout=1.0)
        if not hello:
            link.close()
            return None
        return link, hello.get("name") or "蓝牙", hello

    def _use(self, found, now):
        link, where, hello = found
        self._link = link
        self._port = where
        self._last_sent = None  # 新连接立刻发送一帧完整状态
        self._last_agent = {}   # 以及每个工具的 A 帧
        self._next_usb_check = now + RETRY_SECONDS
        log.info("已连接设备 %s（%s，固件 %s）", where, self._link_kind(), hello.get("fw", "?"))

    def _maybe_connect(self, now):
        """USB 优先：未连接时先找 USB，再看蓝牙；走蓝牙时定期检查 USB 是否插回。"""
        if self._link is not None:
            if self._link_kind() == "ble" and now >= self._next_usb_check:
                self._next_usb_check = now + RETRY_SECONDS
                found = self._try_usb()
                if found:
                    self._drop_link("USB 已插回，改走 USB")
                    self._use(found, now)
            return
        if now < self._next_retry:
            return
        self._next_retry = now + RETRY_SECONDS
        found = self._try_usb() or self._try_ble()
        if found:
            self._use(found, now)

    def _read_device(self):
        """读设备输出：HELLO 表示设备重启过，需要立刻重发状态；日志行忽略。"""
        if self._link is None:
            return
        try:
            lines = self._link.read_lines()
        except LinkClosed:
            self._drop_link("设备断开")
            return
        for line in lines:
            msg = parse_device_line(line)
            if msg and msg.get("type") == "HELLO":
                log.info("设备重新上线（固件 %s）", msg.get("fw", "?"))
                self._last_sent = None
                self._last_agent = {}

    def _send(self, snap, now):
        """发送一帧状态；写失败时断开链路，等待下次重连。"""
        line = encode_state(snap, self._seq)
        if self._link.write_line(line):
            self._seq = (self._seq + 1) % 65536
            self._last_sent = snap
            self._last_send_at = now
        else:
            self._drop_link("写入失败")
            self._next_retry = now + RETRY_SECONDS

    def _agent_values(self, tool, counts, now):
        """返回某个工具 A 帧的 (计数, 5 小时窗口, 每周窗口)；演示模式下用示例数据。"""
        if self._demo_start is not None:
            demo_counts, five, seven = DEMO_AGENTS[tool]
            to_window = lambda w: w if w == NO_LIMIT else Window(w[0], now + w[1])
            return demo_counts, to_window(five), to_window(seven)
        usage = self._usage.get(tool)
        return counts[tool], usage.five_hour, usage.seven_day

    def _send_agents(self, now):
        """按工具发送 A 帧：内容变化时才发（距重置分钟数每分钟变一次），不随心跳重发。"""
        counts = self._agg.tool_counts()
        for tool in AGENT_TOOLS:
            line = encode_agent(tool, *self._agent_values(tool, counts, now), now)
            if line == self._last_agent.get(tool):
                continue
            if not self._link.write_line(line):
                self._drop_link("写入失败")
                self._next_retry = now + RETRY_SECONDS
                return
            self._last_agent[tool] = line

    def _drop_link(self, reason):
        if self._link is not None:
            log.info("断开设备链路：%s", reason)
            self._link.close()
            self._link = None
            self._port = None

    # ------------------------------------------------------------------
    # 状态文件
    # ------------------------------------------------------------------

    def _write_status(self, now):
        """内容变化或每 5 秒刷新 status.json（updated 字段用于判断守护进程是否存活）。"""
        snap = self._agg.snapshot()
        status = {
            "cc": snap.cc,
            "cx": snap.cx,
            "ds": snap.ds,
            "done": snap.done,
            "wn": snap.wn,
            "waiting": [list(item) for item in snap.waiting],
            "link": self._link is not None,
            "link_type": self._link_kind(),
            "port": self._port,
            "demo": self._demo_start is not None,
            "recent": self._agg.recent_events(),
            "agents": self._agents_status(),
        }
        if status == self._last_status and now - self._last_status_at < STATUS_REFRESH_SECONDS:
            return
        self._last_status = status
        self._last_status_at = now
        _write_json_atomic(self.status_path, dict(status, updated=now))

    def _agents_status(self):
        """status.json 的 agents 部分：每个工具的计数、用量、数据时间与最近一次失败原因。"""
        counts = self._agg.tool_counts()
        agents = {}
        for tool in AGENT_TOOLS:
            usage = self._usage.get(tool)
            agents[tool] = {
                "wn": counts[tool].wn,
                "bg": counts[tool].bg,
                "done": counts[tool].done,
                "five_hour": _window_json(usage.five_hour),
                "seven_day": _window_json(usage.seven_day),
                "usage_updated": usage.updated,
                "usage_error": usage.error,
            }
        return agents

    def _done_state(self):
        """state.json 的内容：日期、今日完成总数与按工具的完成数。"""
        day, done = self._agg.done_record()
        return {"day": day, "done": done, "done_by_tool": self._agg.done_by_tool()}

    def _persist_done(self):
        """今日完成数变化时写入 state.json，守护进程重启后可恢复。"""
        record = self._done_state()
        if record != self._last_done:
            self._last_done = record
            _write_json_atomic(self.state_path, record)
