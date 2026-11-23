@echo off
REM 烧录 folotoy/ai-passport 固件（ESP32-C3，8MB Flash）
REM 依赖：Python3 + esptool（pip install esptool）
REM 用法：flash.bat [串口]    默认 COM3
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM3
python -m esptool --chip esp32c3 -b 460800 --before default-reset --after hard-reset --flash-mode dio --flash-size 8MB --flash-freq 80m -p %PORT% write-flash 0x0 folotoy-ai-passport-2.6.4-merged.bin
endlocal
