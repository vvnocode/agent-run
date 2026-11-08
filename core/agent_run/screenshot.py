"""FAP_SCREENSHOT_V1 串口截屏的电脑端（与固件 ar_capture.c 配套）。

协议：电脑发一行 "FAP_SCREENSHOT_V1"，设备回一行
"FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>"，随后紧跟恰好 <字节数> 字节的小端 RGB565 像素。
设备的开机日志等其他输出可能出现在头部之前，按行跳过。
PNG 编码只用标准库（zlib + struct），不依赖第三方图像库。
"""
import os
import select
import struct
import time
import zlib

HEADER = b"FAP_SCREENSHOT_V1"

# 社区上架工具只给 15 秒，固件实测一帧约 2 秒
DEFAULT_TIMEOUT = 15.0


class ScreenshotError(Exception):
    """截屏失败：回包格式不对，或数据没有收全。"""


def parse_capture(chunks):
    """从字节块序列中解析一次截屏回包，返回 (宽, 高, RGB565LE 原始字节)。

    chunks 是可迭代的 bytes 块（读完或超时即结束）；头部之前的行被忽略。
    """
    buf = b""
    header = None
    for chunk in chunks:
        buf += chunk
        if header is None:
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                parts = line.strip().split()
                if parts[:1] == [HEADER] and len(parts) == 5:
                    header = parts
                    break
            if header is None:
                continue
            try:
                width, height, length = int(header[1]), int(header[2]), int(header[4])
            except ValueError:
                raise ScreenshotError("截屏头部无法解析：%r" % b" ".join(header))
            if header[3] != b"RGB565LE" or length != width * height * 2:
                raise ScreenshotError("不支持的截屏格式：%r" % b" ".join(header))
        if header is not None and len(buf) >= length:
            return width, height, buf[:length]
    if header is None:
        raise ScreenshotError("没有收到截屏头部，设备固件可能不支持截屏")
    raise ScreenshotError("截屏数据不完整：收到 %d / %d 字节" % (len(buf), length))


def rgb565le_to_rgb(raw):
    """小端 RGB565 → 8 位 RGB，低位按高位复制补齐，保证白色为 255。"""
    out = bytearray(len(raw) // 2 * 3)
    for i in range(len(raw) // 2):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out[3 * i] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def scale_rgb(rgb, width, height, factor):
    """最近邻放大 factor 倍（像素画放大后仍保持清晰），返回 (数据, 新宽, 新高)。"""
    if factor <= 1:
        return rgb, width, height
    rows = []
    for y in range(height):
        row = rgb[y * width * 3:(y + 1) * width * 3]
        wide = b"".join(row[x * 3:x * 3 + 3] * factor for x in range(width))
        rows.append(wide * factor)
    return b"".join(rows), width * factor, height * factor


def encode_png(rgb, width, height):
    """把 8 位 RGB 编码成 PNG（每行滤波类型 0）。"""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _read_chunks(fd, timeout):
    """从非阻塞文件描述符持续读取，直到超时。"""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        ready, _, _ = select.select([fd], [], [], min(remaining, 0.2))
        if not ready:
            continue
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not data:
            return
        yield data


def capture(link, timeout=DEFAULT_TIMEOUT):
    """经已打开的 SerialLink 请求一次截屏，返回 (宽, 高, RGB 字节)。"""
    if not link.write_line(HEADER.decode() + "\n"):
        raise ScreenshotError("发送截屏命令失败")
    width, height, raw = parse_capture(_read_chunks(link.fileno(), timeout))
    return width, height, rgb565le_to_rgb(raw)
