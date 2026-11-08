# Agent Run!

把正在跑的 AI 编程 Agent 会话，变成看得见的状态：会话越多，宠物跑得越快；有会话在等你授权或回答，就停下来提醒你。

目前支持在 [FoloToy AI Passport](https://ai-passport.folotoy.cn/) 上显示，经 USB 或蓝牙连接电脑。电脑浮窗、手机锁屏等展示方式计划中，都按同一份 [展示端协议](docs/protocol.md) 对接。

## 工作方式

```
Claude Code ─hook─┐
Codex ───────hook─┼─> agent-run 采集程序 ──USB──────────────────────> AI Passport
dsh ───hook 插件──┘        │            └──蓝牙（AgentRunBridge.app）──> AI Passport
                           └──> ~/.agent-run/run/status.json（本机展示端）
```

- 采集程序通过各工具官方的 hook 接口拿到会话事件，汇总成“工作中”“等你处理”“今日完成”几个数字。
- 送到展示端的只有计数和工具代号，没有项目名、路径或提示词。

## 支持情况

| 工具 | 本机运行的会话 | 识别“等你处理” | 说明 |
|---|---|---|---|
| Claude Code | 桌面端、CLI、VS Code 扩展 | 权限请求、提问（`AskUserQuestion`）、计划确认 | 已在桌面端实测 |
| Codex | 桌面端、CLI | 权限请求、提问（含桌面端的异步提问） | 新装的 hook 要先在 Codex 里审查并信任 |
| dsh | 需在所用 profile 中挂载官方插件 `@deepseek-ai/dsh-hooks-claude-code` | 提问（`ask_user_question`） | 未挂载插件时只按会话文件估算“工作中” |

云端运行的会话（Claude Code on the web、Codex 云任务）不支持：它们不在本机，采集程序接收不到它们的事件。

## 目录结构

| 目录 | 内容 |
|---|---|
| `core/` | 采集程序：hook 入口、会话状态机、守护进程、命令行 `agent-run`（Python 3.9 标准库） |
| `transports/macos-ble/` | macOS 蓝牙桥接程序 `AgentRunBridge.app`（Swift） |
| `displays/ai-passport/` | AI Passport 固件（ESP-IDF），见其 [README](displays/ai-passport/README.md) |
| `docs/` | 展示端协议与设计方案 |
| `scripts/check.sh` | 一次跑完全部测试 |

## 安装（macOS）

需要：macOS 13 或更新版本，系统自带的 `/usr/bin/python3`；构建蓝牙桥接程序需要 Xcode；构建固件需要 ESP-IDF 5.5.3。

1. **安装采集程序并接入 hook：**

   ```bash
   core/bin/agent-run install-hooks      # 备份后，为 Claude Code 与 Codex 追加 hook
   core/bin/agent-run install-service    # 复制到 ~/.agent-run，并注册登录自启
   ```

   Codex 需要在 CLI 的 `/hooks` 或桌面端 Settings → Hooks 里信任新 hook。`agent-run status` 会提示尚未信任的 hook。

2. **dsh（可选）：**

   ```bash
   core/bin/agent-run install-dsh-hooks
   ```

   命令会生成 `~/.agent-run/dsh-hooks.json`，并打印需要加到 dsh profile 中的插件配置。插件版本要与 dsh 一致。DeepSeek Harness 桌面应用的 profile 只能在应用的插件管理里安装插件。

3. **刷写 AI Passport：** 见 [displays/ai-passport/README.md](displays/ai-passport/README.md)。

4. **蓝牙（可选，不插线使用）：**

   ```bash
   transports/macos-ble/build.sh                   # 构建并安装到 ~/Applications
   core/bin/agent-run install-bridge-service       # 注册登录自启
   ```

   首次运行时 macOS 会请求蓝牙权限；设备上出现“配对这台电脑？”时按 OK。之后插着 USB 走 USB，拔掉自动改走蓝牙。

## 常用命令

| 命令 | 作用 |
|---|---|
| `agent-run status [--events]` | 查看汇总状态、链路和最近收到的事件 |
| `agent-run demo` | 让设备依次演示七种画面，约 42 秒 |
| `agent-run setup` | 查找设备串口，写入配置 |
| `agent-run uninstall-hooks` / `uninstall-service` | 卸载 hook 或登录自启（运行数据保留） |

## 已知限制

- Codex 桌面端的异步提问在你回答前一直算“等你处理”。如果你在 Agent 还在干活时就回答了，Codex 没有对应事件，提醒会一直持续到你下一次发消息。
- 从 `0x0` 重新烧录合并镜像会清空设备上的蓝牙配对，需要在 Mac 的蓝牙设置里忽略设备后重新配对。
- 电池续航、蓝牙下的长时间运行还没有实测数据。

## 开发

```bash
scripts/check.sh              # Python、Swift 与固件宿主测试
scripts/check.sh --firmware   # 另外构建固件（需先激活 ESP-IDF）
```

设计方案见 `docs/specs/`。

## 许可

MIT，见 [LICENSE](LICENSE)。`displays/ai-passport/` 中来自 FoloToy/ai-passport 的部分遵循其 MIT 许可；中文字体子集来自 Noto Sans SC（SIL OFL 1.1）。
