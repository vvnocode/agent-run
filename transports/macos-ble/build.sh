#!/usr/bin/env bash
# bridge/build.sh —— 构建 AgentRunBridge.app 并安装到 ~/Applications(ad-hoc 签名,仅本机使用)。
#
# 每次重新构建签名都会变化,macOS 可能再次请求蓝牙权限。
# 用法:bridge/build.sh [安装路径],默认 ~/Applications/AgentRunBridge.app
set -euo pipefail

cd "$(dirname "$0")"
app="${1:-$HOME/Applications/AgentRunBridge.app}"

swift build -c release
mkdir -p "${app}/Contents/MacOS"
cp .build/release/AgentRunBridge "${app}/Contents/MacOS/AgentRunBridge"
cp Info.plist "${app}/Contents/Info.plist"
codesign --force --sign - "${app}"
echo "已安装 ${app}"
