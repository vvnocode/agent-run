"""hook 安装卸载、文件部署与 launchd 注册（设计方案 §5.1、§5.2）。

原则：
- 修改 Agent 配置前先备份；
- 只追加自己的条目（按 hook 脚本路径识别），不改动其他 hook；
- 条目追加在各事件列表末尾，Codex 按位置记录的信任状态不受影响。
"""
import json
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
import time

LAUNCHD_LABEL = "local.agent-run"

# Claude Code 需要监听的事件（设计方案 §5.2 映射表）
CLAUDE_EVENTS = [
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionRequest",
    "PermissionDenied",
    "Notification",
    "Stop",
    "StopFailure",
    "SessionEnd",
]

# Codex 需要监听的事件
CODEX_EVENTS = [
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PermissionRequest",
    "Stop",
    "Interrupt",
    "SessionEnd",
]


# dsh 的 Claude Code 兼容 hook 插件（@deepseek-ai/dsh-hooks-claude-code）支持的事件
DSH_EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop"]


def dsh_hooks_config(python, hook_path):
    """生成给 dsh hook 插件读取的 hooks.json（Claude Code 格式），命令以 dsh 身份上报。

    插件的超时字段单位与 Claude Code 相同（秒）；不声明 async，插件未必支持。
    """
    entry = {"type": "command", "command": "%s %s dsh" % (python, hook_path), "timeout": 2}
    return {"hooks": {event: [{"hooks": [dict(entry)]}] for event in DSH_EVENTS}}


def _load(path):
    """读 JSON 配置；文件不存在时返回空对象。"""
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def _save(path, data):
    """原子写回 JSON 配置，保持缩进可读。"""
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = path + ".agent-run.tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def _backup(paths, backup_dir):
    """把已存在的配置文件复制到 backup_dir/<时间戳-随机后缀>/，返回备份目录。"""
    os.makedirs(backup_dir, exist_ok=True)
    target = tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=backup_dir)
    for path in paths:
        if os.path.exists(path):
            shutil.copy2(path, os.path.join(target, os.path.basename(path)))
    return target


def _has_our_hook(groups, hook_path):
    """事件下是否已有指向本 hook 脚本的条目。"""
    for group in groups:
        for hook in group.get("hooks", []):
            if hook_path in str(hook.get("command", "")):
                return True
    return False


def _add_hooks(path, tool, events, python, hook_path, extra):
    """给一个配置文件的每个事件追加本工具的 hook 条目。"""
    data = _load(path)
    hooks = data.setdefault("hooks", {})
    entry = {"type": "command", "command": "%s %s %s" % (python, hook_path, tool), "async": True}
    entry.update(extra)
    for event in events:
        groups = hooks.setdefault(event, [])
        if not _has_our_hook(groups, hook_path):
            groups.append({"hooks": [dict(entry)]})
    _save(path, data)


def _remove_hooks(path, hook_path):
    """删除配置文件中指向本 hook 脚本的条目；因此变空的分组与事件一并删除。"""
    if not os.path.exists(path):
        return
    data = _load(path)
    hooks = data.get("hooks")
    if not isinstance(hooks, dict):
        return
    for event in list(hooks):
        kept_groups = []
        for group in hooks[event]:
            original = group.get("hooks", [])
            kept = [h for h in original if hook_path not in str(h.get("command", ""))]
            if kept:
                kept_groups.append(dict(group, hooks=kept))
            elif not original:
                kept_groups.append(group)  # 本来就为空的分组不是我们造成的，保留
        if kept_groups:
            hooks[event] = kept_groups
        else:
            del hooks[event]
    if not hooks:
        del data["hooks"]
    _save(path, data)


def _snake_case(event):
    """PreToolUse -> pre_tool_use，与 Codex [hooks.state] 键名中的事件写法一致。"""
    return "".join("_" + c.lower() if c.isupper() else c for c in event).lstrip("_")


def codex_untrusted_events(hooks_path, config_path, hook_path):
    """返回本工具在 Codex 中尚无信任记录的 hook 事件名。

    Codex 只执行审查并信任过的 hook，信任记录写在 config.toml 的
    [hooks.state."<hooks.json 路径>:<事件>:<分组下标>:<条目下标>"] 下。
    这里只检查记录是否存在；命令改动后哈希失效的情况无法从这里判断。
    """
    try:
        hooks = _load(hooks_path).get("hooks", {})
    except (OSError, ValueError):
        return []
    try:
        with open(config_path, encoding="utf-8") as f:
            config = f.read()
    except OSError:
        config = ""
    trusted = set(re.findall(r'^\[hooks\.state\."([^"]+)"\]', config, re.M))
    prefix = os.path.abspath(os.path.expanduser(hooks_path))
    untrusted = []
    for event, groups in hooks.items():
        for gi, group in enumerate(groups):
            for hi, hook in enumerate(group.get("hooks", [])):
                if hook_path not in str(hook.get("command", "")):
                    continue
                key = "%s:%s:%d:%d" % (prefix, _snake_case(event), gi, hi)
                if key not in trusted and event not in untrusted:
                    untrusted.append(event)
    return untrusted


def install_hooks(claude_path, codex_path, python, hook_path, backup_dir):
    """为 Claude Code 与 Codex 安装 hook；返回备份目录。重复执行不会重复添加。"""
    backup = _backup([claude_path, codex_path], backup_dir)
    _add_hooks(claude_path, "claude", CLAUDE_EVENTS, python, hook_path, {})
    # Codex 的 timeout 单位是秒；SessionEnd 始终同步执行，默认只有 1 秒
    _add_hooks(codex_path, "codex", CODEX_EVENTS, python, hook_path, {"timeout": 2})
    return backup


def uninstall_hooks(claude_path, codex_path, hook_path, backup_dir):
    """删除两份配置中本 hook 的条目；返回备份目录。"""
    backup = _backup([claude_path, codex_path], backup_dir)
    _remove_hooks(claude_path, hook_path)
    _remove_hooks(codex_path, hook_path)
    return backup


# ----------------------------------------------------------------------
# 部署与 launchd
# ----------------------------------------------------------------------

def deploy(src_host_dir, home):
    """把 agent_run 包与 bin 脚本复制到 home（~/.agent-run），使运行不依赖仓库路径。"""
    lib = os.path.join(home, "lib")
    bin_dir = os.path.join(home, "bin")
    hook_dst = os.path.join(bin_dir, "ar_hook.py")
    script_dst = os.path.join(bin_dir, "agent-run")
    # 从已部署的副本执行时（来源就是 home/lib），程序已经就位：直接返回，
    # 否则下面先删目标再复制会把来源一起删掉
    if os.path.realpath(src_host_dir) == os.path.realpath(lib):
        return hook_dst, script_dst
    os.makedirs(lib, exist_ok=True)
    os.makedirs(bin_dir, exist_ok=True)
    os.makedirs(os.path.join(home, "log"), exist_ok=True)
    target_pkg = os.path.join(lib, "agent_run")
    if os.path.isdir(target_pkg):
        shutil.rmtree(target_pkg)
    shutil.copytree(os.path.join(src_host_dir, "agent_run"), target_pkg,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for name, dst in (("ar_hook.py", hook_dst), ("agent-run", script_dst)):
        shutil.copy2(os.path.join(src_host_dir, "bin", name), dst)
        os.chmod(dst, 0o755)
    return hook_dst, script_dst


def launchd_plist(python, script, home, label=LAUNCHD_LABEL):
    """生成 LaunchAgent 的 plist 内容：登录后启动，异常退出自动拉起。"""
    log_path = os.path.join(home, "log", "agent-run.log")
    return plistlib.dumps({
        "Label": label,
        "ProgramArguments": [python, script, "run"],
        "EnvironmentVariables": {"AGENT_RUN_HOME": home},
        "RunAtLoad": True,
        "KeepAlive": True,
        "StandardOutPath": log_path,
        "StandardErrorPath": log_path,
    })


BRIDGE_LABEL = "local.agent-run.bridge"


def bridge_plist(app_path, home, label=BRIDGE_LABEL):
    """蓝牙桥接程序的 LaunchAgent：直接运行 .app 里的可执行文件，异常退出自动拉起。"""
    log_path = os.path.join(home, "log", "bridge.log")
    return plistlib.dumps({
        "Label": label,
        "ProgramArguments": [os.path.join(app_path, "Contents", "MacOS", "AgentRunBridge")],
        "EnvironmentVariables": {"AGENT_RUN_HOME": home},
        "RunAtLoad": True,
        "KeepAlive": True,
        "StandardOutPath": log_path,
        "StandardErrorPath": log_path,
    })


def plist_path(label=LAUNCHD_LABEL):
    return os.path.expanduser("~/Library/LaunchAgents/%s.plist" % label)


def _bootstrap(label, content):
    """写入 plist 并用 launchctl 加载（已加载时先卸载再加载，使新代码生效）。"""
    path = plist_path(label)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(content)
    domain = "gui/%d" % os.getuid()
    subprocess.run(["launchctl", "bootout", "%s/%s" % (domain, label)], capture_output=True)
    result = subprocess.run(["launchctl", "bootstrap", domain, path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("launchctl bootstrap 失败：%s" % result.stderr.strip())
    return path


def install_bridge_service(app_path, home):
    """注册蓝牙桥接程序的 LaunchAgent。"""
    return _bootstrap(BRIDGE_LABEL, bridge_plist(app_path, home))


def install_service(python, script, home, label=LAUNCHD_LABEL):
    """写入 plist 并用 launchctl 加载（已加载时先卸载再加载，使新代码生效）。"""
    path = plist_path(label)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(launchd_plist(python, script, home, label))
    domain = "gui/%d" % os.getuid()
    subprocess.run(["launchctl", "bootout", "%s/%s" % (domain, label)], capture_output=True)
    result = subprocess.run(["launchctl", "bootstrap", domain, path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("launchctl bootstrap 失败：%s" % result.stderr.strip())
    return path


def uninstall_service(label=LAUNCHD_LABEL):
    """卸载 LaunchAgent 并删除 plist；运行数据保留。"""
    domain = "gui/%d" % os.getuid()
    subprocess.run(["launchctl", "bootout", "%s/%s" % (domain, label)], capture_output=True)
    path = plist_path(label)
    if os.path.exists(path):
        os.remove(path)
