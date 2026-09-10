#!/usr/bin/env bash
# 编译 SD2 小电视固件（GeekMagic Open Firmware，已适配本板引脚）
# 用法: ./build.sh
set -euo pipefail

FW_DIR="$(cd "$(dirname "$0")/../firmware/GeekMagic-SD2" && pwd)"
PIO="pio"

# 若系统未装 PlatformIO，尝试用虚拟环境
if ! command -v "$PIO" >/dev/null 2>&1; then
  for cand in "$HOME/.venvs/pio/bin/pio" "$(command -v python3 -v 2>/dev/null || true)/../bin/pio"; do
    if [ -x "$cand" ]; then PIO="$cand"; break; fi
  done
fi

if ! command -v "$PIO" >/dev/null 2>&1 && [ ! -x "$PIO" ]; then
  echo "错误: 未找到 PlatformIO。请安装: pipx install platformio" >&2
  exit 1
fi

cd "$FW_DIR"
echo "==> 编译固件 (esp12e) ..."
"$PIO" run -e esp12e
echo "==> 打包 LittleFS 文件系统 ..."
"$PIO" run -e esp12e -t buildfs

OUT="$(cd .. && pwd)/build"
cp -f .pio/build/esp12e/firmware.bin  "$OUT/firmware.bin"
cp -f .pio/build/esp12e/littlefs.bin  "$OUT/littlefs.bin"
echo ""
echo "完成。产物:"
ls -la "$OUT/firmware.bin" "$OUT/littlefs.bin"
