#!/usr/bin/env bash
# 烧录 SD2 小电视（固件 0x0 + LittleFS 0x200000）
# 用法: ./flash.sh            # 自动探测串口
#       ./flash.sh /dev/cu.usbserial-210   # 指定串口
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE="$ROOT/build/firmware.bin"
LITTLEFS="$ROOT/build/littlefs.bin"

for f in "$FIRMWARE" "$LITTLEFS"; do
  if [ ! -f "$f" ]; then
    echo "错误: 缺少 $f，请先运行 scripts/build.sh" >&2
    exit 1
  fi
done

# 找 esptool
ESPTOOL="esptool.py"
if ! command -v "$ESPTOOL" >/dev/null 2>&1; then
  # PlatformIO 自带 esptool（新包名 tool-esptoolpy，旧包名 tools-esptool）
  for cand in "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" \
              "$HOME/.platformio/packages/tool-esptoolpy"*/esptool.py \
              "$HOME/.platformio/packages/tools-esptool/esptool.py" \
              "$HOME/.platformio/packages/tools-esptool"*/esptool.py; do
    if [ -f "$cand" ]; then ESPTOOL="$cand"; break; fi
  done
fi

# 探测串口
PORT="${1:-}"
if [ -z "$PORT" ]; then
  echo "未指定串口，自动探测..."
  PORT="$(ls /dev/cu.usb* 2>/dev/null | grep -viE 'Bluetooth|debug-console' | head -n1 || true)"
  if [ -z "$PORT" ]; then
    echo "错误: 未找到 USB 串口，请用 ./flash.sh /dev/cu.usbserial-XXX 指定" >&2
    exit 1
  fi
fi
echo "使用串口: $PORT"

# PlatformIO 的 esptool.py 不可直接执行，需用 python3 调用
if [[ "$ESPTOOL" == *.py ]]; then
  PYSP="python3"
else
  PYSP=""
fi

echo "==> 烧录 firmware.bin 到 0x0 ..."
$PYSP "$ESPTOOL" --chip esp8266 --port "$PORT" --baud 115200 \
  write_flash 0x0 "$FIRMWARE"

echo "==> 烧录 littlefs.bin 到 0x200000 ..."
$PYSP "$ESPTOOL" --chip esp8266 --port "$PORT" --baud 115200 \
  write_flash 0x200000 "$LITTLEFS"

echo ""
echo "烧录完成，设备已重启。"
echo "首次启动需配置 WiFi（见 data/config.json 或 GeekMagic AP 配网）"
echo "用局域网 IP 访问 Web 面板（或扫 AP: GeekMagic / 密码 \$str0ngPa\$\$w0rd）"
