"""dsh 会话文件扫描测试（设计方案 §5.3）。"""
import os
import tempfile
import time
import unittest

from agent_run.dsh import count_working


def make_session(root, project, name, age_seconds, now, filename="session.v4.jsonl.zstd"):
    """在临时目录里造一个 dsh 会话目录，并把记录文件的修改时间设为 now - age。"""
    path = os.path.join(root, project, name)
    os.makedirs(path)
    if filename:
        file_path = os.path.join(path, filename)
        with open(file_path, "wb") as f:
            f.write(b"x")
        os.utime(file_path, (now - age_seconds, now - age_seconds))
    return path


class CountWorkingTest(unittest.TestCase):
    def test_counts_recently_written_sessions_only(self):
        root = tempfile.mkdtemp()
        now = time.time()
        make_session(root, "--Users-me-a--", "session-1", 5, now)
        make_session(root, "--Users-me-a--", "session-2", 60, now)
        make_session(root, "--Users-me-b--", "session-3", 1, now)
        make_session(root, "--Users-me-b--", "session-4", 1, now, filename=None)
        self.assertEqual(count_working(root, now, window=20), 2)

    def test_other_record_versions_are_accepted(self):
        root = tempfile.mkdtemp()
        now = time.time()
        make_session(root, "--p--", "session-1", 2, now, filename="session.v5.jsonl")
        self.assertEqual(count_working(root, now, window=20), 1)

    def test_non_session_directories_are_ignored(self):
        root = tempfile.mkdtemp()
        now = time.time()
        make_session(root, "--p--", "3f2a-legacy", 1, now)
        self.assertEqual(count_working(root, now, window=20), 0)

    def test_missing_root_counts_zero(self):
        self.assertEqual(count_working("/nonexistent/dsh/sessions", time.time(), window=20), 0)


if __name__ == "__main__":
    unittest.main()
