"""查找 AI Passport 的 USB 串口（设计方案 §5.6 setup）。

AI Passport 的 USB 口是 ESP32-C3 自带的 USB Serial/JTAG，USB 厂商号为 Espressif 的 0x303A。
用 ioreg 列出 USB 设备子树，从中找到该厂商设备下的 /dev/cu.* 端口。
"""
import re
import subprocess

ESPRESSIF_VID = 0x303A

_VENDOR = re.compile(r'"idVendor" = (\d+)')
_CALLOUT = re.compile(r'"IOCalloutDevice" = "([^"]+)"')


def parse_ioreg(text, vendor_id=ESPRESSIF_VID):
    """从 `ioreg -r -c IOUSBHostDevice -l -w0` 的输出中提取指定厂商设备的端口。

    -r 模式下每个 USB 设备的子树以第 0 列的 "+-o" 开头；设备自身属性里的 idVendor
    出现在子节点之前，子节点中的 IOSerialBSDClient 带 IOCalloutDevice。
    """
    ports = []
    vendor = None
    for line in text.splitlines():
        if line.startswith("+-o"):
            vendor = None  # 新的设备子树
            continue
        if vendor is None:
            match = _VENDOR.search(line)
            if match:
                vendor = int(match.group(1))
                continue
        match = _CALLOUT.search(line)
        if match and vendor == vendor_id and match.group(1) not in ports:
            ports.append(match.group(1))
    return ports


def find_espressif_ports():
    """返回当前接入的 Espressif USB 串口列表；ioreg 不可用时返回空列表。"""
    try:
        out = subprocess.run(
            ["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return parse_ioreg(out)
