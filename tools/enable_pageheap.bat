@echo off
REM Enable FULL page heap for dusklight.exe (catches heap corruption at the
REM exact corrupting write). Self-elevates via UAC. Run once; relaunch the game.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "KEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\dusklight.exe"
reg add "%KEY%" /v GlobalFlag    /t REG_SZ /d 0x02000000 /f
reg add "%KEY%" /v PageHeapFlags /t REG_SZ /d 0x3 /f
echo.
echo === Current setting ===
reg query "%KEY%"
echo.
echo Full page heap ENABLED for dusklight.exe. Relaunch the game and reproduce.
pause
