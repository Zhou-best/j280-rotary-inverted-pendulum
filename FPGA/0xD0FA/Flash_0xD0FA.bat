@echo off
setlocal
rem ============================================================================
rem  J280 0xD0FA SRAM flash helper
rem  Portable: reads ..\GowinRoot.txt and ..\Cable.txt (see *.example.txt).
rem  No parenthesised blocks anywhere: cmd mis-parses unescaped ( ) inside them,
rem  which used to break this script right after programming started.
rem ============================================================================

set "FS=%~dp0furuta_lqr_ctrl_0xD0FA.fs"
if not exist "%FS%" goto :no_fs

rem ---- Gowin install root: env var > ..\GowinRoot.txt > common paths --------
rem Read-Config.ps1 handles BOM / ANSI / CRLF / comments. -NonInteractive and
rem 2^>nul keep PowerShell's banner and error text out of the captured value:
rem without them a failed call yields CFG="Windows PowerShell" and we would then
rem happily program with the wrong tool path.
set "CFGRDR=%~dp0..\Read-Config.ps1"
set "CFG="
if not exist "%CFGRDR%" echo WARNING: Read-Config.ps1 is missing; GowinRoot.txt and Cable.txt will be IGNORED.
if not exist "%CFGRDR%" goto :skip_cfg
if exist "%~dp0..\GowinRoot.txt" for /f "usebackq delims=" %%G in (`powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%CFGRDR%" GowinRoot.txt 2^>nul`) do if not defined CFG set "CFG=%%G"
if not defined CFG if exist "%~dp0..\GowinRoot.txt" echo WARNING: GowinRoot.txt exists but could not be read; using default Gowin paths.
:skip_cfg

set "PROG="
if defined J280_GOWIN_ROOT if exist "%J280_GOWIN_ROOT%\Programmer\bin\programmer_cli.exe" set "PROG=%J280_GOWIN_ROOT%\Programmer\bin\programmer_cli.exe"
if not defined PROG if defined CFG if exist "%CFG%\Programmer\bin\programmer_cli.exe" set "PROG=%CFG%\Programmer\bin\programmer_cli.exe"
if not defined PROG if exist "D:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe" set "PROG=D:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not defined PROG if exist "C:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe" set "PROG=C:\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not defined PROG if exist "%ProgramFiles%\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe" set "PROG=%ProgramFiles%\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"
if not defined PROG if exist "%ProgramFiles(x86)%\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe" set "PROG=%ProgramFiles(x86)%\Gowin\Gowin_V1.9.12.03_x64\Programmer\bin\programmer_cli.exe"

if not defined PROG goto :no_prog
if defined CFG echo GowinRoot.txt: %CFG%

rem ---- download cable: ..\Cable.txt overrides the default --------------------
set "CABLE=Gowin USB Cable(GWU2X)"
set "CABLECFG="
if exist "%CFGRDR%" if exist "%~dp0..\Cable.txt" for /f "usebackq delims=" %%C in (`powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%CFGRDR%" Cable.txt 2^>nul`) do if not defined CABLECFG set "CABLECFG=%%C"
if defined CABLECFG set "CABLE=%CABLECFG%"

echo Using: %PROG%
echo Target bitstream: %FS%
echo.
echo SRAM program - power-off loses it. Make sure the board is powered and the JTAG cable is connected.
echo Trying %CABLE% ...
"%PROG%" -d GW2A-55C -r 2 --cable "%CABLE%" --cable-index 0 --fsFile "%FS%"
if errorlevel 1 goto :fallback
goto :done

:fallback
echo.
echo First attempt failed; retrying with the default cable, FT2CH or auto...
"%PROG%" -d GW2A-55C -r 2 --cable-index 0 --fsFile "%FS%"
if errorlevel 1 goto :failed

:done
echo.
echo Check the User Code above; it must be 0x0000D0FA.
pause
exit /b 0

:failed
echo.
echo Programming failed. Check board power, the JTAG cable, Cable.txt, and GowinRoot.txt.
pause
exit /b 1

:no_prog
echo Gowin programmer_cli.exe not found.
if defined CFG echo   GowinRoot.txt said: %CFG%   -- but programmer_cli.exe is not under it.
echo.
echo Fix: install Gowin EDA, then create GowinRoot.txt in the handoff root folder
echo with ONE line holding the install root, for example:
echo     C:\Gowin\Gowin_V1.9.12.03_x64
echo See GowinRoot.example.txt.
pause
exit /b 1

:no_fs
echo MISSING bitstream file.
echo Looked for: "%FS%"
pause
exit /b 1