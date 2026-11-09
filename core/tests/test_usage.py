"""订阅用量采集测试（专用页面方案 §3、§5.3）。

Codex 用夹具会话文件，Claude Code 用注入的凭据读取函数与请求函数，不访问钥匙串和网络。
"""
import json
import logging
import os
import tempfile
import unittest

from agent_run.usage import (
    NO_LIMIT,
    ClaudePoller,
    UsageCollector,
    Window,
    classify_codex_limits,
    parse_claude_credentials,
    parse_claude_usage,
    read_codex_usage,
)

NOW = 1794192000.0
TOKEN = "sk-ant-oat01-NEVER-LEAK-THIS"


def token_count(ts, primary=None, secondary=None, limit_id="codex", rate_limits=True):
    """构造一行与 Codex 会话文件格式一致的 token_count 事件。"""
    payload = {"type": "token_count", "info": {"total_token_usage": {"input_tokens": 1}}}
    if rate_limits:
        payload["rate_limits"] = {
            "limit_id": limit_id,
            "primary": primary,
            "secondary": secondary,
            "plan_type": "prolite",
        }
    else:
        payload["rate_limits"] = None
    return json.dumps({"timestamp": ts, "type": "event_msg", "payload": payload}) + "\n"


def window(used, minutes, resets_at):
    return {"used_percent": used, "window_minutes": minutes, "resets_at": resets_at}


WEEK = window(5.0, 10080, 1794632374)
FIVE = window(40.0, 300, 1794200000)


class ClassifyCodexTest(unittest.TestCase):
    def test_week_only_account_has_no_five_hour_limit(self):
        self.assertEqual(classify_codex_limits({"primary": WEEK, "secondary": None}),
                         (NO_LIMIT, Window(5.0, 1794632374)))

    def test_windows_are_classified_by_length_not_position(self):
        expected = (Window(40.0, 1794200000), Window(5.0, 1794632374))
        self.assertEqual(classify_codex_limits({"primary": FIVE, "secondary": WEEK}), expected)
        self.assertEqual(classify_codex_limits({"primary": WEEK, "secondary": FIVE}), expected)

    def test_unknown_window_length_and_bad_values_are_ignored(self):
        odd = window(9.0, 1440, 1794200000)
        bad = {"used_percent": "x", "window_minutes": 300, "resets_at": 1}
        self.assertEqual(classify_codex_limits({"primary": odd, "secondary": bad}), (NO_LIMIT, NO_LIMIT))

    def test_missing_reset_time_is_unknown(self):
        self.assertEqual(classify_codex_limits({"primary": window(7.0, 300, None)})[0], Window(7.0, None))


class ReadCodexTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="codex-")

    def write(self, name, text, age=0.0):
        day = os.path.join(self.root, "2026", "11", "08")
        os.makedirs(day, exist_ok=True)
        path = os.path.join(day, name)
        with open(path, "w") as f:
            f.write(text)
        os.utime(path, (NOW - age, NOW - age))
        return path

    def test_latest_event_wins_and_truncated_line_is_skipped(self):
        self.write("rollout-a.jsonl",
                   token_count("2026-11-09T01:00:00.000Z", window(3.0, 10080, 1794632374))
                   + token_count("2026-11-09T01:04:23.679Z", WEEK)
                   + '{"timestamp": "2026-11-09T01:05:00.000Z", "type": "event_msg", "payload": {"type": "token_co')
        usage = read_codex_usage(self.root, NOW)
        self.assertEqual((usage.five_hour, usage.seven_day), (NO_LIMIT, Window(5.0, 1794632374)))
        self.assertAlmostEqual(usage.updated, 1794186263.679, places=2)
        self.assertIsNone(usage.error)

    def test_events_without_rate_limits_or_other_limit_ids_are_skipped(self):
        self.write("rollout-a.jsonl",
                   token_count("2026-11-09T01:00:00.000Z", WEEK)
                   + token_count("2026-11-09T01:01:00.000Z", rate_limits=False)
                   + token_count("2026-11-09T01:02:00.000Z", window(99.0, 10080, 1), limit_id="other"))
        self.assertEqual(read_codex_usage(self.root, NOW).seven_day, Window(5.0, 1794632374))

    def test_latest_event_across_files(self):
        self.write("rollout-old.jsonl", token_count("2026-11-09T02:00:00.000Z", window(8.0, 10080, 5)), age=60)
        self.write("rollout-new.jsonl", token_count("2026-11-09T01:00:00.000Z", window(6.0, 10080, 5)))
        # 修改时间更新的文件里事件反而更早：以事件时间为准
        self.assertEqual(read_codex_usage(self.root, NOW).seven_day, Window(8.0, 5))

    def test_files_older_than_a_week_are_ignored(self):
        self.write("rollout-a.jsonl", token_count("2026-11-01T01:00:00.000Z", WEEK), age=8 * 86400)
        self.assertIsNone(read_codex_usage(self.root, NOW))

    def test_only_the_tail_of_large_files_is_read(self):
        self.write("rollout-a.jsonl", token_count("2026-11-09T01:00:00.000Z", WEEK) + ("x" * 4000 + "\n"))
        self.assertIsNone(read_codex_usage(self.root, NOW, tail_bytes=1024))
        self.assertIsNotNone(read_codex_usage(self.root, NOW, tail_bytes=8192))

    def test_missing_root_and_no_events(self):
        self.assertIsNone(read_codex_usage(os.path.join(self.root, "nope"), NOW))
        self.write("rollout-a.jsonl", '{"type": "session_meta"}\n')
        self.assertIsNone(read_codex_usage(self.root, NOW))


class ClaudeParseTest(unittest.TestCase):
    def creds(self, token=TOKEN, expires_ms=(NOW + 3600) * 1000):
        return json.dumps({"claudeAiOauth": {"accessToken": token, "expiresAt": expires_ms}})

    def test_valid_token(self):
        self.assertEqual(parse_claude_credentials(self.creds(), NOW), (TOKEN, None))

    def test_expired_empty_or_broken_credentials(self):
        token, error = parse_claude_credentials(self.creds(expires_ms=(NOW - 1) * 1000), NOW)
        self.assertIsNone(token)
        self.assertIn("过期", error)
        for text in (self.creds(token="", expires_ms=0), "{}", "not json", ""):
            token, error = parse_claude_credentials(text, NOW)
            self.assertIsNone(token)
            self.assertIn("未登录", error)

    def test_usage_response(self):
        body = {
            "five_hour": {"utilization": 23.0, "resets_at": "2026-11-09T05:00:00.123456+00:00"},
            "seven_day": {"utilization": 41.5, "resets_at": "2026-11-12T00:00:00Z"},
            "seven_day_opus": None,
        }
        self.assertEqual(parse_claude_usage(body), (Window(23.0, 1794200400.123456), Window(41.5, 1794441600.0)))

    def test_usage_response_missing_or_empty_windows(self):
        body = {"five_hour": None, "seven_day": {"utilization": 3, "resets_at": None}}
        self.assertEqual(parse_claude_usage(body), (NO_LIMIT, Window(3.0, None)))
        self.assertEqual(parse_claude_usage({"seven_day": {"utilization": None}}), (NO_LIMIT, None))

    def test_usage_response_must_be_object(self):
        with self.assertRaises(ValueError):
            parse_claude_usage([])


class FakeClaude:
    """可编排的凭据与请求结果；记录发出的请求次数与使用的令牌。"""

    def __init__(self):
        self.token = TOKEN
        self.responses = []
        self.requests = []

    def read_credentials(self):
        return json.dumps({"claudeAiOauth": {"accessToken": self.token, "expiresAt": (NOW + 86400) * 1000}})

    def fetch(self, token):
        self.requests.append(token)
        result = self.responses.pop(0)
        if isinstance(result, Exception):
            raise result
        return result


OK_BODY = json.dumps({"five_hour": {"utilization": 23.0, "resets_at": None},
                      "seven_day": {"utilization": 41.0, "resets_at": None}}).encode()


class ClaudePollerTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeClaude()
        self.poller = ClaudePoller(read_credentials=self.fake.read_credentials, fetch=self.fake.fetch)

    def test_success_then_every_five_minutes(self):
        self.fake.responses = [(200, OK_BODY)]
        self.assertTrue(self.poller.due(NOW))
        self.poller.poll(NOW)
        usage = self.poller.result(NOW)
        self.assertEqual((usage.five_hour, usage.seven_day), (Window(23.0, None), Window(41.0, None)))
        self.assertEqual(usage.updated, NOW)
        self.assertFalse(self.poller.due(NOW + 299))
        self.assertTrue(self.poller.due(NOW + 300))

    def test_refresh_after_stop_keeps_sixty_second_gap(self):
        self.fake.responses = [(200, OK_BODY)]
        self.poller.poll(NOW)
        self.poller.request_refresh(NOW + 10)
        self.assertFalse(self.poller.due(NOW + 59))
        self.assertTrue(self.poller.due(NOW + 60))

    def test_expired_token_sends_no_request(self):
        self.poller = ClaudePoller(
            read_credentials=lambda: json.dumps({"claudeAiOauth": {"accessToken": TOKEN, "expiresAt": 0}}),
            fetch=self.fake.fetch)
        self.poller.poll(NOW)
        self.assertEqual(self.fake.requests, [])
        self.assertIn("过期", self.poller.result(NOW).error)
        self.assertTrue(self.poller.due(NOW + 300))

    def test_unauthorized_waits_for_a_new_token(self):
        self.fake.responses = [(401, b""), (200, OK_BODY)]
        self.poller.poll(NOW)
        self.assertIn("401", self.poller.result(NOW).error)
        self.poller.poll(NOW + 300)              # 令牌没变：不再请求
        self.assertEqual(len(self.fake.requests), 1)
        self.fake.token = TOKEN + "-new"
        self.poller.poll(NOW + 600)              # 令牌变了：重新请求
        self.assertEqual(len(self.fake.requests), 2)
        self.assertIsNone(self.poller.result(NOW + 600).error)

    def test_rate_limit_and_network_errors_back_off(self):
        self.fake.responses = [(429, b""), OSError("boom"), OSError("boom"), OSError("boom"), (200, OK_BODY)]
        t = NOW
        gaps = []
        for _ in range(4):
            self.poller.poll(t)
            gap = 60
            while not self.poller.due(t + gap):
                gap += 60
            gaps.append(gap)
            t += gap
        self.assertEqual(gaps, [300, 600, 1200, 1200])
        # 退避期间 Stop 不提前请求
        self.poller.request_refresh(t - 1100)
        self.assertFalse(self.poller.due(t - 1100))
        self.poller.poll(t)
        self.assertTrue(self.poller.due(t + 300))
        self.assertIsNone(self.poller.result(t).error)

    def test_failure_keeps_recent_data_then_expires_it(self):
        self.fake.responses = [(200, OK_BODY), (429, b"")]
        self.poller.poll(NOW)
        self.poller.poll(NOW + 300)
        self.assertEqual(self.poller.result(NOW + 300).seven_day, Window(41.0, None))
        self.assertIsNone(self.poller.result(NOW + 1801).seven_day)

    def test_bad_body_is_reported(self):
        self.fake.responses = [(200, b"<html>")]
        self.poller.poll(NOW)
        self.assertIn("解析", self.poller.result(NOW).error)

    def test_token_never_reaches_results_or_logs(self):
        self.fake.responses = [OSError("connect to %s failed" % TOKEN), (401, TOKEN.encode()), (200, OK_BODY)]
        with self.assertLogs("agent-run", level="DEBUG") as logs:
            logging.getLogger("agent-run").debug("start")
            for i in range(3):
                self.poller.poll(NOW + i * 1200)
                if i == 1:
                    self.fake.token = TOKEN + "2"
                text = json.dumps(self.poller.result(NOW + i * 1200))
                self.assertNotIn(TOKEN, text)
        self.assertNotIn(TOKEN, "\n".join(logs.output))


class FakePoller:
    """记录 Stop 刷新请求的 Claude 轮询器替身。"""

    def __init__(self):
        self.refreshes = []
        self.polls = []

    def due(self, now):
        return True

    def poll(self, now):
        self.polls.append(now)

    def request_refresh(self, now):
        self.refreshes.append(now)

    def result(self, now):
        return None


class CollectorTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="codex-")
        day = os.path.join(self.root, "2026", "11", "09")
        os.makedirs(day)
        self.path = os.path.join(day, "rollout-a.jsonl")
        self.claude = FakePoller()
        self.collector = UsageCollector(codex_root=self.root, claude=self.claude, threaded=False)

    def write(self, used):
        with open(self.path, "w") as f:
            f.write(token_count("2026-11-09T01:00:00.000Z", window(used, 10080, 1794632374)))
        os.utime(self.path, (NOW, NOW))

    def test_codex_is_scanned_every_thirty_seconds_or_after_stop(self):
        self.write(5.0)
        self.collector.step(NOW)
        self.assertEqual(self.collector.get("codex").seven_day.used, 5.0)
        self.write(6.0)
        self.collector.step(NOW + 10)
        self.assertEqual(self.collector.get("codex").seven_day.used, 5.0)
        self.collector.on_stop("codex", NOW + 10)
        self.collector.step(NOW + 10)
        self.assertEqual(self.collector.get("codex").seven_day.used, 6.0)

    def test_claude_stop_requests_refresh_and_unthreaded_step_polls(self):
        self.collector.on_stop("claude", NOW)
        self.collector.step(NOW)
        self.assertEqual(self.claude.refreshes, [NOW])
        self.assertEqual(self.claude.polls, [NOW])

    def test_unknown_tool_has_no_data(self):
        usage = self.collector.get("dsh")
        self.assertEqual((usage.five_hour, usage.seven_day, usage.updated), (None, None, None))


if __name__ == "__main__":
    unittest.main()
