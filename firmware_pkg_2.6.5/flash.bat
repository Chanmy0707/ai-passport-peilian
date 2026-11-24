@echo off
REM 烧录 folotoy/ai-passport 固件（ESP32-C3，8MB Flash）
REM 依赖：pip install esptool
REM 用法：flash.bat COM3        （换成设备管理器里实际的 COM 口）
if "%~1"=="" (
  echo 用法: flash.bat COM3
  exit /b 1
)
python -m esptool --chip esp32c3 -b 460800 --before default-reset --after hard-reset --flash-mode dio --flash-size 8MB --flash-freq 80m -p %1 write-flash 0x0 folotoy-ai-passport-2.6.5-merged.bin
