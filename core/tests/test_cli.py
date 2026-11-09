"""agent-run status 中用量一栏的文字（专用页面方案 §4.2、§5.3）。"""
import unittest

from agent_run.cli import describe_window

NOW = 1794192000.0


class DescribeWindowTest(unittest.TestCase):
    def test_missing_and_no_limit(self):
        self.assertEqual(describe_window(None, NOW), "暂无数据")
        self.assertEqual(describe_window("none", NOW), "无此限额")

    def test_reset_countdown_units(self):
        self.assertEqual(describe_window({"used": 23.4, "resets_at": NOW + 30}, NOW), "23%（1 分钟后重置）")
        self.assertEqual(describe_window({"used": 5, "resets_at": NOW + 59 * 60}, NOW), "5%（59 分钟后重置）")
        self.assertEqual(describe_window({"used": 5, "resets_at": NOW + 135 * 60}, NOW), "5%（2 小时 15 分后重置）")
        self.assertEqual(describe_window({"used": 41, "resets_at": NOW + 3 * 86400 + 5 * 3600}, NOW),
                         "41%（3 天 5 小时后重置）")

    def test_passed_or_unknown_reset(self):
        self.assertEqual(describe_window({"used": 85, "resets_at": NOW - 1}, NOW), "0%（已重置）")
        self.assertEqual(describe_window({"used": 85, "resets_at": None}, NOW), "85%（重置时间未知）")


if __name__ == "__main__":
    unittest.main()
