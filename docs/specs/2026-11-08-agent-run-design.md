# Agent Run! 设计方案（v1：AI Passport + USB）

- 日期：2026-11-08
- 状态：2026-11-08 已确认并实现 v1；实现时与原方案不同的地方已直接改在对应章节
- 屏幕稿：设计阶段画在私有的 Design 画布上（六张 240×320 画板）；实现后的真机画面见仓库 README
- 目录说明：本文写于单仓库调整之前。文中的 `host/` 现为 `core/`，固件仓库 `firmware/` 现为 `displays/ai-passport/`（只保留用到的部分），见 [单仓库结构方案](2026-11-08-agent-run-repo-design.md)

## 1. 目标

把 AI Passport 做成桌面上的 Agent 状态牌：

1. Claude Code、Codex、dsh 正在执行任务的会话越多，屏幕上的宠物跑得越快。
2. 屏幕常驻显示各工具的活跃会话数、今日完成数和等你处理的数量。
3. 任一会话在等用户授权或回答时，立刻切到“等你处理”画面并响一次提示音。

v1 只覆盖“设备插在 Mac 上、通过 USB 供电和通信”这一种用法。

## 2. 非目标

以下内容不在 v1 范围，需要时另写 spec：

- 蓝牙连接、Wi-Fi 连接、离开电脑佩戴使用。
- Mac 浮窗、菜单栏图标、iPhone 锁屏和灵动岛。
- 好友联机、Agent 马拉松、宠物成长、额度显示、任务完成庆祝动画。
- 在设备上批准或拒绝权限、从设备跳转到对应会话。
- 发布到 AI Passport 玩法社区（社区发布流程要求的串口截屏协议另行评估）。
- Windows 和 Linux 主机。

## 3. 方案选择

### 3.1 采用的方案

Mac 上运行一个常驻采集进程 `agent-run`：Claude Code 和 Codex 通过 hook 把事件发给它，dsh 由它轮询会话文件；它汇总出一份很小的状态，经 USB 串口推送给设备。设备只负责显示、动画、按键和提示音，不联网、不保存任何会话信息。

### 3.2 被否的方案

| 方案 | 否决原因 |
|---|---|
| 按 `claude` / `codex` 进程数计数 | 2026-11-08 本机实测：6 个 claude 进程和 1 个 codex 进程，但近 10 分钟有写入的会话文件只有 Claude 4 个、Codex 0 个。空闲会话也会让宠物一直跑。 |
| 读各工具的会话日志推断状态（dsh 除外） | 日志格式不是公开接口，随版本变化；也拿不到“等待授权”这类信号。hook 是两边官方提供的扩展点。 |
| hook 脚本直接写串口，不要常驻进程 | 多个会话的 hook 会并发争用串口；没有地方做超时回收和 dsh 轮询；每次 hook 都开关串口，会放大打开端口导致设备复位的风险（见 §11）。 |
| 设备连 Wi-Fi，从 Mac 的局域网 HTTP 服务拉取状态 | 需要配网，只能在同一局域网使用，Wi-Fi 常开占内存、耗电。对插着 USB 的桌面场景没有收益。 |
| v1 直接做蓝牙 | 要处理配对、macOS 蓝牙权限和约 60 KB 的协议栈堆占用。先用 USB 把采集和显示跑通，蓝牙作为后续独立的传输层。 |

## 4. 总体结构

```
Claude Code ──hook──┐
                    ├──> ar_hook.py ──Unix 数据报──> agent-run 守护进程 ──USB 串口──> AI Passport
Codex ──────hook────┘                                   │  ▲
                                                        │  └── 轮询 ~/.dsh/sessions（dsh）
                                                        └───── agent-run status（终端查看）
```

仓库布局：

```
AIPassport/                外层仓库：设计文档与主机端
  docs/specs/              本文件
  host/                    agent-run 采集端（Python）
  firmware/                固件：从 FoloToy/ai-passport fork 的独立仓库，外层 .gitignore 忽略
```

固件单独成库，是为了保留上游历史，方便以后合入上游 BSP 的修复。

## 5. 主机采集端

### 5.1 运行方式

- 语言：Python 3，只用标准库。本机系统自带 Python 3.9.6，代码必须兼容 3.9（不用 `match`、不用 `X | Y` 类型写法）。
- 守护进程：作为 launchd 用户级 LaunchAgent 常驻，开机登录后自动启动。
- 文件位置：程序与运行数据都放在 `~/.agent-run/`：
  - `bin/`：程序；
  - `run/ar.sock`：事件入口；
  - `config.json`：配置；
  - `state.json`：按天保存的完成数；
  - `log/`：日志；
  - `backup/`：修改 hook 配置前的备份。

### 5.2 hook 接入

hook 命令统一为 `/usr/bin/python3 ~/.agent-run/bin/ar_hook.py <claude|codex>`。`ar_hook.py` 的契约：

- 从 stdin 读 hook 的 JSON，只取 `session_id`、`hook_event_name`、`tool_name`、`notification_type`、`agent_id` 这几个字段和 `background_tasks` 的条数，再加上工具名和时间戳，作为一个数据报（不超过 2 KB）发到 `run/ar.sock`。提示词、路径、工具参数一律不转发。
- 任何情况都以退出码 0 结束，不向 stdout 和 stderr 输出内容，200 ms 内退出。守护进程不在、JSON 异常、socket 写满时都静默丢弃。
- 两个工具都注册为异步 hook（`"async": true`），不阻塞 Agent。

**Claude Code**：写入 `~/.claude/settings.json`。

| 事件 | 条件 | 会话状态 |
|---|---|---|
| `SessionStart` | — | 登记为空闲 |
| `UserPromptSubmit` | — | 工作中 |
| `PreToolUse` | `tool_name` 为 `AskUserQuestion` | 等你处理：回答 |
| `PreToolUse` | `tool_name` 为 `ExitPlanMode` | 等你处理：授权 |
| `PreToolUse` | 其他工具 | 工作中 |
| `PostToolUse`、`PostToolUseFailure`、`PermissionDenied` | — | 工作中 |
| `PermissionRequest` | — | 等你处理：授权 |
| `Notification` | `notification_type` 为 `permission_prompt` | 等你处理：授权 |
| `Notification` | `notification_type` 为 `elicitation_dialog`、`elicitation_url_dialog`、`agent_needs_input` | 等你处理：回答 |
| `Notification` | 其他类型 | 不变 |
| `Stop` | `background_tasks` 非空 | 后台运行：计入工作中，不计完成 |
| `Stop` | 没有后台任务，之前是工作中、后台运行或等你处理 | 空闲，今日完成 +1 |
| `StopFailure` | — | 空闲，不计完成 |
| `SessionEnd` | — | 移除 |

**后台任务（2026-11-08 补充）**：Agent 用后台命令、后台子代理或 Monitor 启动长任务后，通常先回复用户，等任务结束再被唤醒继续。这时也会触发 `Stop`，按原规则会话会被算作已完成。Claude Code 的 `Stop` 载荷带 `background_tasks`，即这一轮结束时仍在进行的后台任务（已在 2.1.317 中核实），`ar_hook.py` 只转发它的条数 `bg`：

- `bg > 0` 时会话进入“后台运行”：计入跑速，不计完成，超过 2 小时没有事件才回收。后台任务期间没有任何 hook 事件，所以超时不能沿用工作中的 15 分钟。
- 唤醒后的新一轮照常迁移；最后一次不带后台任务的 `Stop` 计完成 1 次，所以这类任务从开始到结束只计 1 次完成。
- 载荷里的 `session_crons`（`ScheduleWakeup`、`CronCreate`、`/loop` 登记的定时唤醒）不计入。定时唤醒可能隔几小时才触发，也可能循环触发，算作工作中会让跑速长期失真。
- Codex、dsh 以及较早版本的 Claude Code 没有这个字段，仍按原规则处理。

带 `agent_id` 的事件来自子代理：不新建会话；父会话已登记时，状态迁移照常作用于父会话（子代理请求授权时，父会话同样进入“等你处理”）。

异步 hook 可能乱序到达：每个会话只采纳时间戳不早于上一条已采纳事件的事件，避免 `Stop` 之后迟到的 `PostToolUse` 把会话拉回工作中。

**提问类工具（2026-11-08 补充）**：

| 工具 | 类型 | 处理 |
|---|---|---|
| Claude Code `AskUserQuestion`、Codex `request_user_input`、dsh `ask_user_question` | 阻塞式：调用期间 Agent 停下等回答 | `PreToolUse` 进入“等你处理：回答”，`PostToolUse` 后恢复工作中 |
| Codex 桌面端 `request_user_input_async` | 异步式：工具立刻返回，Agent 按默认选项继续，问题卡片一直挂着 | `PreToolUse` 记为“提问挂起”，算作“等你处理：回答”，不计入跑速；到 `UserPromptSubmit`、`Stop`、`StopFailure`、`Interrupt` 或 60 分钟超时清除。用户在这一轮中途回答时没有对应事件，会延续到这一轮结束 |

**Codex**：写入 `~/.codex/hooks.json`，超时设为 2 秒（Codex 的超时单位是秒）。

| 事件 | 会话状态 |
|---|---|
| `SessionStart` | 登记为空闲 |
| `UserPromptSubmit`、`PreToolUse`、`PostToolUse` | 工作中 |
| `PermissionRequest` | 等你处理：授权 |
| `Stop` | 空闲，今日完成 +1（之前是工作中或等你处理时） |
| `Interrupt` | 空闲，不计完成 |
| `SessionEnd` | 移除 |

Codex 的子代理沿用父会话的 `session_id`，不需要额外处理。

**安装与卸载**：

- `agent-run install-hooks` 只在用户手动执行时修改配置。写入前，先把两个配置文件备份到 `backup/<时间戳>/`。
- 只追加自己的条目，按命令路径识别，重复执行不会重复添加，也不改动其他已有 hook（两份配置里现在都已有别的 hook）。
- `uninstall-hooks` 只删除命令路径匹配的条目。
- Codex 要求在 CLI 的 `/hooks` 或桌面端 Settings → Hooks 里审查并信任新 hook 后才会执行，安装后要提示用户去做这一步。

### 5.3 dsh

dsh 没有对外的 hook 事件（它仓库里的 `hooks/` 是给 Claude Code 和 Codex 用的桥接）。v1 用文件活动估算：

- 每 5 秒扫描一次 `~/.dsh/sessions/*/session-*/`。
- `session.v4.jsonl.zstd` 的修改时间在 20 秒以内，就算工作中；否则算空闲。
- dsh 无法报告“等你处理”，它的会话只会计入跑速。

这是启发式做法：文件名带格式版本号，`session.lock` 的含义也还没核实（见 §11）。

2026-11-08 补充：dsh 有官方插件 `@deepseek-ai/dsh-hooks-claude-code`，可以执行 Claude Code 格式的 hook（`SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PostToolUse`、`Stop`）。`agent-run install-dsh-hooks` 会生成 `~/.agent-run/dsh-hooks.json`，命令以 `ar_hook.py dsh` 身份上报；在 dsh profile 中挂载该插件后，dsh 也能识别 `ask_user_question` 提问。守护进程收到过 dsh 的 hook 事件后，dsh 计数改以事件为准，不再用文件扫描。

### 5.4 会话状态与计数规则

- 会话以“工具 + `session_id`”为键，有五种状态：空闲、工作中、后台运行（见 §5.2 后台任务）、等你处理（附带原因：授权或回答）、已移除。
- **超时回收**：
  - 工作中超过 15 分钟没有任何事件，转为空闲。这个时限大于 Claude Code Bash 工具 10 分钟的上限。
  - 后台运行超过 2 小时没有事件，转为空闲，不计完成。
  - 等你处理超过 60 分钟没有事件，也转为空闲。
  - 空闲超过 24 小时，从内存中移除。
- **跑速**：只用“工作中”与“后台运行”的会话数（三个工具合计）计算档位，等你处理的会话不计入。
- **今日完成**：按本地时区计数，零点清零。计数写入 `state.json`，守护进程重启后不丢失。
- **等你处理列表**：按进入等待的先后排序。

### 5.5 隐私

守护进程和串口上只有计数、工具代号（CC / CX / DS）和原因代号（A 授权 / Q 回答）。项目名、路径、提示词和会话标题既不进守护进程，也不上串口。设备屏幕朝外佩戴时，别人也只能看到这些信息。

### 5.6 命令行

| 命令 | 作用 |
|---|---|
| `agent-run status` | 在终端打印当前汇总状态。没有设备时，用它验证采集是否正确。Codex 有 agent-run 的 hook 尚未信任时，在末尾给出提示（按 `config.toml` 的 `[hooks.state]` 记录判断）。 |
| `agent-run setup` | 用 `ioreg` 找 Espressif 的 USB 串口（VID `0x303A`），握手确认后把端口写进 `config.json`。 |
| `agent-run install-hooks` / `uninstall-hooks` | 见 §5.2。 |
| `agent-run install-service` / `uninstall-service` | 把程序复制到 `~/.agent-run/`，注册或注销 launchd 用户级服务 `local.agent-run`。 |
| `agent-run run` | 前台运行守护进程，launchd 调用的就是它。 |
| `agent-run demo` | 每 5 秒切换一个画面，把七种画面轮一遍，用于真机验收。 |

## 6. 串口协议

- **线路**：USB Serial/JTAG（macOS 上显示为 `/dev/cu.usbmodem*`）。每条消息一行 ASCII，以 `\n` 结束，`\r` 也视为行尾，单行不超过 160 字节。
- **主机打开串口**：用 `O_NOCTTY | O_NONBLOCK` 打开，清除 `HUPCL`，不主动翻转 DTR/RTS。
- **主机 → 设备**：
  - 握手：`AR1 PING`
  - 状态帧，状态变化时立刻发送，之后每 2 秒重发一次作为心跳：

    ```
    AR1 S seq=<0-65535> cc=<0-99> cx=<0-99> ds=<0-99> done=<0-999> wn=<0-99> w=<列表>
    ```

    - `cc`、`cx`、`ds`：各工具“工作中”的会话数。
    - `done`：今日完成数。
    - `wn`：等你处理的总数。
    - `w`：最多 3 项，格式 `CC:A,CX:Q`；没有等待时写 `-`。
    - 示例：`AR1 S seq=17 cc=3 cx=1 ds=1 done=21 wn=2 w=CC:A,CX:Q`
- **设备 → 主机**：开机时和收到 `PING` 时回 `AR1 HELLO fw=<版本> proto=1`。状态帧不需要确认。
- **兼容规则**：
  - 设备忽略未知键，也忽略不以 `AR1 ` 开头的行。
  - 主机忽略设备输出的日志行。
  - 超长行和字段越界的帧整帧丢弃。
- **重连**：主机连接断开后每 3 秒重试，并只连接 `config.json` 里记录的端口。

## 7. 固件

### 7.1 基线与代码组织

- **基线**：以 `FoloToy/ai-passport` 的 `main` 分支为基线（ESP-IDF 5.5.3，LVGL 版本以 `dependencies.lock` 为准），复用 `components/bsp`，不修改 BSP。
- **入口**：开机直接进入 Agent Run! 界面，不保留 demo 菜单。
- **新增代码**：放在 `main/agent_run/`：

| 模块 | 职责 | 能否在电脑上测试 |
|---|---|---|
| `ar_proto.c/.h` | `ar_line_kind_t ar_proto_parse_line(const char *line, ar_state_t *out)`：解析一行，区分状态帧、`PING` 和其他行；`ar_line_push()`：逐字节装配行 | 纯 C，可宿主测试 |
| `ar_model.c/.h` | `ar_screen_t ar_model_screen(const ar_state_t *s, bool has_state, uint32_t ms_since_frame)`：算出画面；另含帧间隔、速度条、提示音、亮度和背光规则 | 纯 C，可宿主测试 |
| `ar_raster.c/.h` | 直接写 RGB565 缓冲的矩形、混色、5×7 像素字和精灵绘制 | 纯 C，可宿主测试 |
| `ar_sprite.c/.h` | 宠物各帧的像素矩形表 | 数据文件，宿主测试检查边界 |
| `ar_text.h`、`ar_font_cjk14.c` | 全部中文文案，以及据此生成的字体子集 | 宿主测试检查字形覆盖 |
| `ar_link.c` | USB 串口接收任务 | 需真机 |
| `ar_ui.c` | LVGL 界面与动画 | 需真机 |
| `ar_sound.c` | 提示音 | 需真机 |
| `ar_settings.c` | 亮度、静音写入 NVS | 需真机 |

### 7.2 画面映射

| 画面 | 条件 | 宠物动画 | 帧间隔 |
|---|---|---|---|
| 未连接 | 超过 10 秒没收到合法状态帧 | 睡觉，整体变暗 | 600 ms |
| 等你处理 | `wn > 0`（优先于跑速） | 正对屏幕招手 | 300 ms |
| 睡觉 | 工作中合计 0 | 睡觉，Z 上下浮动 | 600 ms |
| 慢走 | 1 | 走路 | 220 ms |
| 小跑 | 2–3 | 小跑 | 150 ms |
| 狂奔 | 4–6 | 狂奔 | 100 ms |
| 冲刺 | ≥7 | 冲刺，带残影和烟尘 | 70 ms |

每种动画 v1 有 2 帧。屏幕稿目前每种只画了 1 帧，第 2 帧需要补画（§8）。

### 7.3 渲染

- 宠物采用 16×12 逻辑像素、放大 6 倍（96×72）。每帧存成矩形列表（`x, y, w, h, 颜色序号`），与屏幕稿 SVG 里的矩形一一对应，放在 Flash 里。
- 像素元素（标题栏、大数字、宠物场景、计数格）都画进 LVGL canvas 的静态 RGB565 缓冲，LVGL 每块只画一张图。不在 LVGL 自绘回调里逐个画矩形：每个矩形都会占一个绘制任务，24 KB 的 LVGL 内存池容易耗尽。静态缓冲合计约 70 KB。
- 宠物场景（204×100）按动画帧整块重画；画面切换时立刻重画一次，不等下一帧。
- 字体：
  - 中文用 Noto Sans SC Regular 生成的 14 px、4 bpp 子集 `ar_font_cjk14`，字符为可打印 ASCII 加 `ar_text.h` 字符串中的全部汉字。
  - 由 `tools/gen_agent_run_font.sh` 调用 `lv_font_conv` 1.5.3 生成；字体源固定为 noto-cjk 仓库 `Sans2.004` 标签，并校验 SHA-256。
  - 未采用 LVGL 内置的思源黑体子集：2026-11-08 核对，界面文案中有 19 个字不在其中（如“睡”“跑”“冲”）。
  - 数字和英文不再使用 Silkscreen，而由 `ar_raster.c` 内置的 5×7 像素字绘制，省去另一份字体素材。
- 标题栏右侧用像素图标表示静音、USB 链路和电量，不显示电量百分比：按 2 倍像素字绘制时，文字放不下。（2026-11-08 修订：改用 1 倍像素字显示百分比，并以音量图标替代静音图标，见无线方案 §7.4。）
- 等你处理列表最多显示两项，总数以提醒条上的数字为准。

### 7.4 串口接收

按上游社区文档 `serial-screenshot-protocol` 记录的经验实现：

- 显式安装 USB-Serial-JTAG 驱动（rx 256 字节、tx 1024 字节），并调用 `usb_serial_jtag_vfs_use_driver()`。
- 接收任务优先级为 3（低于 LVGL 的 4），读错误后退避 200 ms，所有空闲路径都要有 `vTaskDelay()`。
- 不能只靠换行判断行尾：按“行尾符或长度上限”切行，超长时丢弃到下一个行尾。
- 发布版本的日志输出不超过每秒 1 行，避免挤占串口。

### 7.5 按键、亮度与提示音

| 操作 | 作用 |
|---|---|
| UP / DOWN 短按 | 调亮度，5 档，保存到 NVS（2026-11-08 修订：首页暂不分配功能，音量与亮度改从设置菜单调节，见无线方案 §7.4） |
| OK 长按 | 切换静音，保存到 NVS；静音时标题栏显示静音图标（2026-11-08 修订：长按不分配功能，单击确定打开设置菜单，见无线方案 §7.4） |
| 任意键 | 背光关闭时唤醒屏幕 |

- **提示音**：等你处理的数量增加时，播放一次约 150 ms 的双音提示（实时合成，不占存储），两次提示至少间隔 30 秒，静音时不响。
- **背光**：链路正常时屏幕常亮；进入“未连接”5 分钟后关闭背光。v1 不做深度睡眠。（2026-11-08 修订：现行规则见无线方案 §4.4。）

### 7.6 资源预算

- v1 不启动 Wi-Fi 和 BLE。LVGL 内存池保持上游的 24 KB，不加大显示缓冲。
- 串口任务栈 3 KB。
- 调试版本在开机时和之后每 10 分钟，记录一次空闲堆、最大连续空闲块和各任务栈的高水位。

## 8. 屏幕稿需要的修改

1. “等你处理”列表的原因文案改为“等待授权”和“等你回答”，替换原来的“等你回复”。
2. “等你处理”底部右侧的“OK 查看”改为“长按 OK 静音”（已静音时为“长按 OK 取消静音”）。（2026-11-08 修订：现为“确定键设置”。）
3. 标题栏的 `USB` 改为表示链路状态：链路正常时显示，断开时不显示；另外补画静音图标。
4. 新增“未连接电脑”画面：睡觉姿势整体变暗，文案为“未连接电脑”和“插上 USB 并运行 agent-run”。
5. 为每种动画补画第 2 帧。

2026-11-08 实施时，以上五项直接在固件里实现，Design 画布尚未同步更新。

## 9. 测试与验收

### 9.1 主机端测试（先写测试，看它失败，再实现）

- **`test_state.py`**：
  - 两个工具的事件序列能得到正确的各工具计数、等待列表和完成数。
  - 用注入的时钟验证 15 分钟、60 分钟、24 小时三个超时，以及本地零点清零。
  - 带 `agent_id` 的事件不新建会话。
  - 没有工作过的 `Stop` 不计完成。
- **`test_protocol.py`**：
  - 编码结果与 §6 的示例逐字一致。
  - 等待超过 3 项时 `w` 只取前 3 项，`wn` 仍为总数。
  - 任何输入下单行都不超过 160 字节。
- **`test_hook.py`**：
  - 守护进程不在、或输入是坏 JSON 时，`ar_hook.py` 200 ms 内以退出码 0 退出，且无任何输出。
  - 转发出的数据报只含 §5.2 列出的字段。
- **`test_link.py`**：用伪终端（pty）模拟设备，覆盖握手、心跳间隔、断开后重连。

### 9.2 固件宿主测试（在 Mac 上用 cc 编译运行）

- **`ar_proto`**：合法帧、未知键、缺少字段、字段越界、超长行、`w=-`。
- **`ar_model`**：档位边界 0、1、2、3、4、6、7、99；`wn > 0` 优先于跑速；10 秒无帧切到未连接，收到新帧后恢复。
- 同时满足 `idf.py build` 通过。

### 9.3 真机验收清单（需要设备）

- 用 `agent-run demo` 逐一检查七种画面，并与屏幕稿对照。
- 拔线或停掉守护进程后 10 秒内显示“未连接”；恢复后 5 秒内回到正常画面。
- 每新增一个等待项响一次；30 秒内的第二次不响；静音状态和亮度重启后保持。
- 主机打开串口不会导致设备复位：设备运行时间连续，没有出现新的 `HELLO`。
- 连续运行 8 小时不重启，并记录最小空闲堆和最大连续空闲块。

### 9.4 验收标准

| 编号 | 标准 |
|---|---|
| AC1 | 在真实 Claude Code 会话里提交提示后 2 秒内，计数加 1，跑速档位正确。 |
| AC2 | Claude Code 出现权限确认或提问后 2 秒内进入“等你处理”并响一次；处理完 2 秒内恢复。 |
| AC3 | Codex 满足 AC1、AC2 中的授权部分（hook 已在 Codex 中审查并信任）。 |
| AC4 | 一轮结束后 2 秒内，工作中计数减 1，今日完成加 1。 |
| AC5 | dsh 会话有写入后 10 秒内计入，停止写入 30 秒内移出。 |
| AC6 | §9.3 的断线、复位、提示音、8 小时浸泡全部通过。 |
| AC7 | 装上 hook 后两个工具的操作没有可感知的延迟；`ar_hook.py` 单次耗时 p95 不超过 100 ms（实测记录）。 |
| AC8 | 抓取串口数据核对：只出现 §5.5 允许的内容。 |
| AC9 | §9.1、§9.2 的全部测试通过。 |

在 M1 阶段，AC1–AC5 先用 `agent-run status` 代替设备屏幕验证。

## 10. 里程碑

1. **M1 主机采集端（现在就能做，不需要设备）**：完成 §5、§6 的主机部分和 §9.1，用 `agent-run status` 跑通 AC1–AC5、AC7、AC8。
2. **M2 固件（设备到货前可以完成编译和宿主测试）**：先按 §8 补完屏幕稿，再实现 §7 和 §9.2。
3. **M3 真机联调**：完成 §9.3 和 AC6。

后续另写 spec 的方向：蓝牙传输、dsh 插件上报、额度显示、宠物成长、Mac 浮窗、iPhone 实时活动、好友联机。

## 11. 风险与待核实

| 事项 | 影响 | 处理 |
|---|---|---|
| Claude 桌面端 Code 页和 IDE 扩展是否执行用户级 hook | 这些会话可能不计入 | 2026-11-08 实测：桌面端 Code 页会执行，且修改 `settings.json` 后已打开的会话也立即生效。IDE 扩展未测 |
| dsh 文件启发式：`session.lock` 的含义未核实，格式版本可能变化 | dsh 计数不准 | M1 用真实 dsh 会话核对；长期改为插件上报 |
| 主机打开串口时 DTR/RTS 变化可能让 ESP32-C3 复位 | 每次重连设备都重启 | 2026-11-08 实测：按 §6 的方式打开串口，设备没有复位（没有新的开机日志，握手立即成功） |
| 守护进程每 3 秒探测一次串口 | 与 `esptool`、`idf.py monitor` 等抢占端口 | 烧录或调试前先停止守护进程：`launchctl bootout gui/$(id -u)/local.agent-run` |
| 调试日志与协议共用 USB 串口 | 主机误读日志 | 协议行固定前缀，发布版本限制日志频率 |
| Claude Code 的工具名或通知类型改名（如 `AskUserQuestion`） | 等你处理漏报 | 映射表放进 `config.json`，可以不改代码就调整 |
| Codex hook 需要先审查并信任 | 装完不生效 | `install-hooks` 结束时明确提示 |

## 12. 参考

- Claude Code hooks 文档：<https://code.claude.com/docs/en/hooks>（2026-11-08 读取：事件列表、共有字段、`notification_type` 取值、`async` 选项）
- Codex hooks 文档：<https://learn.chatgpt.com/docs/hooks>（2026-11-08 读取：事件列表、超时单位为秒、信任要求、`async` 选项）
- FoloToy/ai-passport 官方仓库：<https://github.com/FoloToy/ai-passport>
  - `docs/reference/y2lin/serial-screenshot-protocol.zh_CN.md`：USB 串口接收的坑
  - `docs/development/engineering/lvgl-chinese-fonts.zh_CN.md`：中文字体子集
  - `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md`：内存与线程规则
