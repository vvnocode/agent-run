"""hook 安装与卸载测试（设计方案 §5.2「安装与卸载」）。"""
import json
import os
import plistlib
import tempfile
import unittest

from agent_run.install import (
    CLAUDE_EVENTS,
    CODEX_EVENTS,
    codex_untrusted_events,
    install_hooks,
    launchd_plist,
    uninstall_hooks,
)

HOOK = "/Users/test/.agent-run/bin/ar_hook.py"


def write_json(path, data):
    with open(path, "w") as f:
        json.dump(data, f)


def read_json(path):
    with open(path) as f:
        return json.load(f)


class InstallHooksTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.claude = os.path.join(self.dir, "settings.json")
        self.codex = os.path.join(self.dir, "hooks.json")
        self.backup = os.path.join(self.dir, "backup")
        self.other = {"type": "command", "command": "/usr/local/bin/other-tool"}
        write_json(self.claude, {
            "model": "opus",
            "hooks": {"Stop": [{"hooks": [self.other]}]},
        })
        write_json(self.codex, {"hooks": {"Stop": [{"hooks": [self.other]}]}})

    def install(self):
        return install_hooks(self.claude, self.codex, "/usr/bin/python3", HOOK, self.backup)

    def test_appends_entries_for_every_event_and_keeps_others(self):
        self.install()
        claude = read_json(self.claude)
        self.assertEqual(claude["model"], "opus")
        self.assertEqual(set(claude["hooks"]), set(CLAUDE_EVENTS))
        self.assertIn({"hooks": [self.other]}, claude["hooks"]["Stop"])
        ours = [h for g in claude["hooks"]["Stop"] for h in g["hooks"] if HOOK in h["command"]]
        self.assertEqual(ours, [{"type": "command", "command": "/usr/bin/python3 %s claude" % HOOK, "async": True}])
        codex = read_json(self.codex)
        self.assertEqual(set(codex["hooks"]), set(CODEX_EVENTS))
        ours = [h for g in codex["hooks"]["Stop"] for h in g["hooks"] if HOOK in h["command"]]
        self.assertEqual(ours, [{"type": "command", "command": "/usr/bin/python3 %s codex" % HOOK, "async": True, "timeout": 2}])

    def test_install_is_idempotent(self):
        self.install()
        first = read_json(self.claude)
        self.install()
        self.assertEqual(read_json(self.claude), first)

    def test_backs_up_existing_files_before_writing(self):
        self.install()
        backups = []
        for root, _, files in os.walk(self.backup):
            backups.extend(files)
        self.assertEqual(sorted(backups), ["hooks.json", "settings.json"])

    def test_missing_files_are_created(self):
        os.remove(self.claude)
        os.remove(self.codex)
        self.install()
        self.assertEqual(set(read_json(self.claude)["hooks"]), set(CLAUDE_EVENTS))
        self.assertEqual(set(read_json(self.codex)["hooks"]), set(CODEX_EVENTS))

    def test_uninstall_removes_only_our_entries(self):
        before_claude = read_json(self.claude)
        before_codex = read_json(self.codex)
        self.install()
        uninstall_hooks(self.claude, self.codex, HOOK, self.backup)
        self.assertEqual(read_json(self.claude), before_claude)
        self.assertEqual(read_json(self.codex), before_codex)


class CodexTrustTest(unittest.TestCase):
    """Codex 只执行在 config.toml [hooks.state] 里有信任记录的 hook。"""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.hooks = os.path.join(self.dir, "hooks.json")
        self.config = os.path.join(self.dir, "config.toml")
        other = {"type": "command", "command": "/usr/local/bin/other-tool"}
        write_json(self.hooks, {"hooks": {"Stop": [{"hooks": [other]}], "PreToolUse": [{"hooks": [other]}]}})
        install_hooks(os.path.join(self.dir, "settings.json"), self.hooks, "/usr/bin/python3", HOOK,
                      os.path.join(self.dir, "backup"))

    def write_config(self, keys):
        lines = ["[features]", "hooks = true", "", "[hooks.state]"]
        for key in keys:
            lines += ['[hooks.state."%s"]' % key, 'trusted_hash = "sha256:abc"', ""]
        with open(self.config, "w") as f:
            f.write("\n".join(lines))

    def test_untrusted_entries_are_reported(self):
        # 只有原有工具的条目被信任：agent-run 追加在下标 1 的条目都未信任
        self.write_config(["%s:stop:0:0" % self.hooks, "%s:pre_tool_use:0:0" % self.hooks])
        self.assertEqual(sorted(codex_untrusted_events(self.hooks, self.config, HOOK)), sorted(CODEX_EVENTS))

    def test_trusted_entries_use_group_and_hook_index(self):
        keys = []
        for event in CODEX_EVENTS:
            snake = "".join("_" + c.lower() if c.isupper() else c for c in event).lstrip("_")
            index = 1 if event in ("Stop", "PreToolUse") else 0
            keys.append("%s:%s:%d:0" % (self.hooks, snake, index))
        self.write_config(keys[:-1])
        self.assertEqual(codex_untrusted_events(self.hooks, self.config, HOOK), [CODEX_EVENTS[-1]])
        self.write_config(keys)
        self.assertEqual(codex_untrusted_events(self.hooks, self.config, HOOK), [])

    def test_missing_files(self):
        self.assertEqual(codex_untrusted_events(os.path.join(self.dir, "none.json"), self.config, HOOK), [])
        self.assertEqual(sorted(codex_untrusted_events(self.hooks, os.path.join(self.dir, "none.toml"), HOOK)),
                         sorted(CODEX_EVENTS))


class DeployTest(unittest.TestCase):
    def test_redeploy_from_deployed_copy_keeps_files(self):
        # 已部署的 agent-run 再执行部署时，来源就是目标本身：不能先删后拷
        from agent_run.install import deploy
        host_dir = os.path.join(os.path.dirname(__file__), "..")
        home = tempfile.mkdtemp()
        deploy(host_dir, home)
        deployed_host = os.path.join(home, "lib")
        hook, script = deploy(deployed_host, home)
        self.assertTrue(os.path.isfile(os.path.join(home, "lib", "agent_run", "state.py")))
        self.assertTrue(os.path.isfile(hook) and os.path.isfile(script))


class DshHooksTest(unittest.TestCase):
    def test_dsh_hooks_config_uses_claude_code_format(self):
        from agent_run.install import DSH_EVENTS, dsh_hooks_config
        config = dsh_hooks_config("/usr/bin/python3", HOOK)
        self.assertEqual(set(config["hooks"]), set(DSH_EVENTS))
        entry = config["hooks"]["PreToolUse"][0]["hooks"][0]
        self.assertEqual(entry["command"], "/usr/bin/python3 %s dsh" % HOOK)
        self.assertEqual(entry["type"], "command")
        self.assertEqual(entry["timeout"], 2)


class BridgePlistTest(unittest.TestCase):
    def test_bridge_plist_runs_app_executable(self):
        from agent_run.install import BRIDGE_LABEL, bridge_plist
        data = plistlib.loads(bridge_plist("/A/AgentRunBridge.app", "/h/.agent-run"))
        self.assertEqual(data["Label"], BRIDGE_LABEL)
        self.assertEqual(data["ProgramArguments"], ["/A/AgentRunBridge.app/Contents/MacOS/AgentRunBridge"])
        self.assertEqual(data["EnvironmentVariables"], {"AGENT_RUN_HOME": "/h/.agent-run"})
        self.assertTrue(data["KeepAlive"])


class LaunchdTest(unittest.TestCase):
    def test_plist_runs_daemon_and_keeps_alive(self):
        data = plistlib.loads(launchd_plist("/usr/bin/python3", "/h/.agent-run/bin/agent-run", "/h/.agent-run"))
        self.assertEqual(data["Label"], "local.agent-run")
        self.assertEqual(data["ProgramArguments"], ["/usr/bin/python3", "/h/.agent-run/bin/agent-run", "run"])
        self.assertTrue(data["RunAtLoad"])
        self.assertTrue(data["KeepAlive"])
        self.assertEqual(data["StandardErrorPath"], "/h/.agent-run/log/agent-run.log")


if __name__ == "__main__":
    unittest.main()
