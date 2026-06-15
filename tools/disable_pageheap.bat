@echo off
REM Remove page heap for dusklight.exe (run when done debugging). Self-elevates.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "KEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\dusklight.exe"
reg delete "%KEY%" /f
echo.
echo Page heap DISABLED (IFEO entry removed) for dusklight.exe.
pause
