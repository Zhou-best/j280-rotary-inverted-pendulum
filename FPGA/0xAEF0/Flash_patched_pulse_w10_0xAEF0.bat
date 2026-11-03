@echo off
setlocal
rem ============================================================================
rem  J280 — 0xD0FA + 手势命令 脉宽 W10 输入 + AB11 ACK 测试位流 (0xAEF0)
rem  SRAM 烧录，断电即失；烧录会清编码器，烧录瞬间转臂位置就是 0°。
rem  本机实测下载线：Gowin USB Cable(GWU2X)，cable-index 0。
rem ============================================================================

set "FS=%~dp0patched_pulse_w10.fs"
if not exist "%FS%" goto :nofs

set "PROG=E:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not exist "%PROG%" set "PROG=C:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not exist "%PROG%" set "PROG=%ProgramFiles%\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not exist "%PROG%" goto :noprog

echo Using: %PROG%
echo Target : %FS%
echo.
echo SRAM program - power-off loses it.
echo Trying GWU2X ...
"%PROG%" -d GW2A-55C -r 2 --cable "Gowin USB Cable(GWU2X)" --cable-index 0 --fsFile "%FS%"
if errorlevel 1 goto :fallback
goto :done

:fallback
echo.
echo First attempt failed; retrying with default cable...
"%PROG%" -d GW2A-55C -r 2 --cable-index 0 --fsFile "%FS%"
if errorlevel 1 goto :failed

:done
echo.
echo Check the User Code above; it must be 0x00000FB7 for this test build.
pause
exit /b 0

:failed
echo.
echo Programming failed. Check board power, JTAG cable, and cable index.
pause
exit /b 1

:nofs
echo Bitstream not found: %FS%
pause
exit /b 1

:noprog
echo programmer_cli.exe not found. Edit PROG in this bat to your Gowin install.
pause
exit /b 1