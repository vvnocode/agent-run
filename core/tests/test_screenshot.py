"""FAP_SCREENSHOT_V1 截屏的电脑端测试：回包解析、RGB565 转换、PNG 编码与放大。"""
import struct
import unittest
import zlib

from agent_run.screenshot import ScreenshotError, encode_png, parse_capture, rgb565le_to_rgb, scale_rgb


class ParseCaptureTest(unittest.TestCase):
    def test_skips_log_lines_and_reads_exact_payload(self):
        payload = bytes(range(16))   # 2×4 像素，16 字节
        chunks = [b"I (1) boot\r\nFAP_SCREEN", b"SHOT_V1 2 4 RGB565LE 16\n" + payload[:5], payload[5:] + b"tail"]
        width, height, data = parse_capture(iter(chunks))
        self.assertEqual((width, height, data), (2, 4, payload))

    def test_short_payload_raises(self):
        with self.assertRaises(ScreenshotError):
            parse_capture(iter([b"FAP_SCREENSHOT_V1 2 2 RGB565LE 8\n", b"\x00\x01"]))

    def test_inconsistent_header_raises(self):
        with self.assertRaises(ScreenshotError):
            parse_capture(iter([b"FAP_SCREENSHOT_V1 2 2 RGB565LE 9\n" + b"\x00" * 9]))


class ImageTest(unittest.TestCase):
    def test_rgb565le_conversion(self):
        # 小端：红 0xF800、绿 0x07E0、蓝 0x001F、白 0xFFFF
        raw = struct.pack("<4H", 0xF800, 0x07E0, 0x001F, 0xFFFF)
        self.assertEqual(rgb565le_to_rgb(raw), bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]))

    def test_scale_nearest_neighbor(self):
        rgb = bytes([1, 1, 1, 2, 2, 2])   # 2×1
        scaled, w, h = scale_rgb(rgb, 2, 1, 2)
        self.assertEqual((w, h), (4, 2))
        self.assertEqual(scaled, bytes([1, 1, 1] * 2 + [2, 2, 2] * 2) * 2)

    def test_png_structure(self):
        rgb = bytes([10, 20, 30, 40, 50, 60])   # 2×1
        png = encode_png(rgb, 2, 1)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(png[12:16], b"IHDR")
        self.assertEqual(struct.unpack(">II", png[16:24]), (2, 1))
        idat_len = struct.unpack(">I", png[33:37])[0]
        self.assertEqual(png[37:41], b"IDAT")
        self.assertEqual(zlib.decompress(png[41:41 + idat_len]), b"\x00" + rgb)
        self.assertTrue(png.endswith(b"IEND\xaeB`\x82"))


if __name__ == "__main__":
    unittest.main()
