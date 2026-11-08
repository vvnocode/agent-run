"""hook 入口 ar_hook.py 测试（设计方案 §5.2 的契约）。

ar_hook.py 是独立脚本，这里既按模块导入测试字段裁剪，也用子进程测试退出码、输出与耗时。
"""
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HOOK_PATH = os.path.join(os.path.dirname(__file__), "..", "bin", "ar_hook.py")


def load_hook_module():
    """按文件路径加载 ar_hook.py，避免依赖安装位置。"""
    spec = importlib.util.spec_from_file_location("ar_hook", HOOK_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ar_hook = load_hook_module()


class ReducePayloadTest(unittest.TestCase):
    def test_only_whitelisted_fields_survive(self):
        payload = {
            "session_id": "s1",
            "hook_event_name": "PreToolUse",
            "tool_name": "Bash",
            "tool_input": {"command": "cat ~/secret"},
            "cwd": "/Users/me/private-project",
            "transcript_path": "/Users/me/.claude/projects/x.jsonl",
            "prompt": "do something private",
        }
        msg = ar_hook.reduce_payload(payload, "claude", 123.5)
        self.assertEqual(msg, {"tool": "claude", "event": "PreToolUse", "sid": "s1", "ts": 123.5, "tool_name": "Bash"})

    def test_notification_type_and_subagent_flag(self):
        payload = {
            "session_id": "s1",
            "hook_event_name": "Notification",
            "notification_type": "permission_prompt",
            "agent_id": "a-1",
            "message": "Claude needs your permission",
        }
        msg = ar_hook.reduce_payload(payload, "claude", 1.0)
        self.assertEqual(msg["ntype"], "permission_prompt")
        self.assertIs(msg["agent"], True)
        self.assertNotIn("message", msg)

    def test_missing_session_or_event_returns_none(self):
        self.assertIsNone(ar_hook.reduce_payload({"hook_event_name": "Stop"}, "claude", 1.0))
        self.assertIsNone(ar_hook.reduce_payload({"session_id": "s1"}, "codex", 1.0))
        self.assertIsNone(ar_hook.reduce_payload(["not", "a", "dict"], "codex", 1.0))

    def test_overlong_values_are_rejected(self):
        payload = {"session_id": "x" * 500, "hook_event_name": "Stop"}
        self.assertIsNone(ar_hook.reduce_payload(payload, "claude", 1.0))

    def test_background_task_count_is_forwarded_without_details(self):
        # Stop 载荷里的后台任务只转发数量；命令、描述与定时任务一律不转发
        payload = {
            "session_id": "s1",
            "hook_event_name": "Stop",
            "last_assistant_message": "回放完成后告诉你结果",
            "background_tasks": [
                {"id": "b1", "type": "shell", "status": "running", "description": "回放",
                 "command": "./replay.sh ~/private/video.mp4"},
                {"id": "a1", "type": "subagent", "status": "running", "description": "查资料",
                 "agent_type": "Explore"},
            ],
            "session_crons": [{"id": "c1", "schedule": "*/5 * * * *", "recurring": True, "prompt": "check"}],
        }
        msg = ar_hook.reduce_payload(payload, "claude", 1.0)
        self.assertEqual(msg, {"tool": "claude", "event": "Stop", "sid": "s1", "ts": 1.0, "bg": 2})

    def test_background_tasks_absent_or_malformed(self):
        base = {"session_id": "s1", "hook_event_name": "Stop"}
        self.assertEqual(ar_hook.reduce_payload(dict(base, background_tasks=[]), "claude", 1.0)["bg"], 0)
        self.assertNotIn("bg", ar_hook.reduce_payload(dict(base, background_tasks="x"), "claude", 1.0))
        self.assertNotIn("bg", ar_hook.reduce_payload(base, "codex", 1.0))


class HookProcessTest(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="ar-")
        os.makedirs(os.path.join(self.home, "run"))
        self.env = dict(os.environ, AGENT_RUN_HOME=self.home)

    def run_hook(self, tool, stdin_bytes):
        start = time.monotonic()
        proc = subprocess.run(
            [sys.executable, HOOK_PATH, tool],
            input=stdin_bytes,
            capture_output=True,
            env=self.env,
            timeout=5,
        )
        return proc, time.monotonic() - start

    def test_no_daemon_exits_quietly_and_fast(self):
        payload = json.dumps({"session_id": "s1", "hook_event_name": "Stop"}).encode()
        proc, elapsed = self.run_hook("claude", payload)
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (0, b"", b""))
        self.assertLess(elapsed, 0.2)

    def test_bad_json_exits_quietly(self):
        proc, _ = self.run_hook("codex", b"{not json")
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (0, b"", b""))

    def test_forwards_reduced_message_to_daemon_socket(self):
        path = os.path.join(self.home, "run", "ar.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        server.bind(path)
        server.settimeout(2)
        try:
            payload = json.dumps({"session_id": "s1", "hook_event_name": "UserPromptSubmit", "prompt": "secret"}).encode()
            proc, _ = self.run_hook("codex", payload)
            self.assertEqual(proc.returncode, 0)
            data = json.loads(server.recv(4096).decode())
        finally:
            server.close()
        self.assertEqual(set(data), {"tool", "event", "sid", "ts"})
        self.assertEqual((data["tool"], data["event"], data["sid"]), ("codex", "UserPromptSubmit", "s1"))

    def test_dsh_tool_is_accepted(self):
        path = os.path.join(self.home, "run", "ar.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        server.bind(path)
        server.settimeout(2)
        try:
            payload = json.dumps({"session_id": "d1", "hook_event_name": "PreToolUse",
                                  "tool_name": "ask_user_question", "transcript_path": ""}).encode()
            proc, _ = self.run_hook("dsh", payload)
            self.assertEqual(proc.returncode, 0)
            data = json.loads(server.recv(4096).decode())
        finally:
            server.close()
        self.assertEqual((data["tool"], data["tool_name"]), ("dsh", "ask_user_question"))

    def test_unknown_tool_sends_nothing(self):
        path = os.path.join(self.home, "run", "ar.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        server.bind(path)
        server.settimeout(0.3)
        try:
            payload = json.dumps({"session_id": "s1", "hook_event_name": "Stop"}).encode()
            proc, _ = self.run_hook("vim", payload)
            self.assertEqual(proc.returncode, 0)
            with self.assertRaises(socket.timeout):
                server.recv(4096)
        finally:
            server.close()


if __name__ == "__main__":
    unittest.main()
