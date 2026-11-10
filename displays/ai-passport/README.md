# AI Passport 展示端

[FoloToy AI Passport](https://ai-passport.folotoy.cn/)（ESP32-C3、240×320 屏幕、三个按键）上的 Agent Run! 固件。工作中的会话越多，宠物跑得越快；有会话等你处理时，宠物停下招手并响一声。经 USB 或蓝牙接收 [展示端协议](../../docs/protocol.md)。

## 来源与许可

- 板级支持 `components/bsp/`、`main/key_guard.h` 以及根目录的 `CMakeLists.txt`、`partitions.csv`、`sdkconfig.defaults`、`dependencies.lock`、`assets/battery/`，取自 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 提交 `488beda`（2026-11-02），遵循其 MIT 许可（见本目录 `LICENSE`）。其中 `sdkconfig.defaults` 与 `CMakeLists.txt` 已按本项目修改。
- `main/` 下的其余代码是本项目代码。
- 中文字体子集 `main/agent_run/ar_font_cjk14.c` 由 Noto Sans SC Regular（SIL Open Font License 1.1）生成，见下文。

上游 BSP 有更新时，用官方仓库的 `components/bsp/` 与本目录逐文件比对，再手工合入。

## 构建

需要 ESP-IDF 5.5.3。

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
idf.py -C displays/ai-passport build
```

在仓库根目录运行 `scripts/check.sh --firmware`，会先跑宿主测试，再构建并生成合并镜像 `build/agent-run-ai-passport-full.bin`。

## 烧录

烧录前先停止 agent-run 采集程序，否则它每 3 秒探测一次串口，会和烧录工具抢端口：

```bash
launchctl bootout gui/$(id -u)/local.agent-run
```

- **第一次烧录（替换出厂固件）：** 把合并镜像从 `0x0` 写入。这会清空 NVS，包括亮度、音量设置和蓝牙配对。

  ```bash
  esptool.py --chip esp32c3 -p /dev/cu.usbmodem101 -b 460800 write_flash 0x0 displays/ai-passport/build/agent-run-ai-passport-full.bin
  ```

- **之后更新：** 用 `idf.py -C displays/ai-passport -p /dev/cu.usbmodem101 flash`，只写引导、分区表和应用，NVS 保留，不需要重新配对。
- **蓝牙配对失效：** 如果清空过 NVS，Mac 会提示“Peer removed pairing information”，`agent-run status` 里也会给出提示。到 系统设置 → 蓝牙 中忽略 `AgentRun-XXXX`，再在设备上按 OK 重新确认配对。
- 烧录结束后设备偶尔不响应握手，按一下电源键重启即可。

完成后重新启动采集程序：

```bash
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.agent-run.plist
```

## 按键

| 操作 | 作用 |
|---|---|
| 确定 单击 | 打开设置菜单（音量、亮度、电池熄屏、接电源熄屏、勿扰、夜间勿扰），默认选中音量 |
| 确定 长按 | 立即熄屏（菜单打开、配对确认时不起作用） |
| 菜单里 上 / 下 | 切换选中项；进入调节后加减档位并保存，勿扰类开关为上键开启、下键关闭 |
| 菜单里 确定 单击 / 双击 | 进入选中项 / 返回上一级 |
| 菜单无操作 | 菜单 10 秒、调节界面 3 秒后回首页 |
| 上 按住 5 秒 | 清除蓝牙配对，之后可以配对另一台电脑 |
| 配对确认时 确定 / 下 | 接受 / 拒绝 |
| 任意键 | 背光关闭时只唤醒屏幕；深睡时唤醒设备 |

## 截屏

固件实现了 AI Passport 社区使用的 `FAP_SCREENSHOT_V1` 串口截屏协议（见 [展示端协议](../../docs/protocol.md) §4）。用法：先停止采集程序，再执行 `agent-run screenshot -o screen.png --scale 2`。

整屏缓冲需要 150 KB，设备拿不出这么大的内存。所以截屏时临时申请一块 64 行的条带缓冲（30 KB），分 5 段整屏刷新并逐段发送，一帧约 1 秒。仓库 README 里的截图由 `scripts/readme_screens.py` 用这个协议生成。

## 中文字体

界面上的中文文案全部集中在 `main/agent_run/ar_text.h`。修改文案后重新生成字体：

```bash
displays/ai-passport/tools/gen_agent_run_font.sh
```

- 生成脚本会下载 noto-cjk 仓库 `Sans2.004` 标签下的 `NotoSansSC-Regular.otf`，并校验 SHA-256；
- 用 `lv_font_conv` 1.5.3 生成 14 px、4 bpp 的子集；
- `tests/test_ar_font_coverage.py` 会检查字体是否覆盖全部文案。

数字与英文由 `ar_raster.c` 内置的 5×7 像素字绘制，不依赖字体文件。

## 资源占用（实测）

2026-11-10 固件 0.5.0 实测，开启蓝牙并按 `sdkconfig.defaults` 裁剪：

- **堆：** 开机空闲约 36 KB，运行中约 40 KB，最大连续块约 27 KB。截屏时临时申请的 40 行条带缓冲约 19 KB，放得下。
- **LVGL 内存池（32 KB）：** 全部画面开机时一次创建后空闲约 3.4 KB；把设置菜单各页都打开过一遍后，稳定在约 3.3 KB，最大空闲块约 2.4 KB。再加界面元素前先看这个余量。固件在开机、每次关闭设置菜单和每 10 分钟的内存记录里打印内存池用量与峰值。
- **静态缓冲：** 界面使用约 70 KB。
