#!/usr/bin/env python3
"""生成 README 用的真机截图：依次把设备切到各个画面并截屏，保存到 docs/images/。

用法（需先停止采集程序，让出串口）：
    launchctl bootout gui/$(id -u)/local.agent-run
    /usr/bin/python3 scripts/readme_screens.py [--port /dev/cu.usbmodemXXXX] [--scale 2]
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.agent-run.plist

设置菜单、专用页面这类要按键才能进入的画面，用连续截屏模式：脚本持续发示例状态帧并不停
截屏，画面每变一次就存一张，期间由人在设备上按键，事后从中挑图改名：
    /usr/bin/python3 scripts/readme_screens.py --watch 300 --out /tmp/screens [--minute 1380]

--minute 是随状态帧发送的本地时间（当日分钟数），例如 1380 表示 23:00，可以让夜间勿扰生效。
画面内容是固定的示例数字，不是真实会话数据。
"""
import argparse
import hashlib
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "core"))

from agent_run import screenshot  # noqa: E402
from agent_run.ports import find_espressif_ports  # noqa: E402
from agent_run.protocol import encode_state  # noqa: E402
from agent_run.serial_link import SerialLink  # noqa: E402
from agent_run.state import Snapshot  # noqa: E402

# (文件名, 快照)：与设计稿一致的六种在线画面
SCREENS = [
    ("sleep", Snapshot(cc=0, cx=0, ds=0, done=12, wn=0, waiting=[])),
    ("walk", Snapshot(cc=1, cx=0, ds=0, done=13, wn=0, waiting=[])),
    ("trot", Snapshot(cc=2, cx=1, ds=0, done=16, wn=0, waiting=[])),
    ("run", Snapshot(cc=3, cx=1, ds=1, done=21, wn=0, waiting=[])),
    ("dash", Snapshot(cc=4, cx=2, ds=2, done=34, wn=0, waiting=[])),
    ("waiting", Snapshot(cc=2, cx=1, ds=0, done=21, wn=2, waiting=[("claude", "A"), ("codex", "Q")])),
]

# 等“完成 +N”提示结束的时间：固件 AR_DONE_FLASH_MS 为 5 秒，多留 0.5 秒余量；
# 仍短于 USB 链路 10 秒超时，等待期间设备不会判定未连接
DONE_FLASH_WAIT_S = 5.5


def save(rgb, width, height, scale, path):
    rgb, width, height = screenshot.scale_rgb(rgb, width, height, scale)
    with open(path, "wb") as f:
        f.write(screenshot.encode_png(rgb, width, height))
    print("已保存 %s" % os.path.relpath(path, ROOT))


def watch(link, seconds, minute, out, scale):
    """连续截屏：每约 2 秒发一帧示例状态（“狂奔”），其余时间不停截屏；与之前任何一张都不同的
    画面存成 watch-NNN.png。宠物动画会让首页每张都不同，菜单面板盖住场景时画面才会稳定。"""
    snap = dict(SCREENS)["run"]
    seen = set()
    seq, next_frame, saved = 0, 0.0, 0
    end = time.time() + seconds
    while time.time() < end:
        now = time.time()
        if now >= next_frame:
            link.write_line(encode_state(snap, seq, minute=minute))
            seq += 1
            next_frame = now + 2.0
        width, height, rgb = screenshot.capture(link)
        digest = hashlib.sha1(rgb).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        save(rgb, width, height, scale, os.path.join(out, "watch-%03d.png" % saved))
        saved += 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", help="设备串口，默认按 Espressif 厂商号自动查找")
    parser.add_argument("--scale", type=int, default=2, help="最近邻放大倍数，默认 2")
    parser.add_argument("--out", default=os.path.join(ROOT, "docs", "images"), help="输出目录")
    parser.add_argument("--watch", type=int, metavar="SECONDS",
                        help="连续截屏这么多秒，画面每变一次存一张，用于要按键才能进入的画面")
    parser.add_argument("--minute", type=int, help="随状态帧发送的本地时间（当日分钟数 0-1439），只用于 --watch")
    args = parser.parse_args()

    port = args.port or (find_espressif_ports() or [None])[0]
    if not port:
        print("没有找到设备串口")
        return 1
    os.makedirs(args.out, exist_ok=True)
    link = SerialLink(port)
    link.open()
    if args.watch:
        try:
            return watch(link, args.watch, args.minute, args.out, args.scale)
        finally:
            link.close()
    try:
        for seq, (name, snap) in enumerate(SCREENS):
            # 连发两帧并等画面稳定（切换后会立即重画，动画帧间隔最长 600 ms）。
            # 完成数比设备上一帧大时，固件会用“完成 +N”替换状态行 5 秒，
            # 所以第一帧之后先等这段提示过去，再发完成数不变的第二帧并截屏。
            link.write_line(encode_state(snap, seq * 2))
            time.sleep(DONE_FLASH_WAIT_S)
            link.write_line(encode_state(snap, seq * 2 + 1))
            time.sleep(1.0)
            width, height, rgb = screenshot.capture(link)
            save(rgb, width, height, args.scale, os.path.join(args.out, "screen-%s.png" % name))
        # “未连接”：停发状态帧，等设备超时（USB 10 秒）
        print("等待设备判定未连接……")
        time.sleep(11.5)
        width, height, rgb = screenshot.capture(link)
        save(rgb, width, height, args.scale, os.path.join(args.out, "screen-disconnected.png"))
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
