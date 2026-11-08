#!/usr/bin/env bash
# scripts/check.sh —— 一次跑完本仓库的全部校验。
#
#   scripts/check.sh            core 的 Python 测试、蓝牙桥接的 Swift 测试、固件的宿主测试
#   scripts/check.sh --firmware 另外构建 AI Passport 固件(需先激活 ESP-IDF 5.5.3)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"

build_firmware=false
if [[ "${1:-}" == "--firmware" ]]; then
    build_firmware=true
fi

echo "== core:Python 测试"
# 使用系统 Python(与 hook、launchd 运行时一致),代码需兼容 3.9
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -m unittest discover -s core/tests -t core

echo "== transports/macos-ble:Swift 测试"
# 只显示 XCTest 的汇总行;测试失败时 swift test 返回非零,pipefail 会让脚本停下
(cd transports/macos-ble && swift test 2>&1 | grep -E "Executed .* tests|error:" | tail -2)

echo "== displays/ai-passport:宿主测试"
fw=displays/ai-passport
tmp="$(mktemp -d "${TMPDIR:-/tmp}/agent-run-check.XXXXXX")"
trap 'rm -rf -- "${tmp}"' EXIT
cc_flags=(-std=c11 -Wall -Wextra -Werror -I"${fw}/main")
"${CC:-cc}" "${cc_flags[@]}" "${fw}/tests/test_ar_proto.c" "${fw}/main/agent_run/ar_proto.c" -o "${tmp}/proto"
"${CC:-cc}" "${cc_flags[@]}" "${fw}/tests/test_ar_model.c" "${fw}/main/agent_run/ar_model.c" -o "${tmp}/model"
"${CC:-cc}" "${cc_flags[@]}" "${fw}/tests/test_ar_raster.c" "${fw}/main/agent_run/ar_raster.c" \
    "${fw}/main/agent_run/ar_sprite.c" -o "${tmp}/raster"
"${CC:-cc}" "${cc_flags[@]}" "${fw}/tests/test_ar_pair.c" "${fw}/main/agent_run/ar_pair.c" -o "${tmp}/pair"
"${CC:-cc}" "${cc_flags[@]}" "${fw}/tests/test_ar_capture.c" "${fw}/main/agent_run/ar_capture_math.c" -o "${tmp}/capture"
for t in proto model raster pair capture; do "${tmp}/${t}"; done
PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_ar_font_coverage.py"

if ${build_firmware}; then
    echo "== displays/ai-passport:固件构建"
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: 没有找到 idf.py,请先激活 ESP-IDF 5.5.3(. ~/esp/esp-idf-v5.5.3/export.sh)" >&2
        exit 1
    fi
    idf.py -C "${fw}" build
    idf.py -C "${fw}" merge-bin -o agent-run-ai-passport-full.bin
    echo "合并镜像:${fw}/build/agent-run-ai-passport-full.bin(从 0x0 烧录)"
fi

echo "全部通过"
