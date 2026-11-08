"""ioreg 输出解析测试。样例按 `ioreg -r -c IOUSBHostDevice -l -w0` 的结构手工构造。"""
import unittest

from agent_run.ports import parse_ioreg

SAMPLE = """\
+-o Some Keyboard@01100000  <class IOUSBHostDevice, id 0x1000011aa>
    {
      "idVendor" = 1452
      "USB Product Name" = "Keyboard"
    }
  +-o IOUSBHostInterface@0  <class IOUSBHostInterface>
      {
        "idVendor" = 1452
      }
    +-o IOSerialBSDClient  <class IOSerialBSDClient>
        {
          "IOCalloutDevice" = "/dev/cu.usbmodemKEYBOARD"
        }
+-o USB JTAG_serial debug unit@00100000  <class IOUSBHostDevice, id 0x1000011bb>
    {
      "idProduct" = 4097
      "idVendor" = 12346
      "USB Product Name" = "USB JTAG_serial debug unit"
    }
  +-o IOUSBHostInterface@0  <class IOUSBHostInterface>
    +-o AppleUSBACMData  <class AppleUSBACMData>
      +-o IOSerialBSDClient  <class IOSerialBSDClient>
          {
            "IOCalloutDevice" = "/dev/cu.usbmodem101"
            "IODialinDevice" = "/dev/tty.usbmodem101"
          }
"""


class ParseIoregTest(unittest.TestCase):
    def test_finds_only_espressif_callout_device(self):
        self.assertEqual(parse_ioreg(SAMPLE), ["/dev/cu.usbmodem101"])

    def test_empty_output(self):
        self.assertEqual(parse_ioreg(""), [])


if __name__ == "__main__":
    unittest.main()
