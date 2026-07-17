#!/usr/bin/env bash
# 重新生成 ui_display 的中文补字字库（需 Node.js；系统需有 TrueType 源字体，勿用 .ttc）
# 当前默认 RANGES：0x3000-0x303F（CJK 标点）、0x4e00-0x9FFF（CJK 基本区）、0xFF00-0xFFEF（全角符号），bpp=1。
# 如 app 分区体积不够，可用 RANGES=0x4e00-0x56FF,0x75B0-0x764F 等缩小范围。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/components/ui_display/lv_font_sc_supplement_16.c"
FONT="${FONT:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"
RANGES="${RANGES:-0x3000-0x303F,0x4e00-0x9FFF,0xFF00-0xFFEF}"
if [[ ! -f "$FONT" ]]; then
  echo "未找到字体: $FONT ，请设置 FONT=你的.ttf 路径" >&2
  exit 1
fi
npx --yes lv_font_conv \
  --font "$FONT" --size 16 --bpp 1 --format lvgl \
  -r "$RANGES" \
  -o "$OUT" \
  --lv-font-name lv_font_sc_supplement_16 \
  --force-fast-kern-format
sed -i 's|"lvgl/lvgl.h"|"lvgl.h"|g' "$OUT"
echo "已写入 $OUT ，请重新 idf.py build"
