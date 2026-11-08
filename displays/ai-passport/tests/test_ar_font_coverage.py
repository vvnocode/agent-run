#!/usr/bin/env python3
"""Agent Run! 中文字体覆盖检查。

- ar_text.h 字符串字面量里的每个非 ASCII 字符,都必须出现在 ar_font_cjk14.c 生成参数的
  --symbols 清单中(可打印 ASCII 由 --range 0x20-0x7E 覆盖);
- 界面代码中不得直接写中文字面量,统一引用 ar_text.h 的宏,否则字体可能缺字。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "main" / "agent_run"
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def literal_chars(path: Path) -> set:
    """提取文件中字符串字面量里的全部非 ASCII 字符(忽略注释)。"""
    text = path.read_text(encoding="utf-8")
    # 去掉 // 与 /* */ 注释,避免把注释里的中文算进来
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    chars = set()
    for literal in LITERAL.findall(text):
        chars.update(ch for ch in literal if ord(ch) > 0x7E)
    return chars


def font_symbols() -> set:
    header = (APP / "ar_font_cjk14.c").read_text(encoding="utf-8").split("*/", 1)[0]
    match = re.search(r"--symbols (\S+)", header)
    if not match:
        raise AssertionError("ar_font_cjk14.c 头部没有 --symbols 生成参数")
    return set(match.group(1))


class FontCoverageTest(unittest.TestCase):
    def test_font_covers_all_ui_text(self) -> None:
        missing = sorted(literal_chars(APP / "ar_text.h") - font_symbols())
        self.assertEqual(missing, [], "字体缺字,请运行 tools/gen_agent_run_font.sh 重新生成")

    def test_font_range_covers_printable_ascii(self) -> None:
        header = (APP / "ar_font_cjk14.c").read_text(encoding="utf-8").split("*/", 1)[0]
        self.assertIn("--range 0x20-0x7E", header)

    def test_ui_code_has_no_inline_chinese_literals(self) -> None:
        # 只有 ar_ui.c 把文字交给 LVGL 显示;其他文件里的中文日志不经过字体
        path = APP / "ar_ui.c"
        self.assertEqual(sorted(literal_chars(path)), [], "中文文案请放进 ar_text.h")


if __name__ == "__main__":
    unittest.main()
