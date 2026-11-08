# Agent Run! 单仓库结构方案

- 日期：2026-11-08
- 状态：2026-11-08 已确认（方案 A）并完成第 6 节第 1–5 步；第 5 步的上传 GitHub 与第 6 步删除 `firmware/` 克隆待用户确认

## 1. 目标

1. 只保留一个 Git 仓库 `agent-run`，准备上传到 GitHub。
2. 以“会话状态采集”为核心，AI Passport 只是展示方式之一；以后加电脑浮窗、手机锁屏、其他设备时，不需要改动核心的目录结构。
3. 上传前清理个人信息，补齐许可证与说明文档。

## 2. 现状

| 仓库 | 内容 | 问题 |
|---|---|---|
| `AIPassport/`（外层） | `docs/`、`host/`（采集程序）、`bridge/`（蓝牙桥接） | 名字以设备命名，与“多种展示方式”的定位不符 |
| `AIPassport/firmware/` | FoloToy/ai-passport 的完整克隆（351 个文件）加 Agent Run 应用 | 真正用到的只有 `components/bsp`（18 个文件）和构建配置；其余是官方的文档、技能、示例与 CI |

## 3. 目标结构

```
agent-run/
  README.md                  项目说明：是什么、支持哪些工具与展示方式、怎么安装
  LICENSE                    MIT
  core/                      采集与汇总（原 host/）：hook 接入、会话状态机、守护进程、命令行
  transports/
    macos-ble/               macOS 蓝牙桥接（原 bridge/）
  displays/
    ai-passport/             AI Passport 固件（ESP-IDF 工程）
  docs/
    protocol.md              展示端契约：AR1 行协议与状态快照字段（从现有 spec 中整理出来）
    specs/                   设计方案
```

以后增加展示方式时，在 `displays/` 下新建目录（如 `macos-panel/`、`ios-live-activity/`），统一以 `docs/protocol.md` 为契约。现在不提前抽象“展示端插件”接口，等第二种展示方式真正开工时再按需要设计。

## 4. 固件怎么并入：方案比较

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 只并入用到的部分（推荐）** | 把 `components/bsp`、根 `CMakeLists.txt`、`partitions.csv`、`sdkconfig.defaults`、`dependencies.lock`、`main/key_guard.h`，加上我们的 `main/` 与测试，复制进 `displays/ai-passport/`；保留 FoloToy 的 MIT 版权声明，并记录来源提交 `488beda` | 仓库干净，目录只含 Agent Run 自己的东西；不混入官方的文档规范与 CI | 官方 BSP 有修复时要手工比对合入。官方仓库从 2026-08-07 到 2026-10-21 共有 21 次提交涉及 BSP，集中在 9 月，10 月以后约每月 2 次 |
| B. git subtree 整体并入 | 用 `git subtree add --squash` 把官方仓库整体放进 `displays/ai-passport/`，改动在其上提交 | 可用 `git subtree pull` 合入官方更新；官方的校验脚本、宿主测试原样可用 | 混入约 290 个与本项目无关的文件，以及官方的中英双语文档等仓库规则 |
| C. git submodule | 固件仍是独立的 GitHub fork，主仓库引用它 | 官方更新合入最方便 | 本质上还是两个仓库，不符合目标 |

采用 A。官方的 `tools/validate.sh`、`check_repo.py` 不再沿用，改为在仓库根目录提供一个统一的校验脚本，依次运行 `core/` 的 Python 测试、`transports/macos-ble/` 的 Swift 测试、固件的宿主测试；装有 ESP-IDF 时再加上固件构建。

## 5. 上传前的清理

- **个人信息：**
  - 文档与测试中的本机路径（形如 `/Users/<用户名>/...`）改为 `~/...` 或占位符；
  - 设备广播名中的真实后缀改为 `AgentRun-XXXX`；
  - 删除 dsh profile 备份文件名等本机细节。
- **不提交的内容：** 构建产物（`build/`、`.build/`、`managed_components/`、`sdkconfig`）、字体源文件、运行数据。
- **许可证：**
  - 本项目代码用 MIT；
  - `displays/ai-passport/` 保留 FoloToy 的 MIT 声明；
  - 字体子集注明 Noto Sans SC 的 OFL 1.1。
- **说明文档：** README 写明支持矩阵（Claude Code、Codex、dsh 的桌面端、CLI、网页端），以及已知限制（dsh 需要 hook 插件、Codex 需要信任 hook）。

## 6. 迁移步骤

1. 在当前目录调整结构：`host/` → `core/`，`bridge/` → `transports/macos-ble/`；按方案 A 从 `firmware/` 复制固件文件到 `displays/ai-passport/`。
2. 修正各处路径引用（部署脚本、文档链接、固件 CMake 中的组件路径），跑通统一校验脚本与固件构建。
3. 在本机重新部署一次 `agent-run`，确认采集、USB、蓝牙都正常。
4. 完成第 5 节的清理。
5. 提交到本地 `main`。上传 GitHub 需要你先确认仓库所属账号、名称和公开或私有；本机没有安装 `gh`，可以由你在网页上新建空仓库并把地址给我，或者你安装 `gh` 并登录后由我创建。
6. 确认无误后删除 `firmware/` 克隆。

## 7. 验收

- 仓库里只有一个 `.git`；`git ls-files` 中不含构建产物、字体源文件和本机路径。
- 统一校验脚本全部通过，固件能构建，刷入后真机行为与迁移前一致。
- 本机部署的采集程序与桥接程序从新路径重新部署后工作正常。
