#!/usr/bin/env bash
# tools/gen_agent_run_font.sh —— 生成 Agent Run! 的中文字体子集 main/agent_run/ar_font_cjk14.c。
#
# 字符清单 = 可打印 ASCII(0x20-0x7E)+ main/agent_run/ar_text.h 中出现的全部非 ASCII 字符。
# 字体源:Noto Sans SC Regular(SIL Open Font License 1.1),固定取 noto-cjk 仓库 Sans2.004 标签。
# 转换工具:lv_font_conv 1.5.3(通过 npx 运行)。
#
# 用法:tools/gen_agent_run_font.sh [字体文件路径]
#   不传路径时把字体下载到 ${AR_FONT_CACHE:-$HOME/.cache/agent-run-fonts}/,并校验 SHA-256。
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
text_header="${repo_root}/main/agent_run/ar_text.h"
output="${repo_root}/main/agent_run/ar_font_cjk14.c"

font_url="https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
# 2026-11-08 下载并核对的源文件哈希;换用其他字体文件时通过 AR_FONT_SHA256 覆盖
font_sha256="${AR_FONT_SHA256:-faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9}"
lv_font_conv_version="1.5.3"

font="${1:-}"
if [[ -z "${font}" ]]; then
    cache="${AR_FONT_CACHE:-$HOME/.cache/agent-run-fonts}"
    font="${cache}/NotoSansSC-Regular.otf"
    if [[ ! -f "${font}" ]]; then
        mkdir -p "${cache}"
        echo "下载字体源:${font_url}"
        curl -fL --retry 3 -o "${font}.part" "${font_url}"
        mv "${font}.part" "${font}"
    fi
fi

actual_sha256="$(shasum -a 256 "${font}" | awk '{print $1}')"
if [[ -n "${font_sha256}" && "${actual_sha256}" != "${font_sha256}" ]]; then
    echo "ERROR: 字体文件 SHA-256 不匹配:${actual_sha256}" >&2
    exit 1
fi

# 只从文案头文件的字符串字面量中提取非 ASCII 字符(注释不算),去重并按码点排序,
# 保证生成结果可复现
symbols="$(python3 -I - "${text_header}" <<'EOF'
import re
import sys
text = open(sys.argv[1], encoding="utf-8").read()
literals = "".join(re.findall(r'"([^"\n]*)"', text))
print("".join(sorted({ch for ch in literals if ord(ch) > 0x7E})), end="")
EOF
)"

echo "字体:${font}(SHA-256 ${actual_sha256})"
echo "非 ASCII 字符:${symbols}"

npx --yes "lv_font_conv@${lv_font_conv_version}" \
    --no-compress --no-prefilter --bpp 4 --size 14 \
    --font "${font}" --range 0x20-0x7E --symbols "${symbols}" \
    --format lvgl --lv-include "lvgl.h" --lv-font-name ar_font_cjk14 \
    -o "${output}"

echo "已生成 ${output}"
