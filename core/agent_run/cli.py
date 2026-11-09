"""agent-run 命令行（设计方案 §5.6）。

子命令：
  run               前台运行守护进程（launchd 调用的就是它）
  status            打印守护进程汇总的当前状态
  demo              让设备依次演示七种画面
  setup             查找 AI Passport 的串口，握手确认后写入 config.json
  install-hooks     部署脚本并为 Claude Code、Codex 安装 hook（先备份）
  uninstall-hooks   删除本工具安装的 hook
  install-service   部署脚本并注册 launchd，登录后自动运行
  uninstall-service 注销 launchd（保留运行数据）
  install-dsh-hooks 生成 dsh hook 插件用的配置，并打印挂载方法
  install-bridge-service / uninstall-bridge-service  注册或注销蓝牙桥接程序
  screenshot        经 USB 截取设备屏幕（需先停止守护进程）
"""
import argparse
import json
import logging
import math
import os
import socket
import sys
import time

from . import install
from .ble_link import BleLink
from .daemon import DEMO_PAUSE_SECONDS, DEMO_SCREENS, DEMO_STEP_SECONDS, Daemon, default_home
from .ports import find_espressif_ports
from .serial_link import SerialLink
from .usage import UsageCollector

# hook 与 launchd 统一使用系统自带的 Python，避免依赖用户环境里的解释器
SYSTEM_PYTHON = "/usr/bin/python3"

# 仓库中 host/ 目录（部署来源）
HOST_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_config(home):
    try:
        with open(os.path.join(home, "config.json")) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _port_finder(home):
    """优先用 config.json 记录的端口；未记录时按 Espressif 厂商号自动查找。"""
    def find():
        config = _load_config(home)
        port = config.get("port")
        if port:
            return port if os.path.exists(port) else None
        ports = find_espressif_ports()
        return ports[0] if ports else None
    return find


def cmd_run(args):
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    home = args.home
    config = _load_config(home)
    run_dir = os.path.join(home, "run")
    daemon = Daemon(
        home=home,
        port_finder=_port_finder(home),
        dsh_root=os.path.expanduser(config.get("dsh_root", "~/.dsh/sessions")),
        rules=config.get("rules"),
        # 没有 USB 时经蓝牙桥接程序发送
        ble_factory=lambda: BleLink(os.path.join(run_dir, "ble.sock"), os.path.join(run_dir, "ble-status.json")),
        # Claude Code、Codex 的订阅用量，供设备上的专用页面显示
        usage=UsageCollector(),
    )
    daemon.start()
    try:
        daemon.run_forever()
    except KeyboardInterrupt:
        pass
    return 0


def _countdown(seconds):
    """距重置时间的文字，与设备专用页面的写法一致（专用页面方案 §4.2）。"""
    minutes = max(1, int(math.ceil(seconds / 60.0)))
    if minutes < 60:
        return "%d 分钟后重置" % minutes
    if minutes < 24 * 60:
        return "%d 小时 %d 分后重置" % (minutes // 60, minutes % 60)
    return "%d 天 %d 小时后重置" % (minutes // (24 * 60), minutes % (24 * 60) // 60)


def describe_window(value, now):
    """status.json 里一个用量窗口的文字：百分比与距重置时间、无此限额或暂无数据。"""
    if value is None:
        return "暂无数据"
    if value == "none":
        return "无此限额"
    resets_at = value.get("resets_at")
    if resets_at is not None and resets_at <= now:
        return "0%（已重置）"
    percent = int(math.floor(value.get("used", 0) + 0.5))
    if resets_at is None:
        return "%d%%（重置时间未知）" % percent
    return "%d%%（%s）" % (percent, _countdown(resets_at - now))


def cmd_status(args):
    path = os.path.join(args.home, "run", "status.json")
    try:
        with open(path) as f:
            status = json.load(f)
    except (OSError, ValueError):
        print("守护进程没有运行（找不到 %s）。" % path)
        return 1
    age = time.time() - status.get("updated", 0)
    if age > 10:
        print("警告：状态已 %d 秒未更新，守护进程可能已停止。" % age)
    via = {"usb": "USB", "ble": "蓝牙"}.get(status.get("link_type"), "")
    link = "已连接 %s（%s）" % (status.get("port"), via) if status.get("link") else "未连接"
    print("设备链路：%s%s" % (link, "（演示中）" if status.get("demo") else ""))
    print("工作中：Claude Code %d · Codex %d · dsh %d" % (status["cc"], status["cx"], status["ds"]))
    print("今日完成：%d" % status["done"])
    reasons = {"A": "等待授权", "Q": "等你回答"}
    names = {"claude": "Claude Code", "codex": "Codex", "dsh": "dsh"}
    print("等你处理：%d" % status["wn"])
    for tool, reason in status.get("waiting", []):
        print("  - %s：%s" % (names.get(tool, tool), reasons.get(reason, reason)))

    # 专用页面的数据：按工具的计数与订阅用量
    now = time.time()
    for tool, agent in status.get("agents", {}).items():
        plan = agent.get("plan")    # 套餐显示名；旧版守护进程的 status.json 没有这一项
        print("%s%s：等你处理 %d · 后台运行 %d · 今日完成 %d" % (
            names.get(tool, tool), "（%s）" % plan if plan else "", agent["wn"], agent["bg"], agent["done"]))
        print("  5 小时用量 %s · 每周用量 %s" % (
            describe_window(agent.get("five_hour"), now), describe_window(agent.get("seven_day"), now)))
        updated = agent.get("usage_updated")
        if updated:
            print("  用量数据时间：%d 分钟前" % max(0, (now - updated) // 60))
        if agent.get("usage_error"):
            print("  用量未更新：%s" % agent["usage_error"])

    # Codex 会静默跳过未信任的 hook，这里主动提示，避免“跑了任务却不计数”
    untrusted = install.codex_untrusted_events(
        os.path.expanduser("~/.codex/hooks.json"),
        os.path.expanduser("~/.codex/config.toml"),
        os.path.join(args.home, "bin", "ar_hook.py"),
    )
    if untrusted:
        print("注意：Codex 有 %d 个 agent-run hook 尚未信任（%s），Codex 会话不会计入。" % (len(untrusted), "、".join(untrusted)))
        print("      请在 Codex 桌面端 Settings → Hooks，或 CLI 的 /hooks 中审查并信任。")

    # 蓝牙桥接程序报告需要用户处理的问题（如重新烧录后配对失效）
    try:
        with open(os.path.join(args.home, "run", "ble-status.json")) as f:
            bridge_hint = json.load(f).get("hint")
    except (OSError, ValueError, AttributeError):
        bridge_hint = None
    if bridge_hint:
        print("蓝牙：%s" % bridge_hint)

    if getattr(args, "events", False):
        print("最近收到的事件：")
        now = time.time()
        for item in status.get("recent", []):
            detail = item.get("tool_name") or item.get("ntype") or ""
            print("  %5.0f 秒前  %-6s %-18s %s" % (now - item["ts"], item["tool"], item["event"], detail))
    return 0


def cmd_install_dsh_hooks(args):
    """写出 dsh hook 插件用的配置文件，并打印需要加到 dsh profile 里的配置。"""
    hook_path, _ = install.deploy(HOST_DIR, args.home)
    path = os.path.join(args.home, "dsh-hooks.json")
    with open(path, "w") as f:
        json.dump(install.dsh_hooks_config(SYSTEM_PYTHON, hook_path), f, indent=2)
        f.write("\n")
    print("已写出 %s。" % path)
    print("在要统计的 dsh profile 的 cordis.patch.yml 中挂载插件：")
    print("  - name: '@deepseek-ai/dsh-hooks-claude-code'")
    print("    config:")
    print("      configPath: %s" % path)
    return 0


def cmd_demo(args):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.sendto(b'{"ctl":"demo"}', os.path.join(args.home, "run", "ar.sock"))
    except OSError:
        print("发送失败：守护进程没有运行。")
        return 1
    finally:
        sock.close()
    total = len(DEMO_SCREENS) * DEMO_STEP_SECONDS + DEMO_PAUSE_SECONDS
    print("已开始演示：睡觉 → 慢走 → 小跑 → 狂奔 → 冲刺 → 等你处理 → 未连接，共约 %d 秒。" % total)
    return 0


def cmd_setup(args):
    ports = find_espressif_ports()
    if not ports:
        print("没有找到 Espressif 的 USB 串口。请确认 AI Passport 已开机，并用支持数据传输的线连接电脑。")
        return 1
    for port in ports:
        link = SerialLink(port)
        try:
            link.open()
            hello = link.handshake(timeout=1.5)
        except OSError as exc:
            print("%s：打开失败（%s）。如果 agent-run 守护进程正在运行，它可能已占用该端口。" % (port, exc))
            continue
        finally:
            link.close()
        if hello:
            config = _load_config(args.home)
            config["port"] = port
            os.makedirs(args.home, exist_ok=True)
            with open(os.path.join(args.home, "config.json"), "w") as f:
                json.dump(config, f, indent=2, ensure_ascii=False)
            print("已找到 AI Passport：%s（固件 %s），已写入 config.json。" % (port, hello.get("fw", "?")))
            return 0
        print("%s：没有收到握手回复，设备上可能还不是 Agent Run! 固件。" % port)
    return 1


def cmd_screenshot(args):
    """经 USB 截取设备屏幕，保存为 PNG。守护进程运行时占用串口，需先停止。"""
    from . import screenshot

    port = _port_finder(args.home)()
    if not port:
        print("没有找到 AI Passport 的 USB 串口。")
        return 1
    link = SerialLink(port)
    try:
        link.open()
    except OSError as exc:
        print("打开 %s 失败（%s）。截屏前请先停止采集程序：launchctl bootout gui/$(id -u)/local.agent-run" % (port, exc))
        return 1
    try:
        width, height, rgb = screenshot.capture(link)
    except screenshot.ScreenshotError as exc:
        print("截屏失败：%s" % exc)
        return 1
    finally:
        link.close()
    rgb, width, height = screenshot.scale_rgb(rgb, width, height, args.scale)
    with open(args.output, "wb") as f:
        f.write(screenshot.encode_png(rgb, width, height))
    print("已保存 %s（%d×%d）" % (args.output, width, height))
    return 0


def _agent_config_paths():
    return os.path.expanduser("~/.claude/settings.json"), os.path.expanduser("~/.codex/hooks.json")


def cmd_install_hooks(args):
    hook_path, _ = install.deploy(HOST_DIR, args.home)
    claude, codex = _agent_config_paths()
    backup = install.install_hooks(claude, codex, SYSTEM_PYTHON, hook_path, os.path.join(args.home, "backup"))
    print("已为 Claude Code 和 Codex 安装 hook，原配置备份在 %s。" % backup)
    print("Codex 需要在 CLI 的 /hooks 或桌面端 Settings → Hooks 里审查并信任新 hook 后才会执行。")
    return 0


def cmd_uninstall_hooks(args):
    hook_path = os.path.join(args.home, "bin", "ar_hook.py")
    claude, codex = _agent_config_paths()
    backup = install.uninstall_hooks(claude, codex, hook_path, os.path.join(args.home, "backup"))
    print("已删除 agent-run 的 hook，修改前的配置备份在 %s。" % backup)
    return 0


def cmd_install_service(args):
    _, script = install.deploy(HOST_DIR, args.home)
    path = install.install_service(SYSTEM_PYTHON, script, args.home)
    print("已注册 launchd：%s，守护进程已启动并会在登录后自动运行。" % path)
    return 0


def cmd_install_bridge_service(args):
    """注册蓝牙桥接程序（需先用 bridge/build.sh 构建并安装 .app）。"""
    app = os.path.expanduser("~/Applications/AgentRunBridge.app")
    if not os.path.exists(os.path.join(app, "Contents", "MacOS", "AgentRunBridge")):
        print("没有找到 %s，请先运行 transports/macos-ble/build.sh。" % app)
        return 1
    path = install.install_bridge_service(app, args.home)
    print("已注册 launchd：%s。首次运行时 macOS 会请求蓝牙权限，请点“允许”。" % path)
    return 0


def cmd_uninstall_bridge_service(args):
    install.uninstall_service(install.BRIDGE_LABEL)
    print("已注销蓝牙桥接程序的登录自启；如需彻底移除，再删除 ~/Applications/AgentRunBridge.app。")
    return 0


def cmd_uninstall_service(args):
    install.uninstall_service()
    print("已注销 launchd，运行数据保留在 %s。" % args.home)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="agent-run", description="Agent Run! 主机采集端")
    parser.add_argument("--home", default=default_home(), help="运行数据目录，默认 ~/.agent-run")
    sub = parser.add_subparsers(dest="command", required=True)
    status = sub.add_parser("status")
    status.add_argument("--events", action="store_true", help="同时列出最近收到的事件")
    status.set_defaults(func=cmd_status)
    shot = sub.add_parser("screenshot", help="经 USB 截取设备屏幕（需先停止采集程序）")
    shot.add_argument("-o", "--output", default="agent-run-screen.png", help="输出 PNG 路径")
    shot.add_argument("--scale", type=int, default=1, help="最近邻放大倍数")
    shot.set_defaults(func=cmd_screenshot)
    for name, func in [
        ("run", cmd_run),
        ("demo", cmd_demo),
        ("install-dsh-hooks", cmd_install_dsh_hooks),
        ("setup", cmd_setup),
        ("install-hooks", cmd_install_hooks),
        ("uninstall-hooks", cmd_uninstall_hooks),
        ("install-service", cmd_install_service),
        ("install-bridge-service", cmd_install_bridge_service),
        ("uninstall-bridge-service", cmd_uninstall_bridge_service),
        ("uninstall-service", cmd_uninstall_service),
    ]:
        sub.add_parser(name).set_defaults(func=func)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
