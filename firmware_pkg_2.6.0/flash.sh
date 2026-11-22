#!/usr/bin/env bash
# 烧录 folotoy/ai-passport 固件（ESP32-C3，8MB Flash）
# 依赖：Python3 + esptool（pip install esptool）
# 用法：./flash.sh [串口]    默认 /dev/ttyACM0（Linux/macOS）
set -e
PORT="${1:-/dev/ttyACM0}"
python3 -m esptool --chip esp32c3 -b 460800 --before default-reset --after hard-reset \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  -p "$PORT" write-flash 0x0 folotoy-ai-passport-2.6.0-merged.bin
