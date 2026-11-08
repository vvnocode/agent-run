"""dsh 会话文件扫描（设计方案 §5.3）。

dsh 没有对外的 hook 事件，只能按会话记录文件的修改时间估算“工作中”：
~/.dsh/sessions/<项目路径转写>/session-<uuid>/session.v4.jsonl.zstd
这是启发式做法，拿不到“等你处理”信号。
"""
import os


def _newest_record_mtime(session_dir):
    """返回会话目录下最新一份记录文件（session*.jsonl*）的修改时间，没有则返回 None。"""
    newest = None
    try:
        entries = os.scandir(session_dir)
    except OSError:
        return None
    with entries:
        for entry in entries:
            # 文件名带格式版本号（如 v4），按前缀与扩展名匹配，兼容后续版本
            if not (entry.name.startswith("session") and ".jsonl" in entry.name):
                continue
            try:
                if not entry.is_file():
                    continue
                mtime = entry.stat().st_mtime
            except OSError:
                continue
            if newest is None or mtime > newest:
                newest = mtime
    return newest


def count_working(root, now, window=20):
    """统计 window 秒内有写入的 dsh 会话数；目录不存在时返回 0。"""
    count = 0
    try:
        projects = os.scandir(root)
    except OSError:
        return 0
    with projects:
        for project in projects:
            try:
                if not project.is_dir():
                    continue
                sessions = os.scandir(project.path)
            except OSError:
                continue
            with sessions:
                for session in sessions:
                    if not session.name.startswith("session-"):
                        continue
                    mtime = _newest_record_mtime(session.path)
                    if mtime is not None and now - mtime <= window:
                        count += 1
    return count
