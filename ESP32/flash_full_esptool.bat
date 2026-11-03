@echo off
setlocal
rem ============================================================================
rem  J280 ESP32-S3 手势桥固件 — 完整烧录（第一次/换了语音分区时用）
rem  默认 COM18；换口改下面一行。
rem ============================================================================
set "PORT=COM18"

set "PY=E:\ESP-IDF\Espressif\python_env\idf5.4_py3.13_env\Scripts\python.exe"
if not exist "%PY%" set "PY=python"

echo Full flashing to %PORT% ...
"%PY%" -m esptool --chip esp32s3 -p %PORT% -b 460800 ^
  --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m ^
  0x0      "%~dp0build\bootloader\bootloader.bin" ^
  0x8000   "%~dp0build\partition_table\partition-table.bin" ^
  0x10000  "%~dp0build\PERIPHERAL.bin" ^
  0x310000 "%~dp0build\srmodels\srmodels.bin" ^
  0x810000 "%~dp0components\esp-sr\esp-tts\esp_tts_chinese\esp_tts_voice_data_xiaole.dat"

if errorlevel 1 (
    echo.
    echo FAILED. Check COM port, USB cable, and that no other tool holds the port.
    pause
    exit /b 1
)
echo.
echo Full flash done. Open the serial monitor at 115200 to see FPGA_LINK logs.
pause
exit /b 0