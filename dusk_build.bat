@echo off
REM Helper: set up MSVC env + VS-bundled cmake/ninja, then run the given cmake step.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VSCMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%VSCMAKE%;%VSNINJA%;%PATH%"
cd /d "C:\OGames\Dolphin\dusklight"
echo === cmake: %CMAKE_VERSION% ===
cmake --version
if "%1"=="configure" (
  cmake --preset windows-msvc-relwithdebinfo
) else if "%1"=="build" (
  cmake --build --preset windows-msvc-relwithdebinfo
) else if "%1"=="build-target" (
  cmake --build --preset windows-msvc-relwithdebinfo --target %2
) else (
  echo unknown step "%1"
  exit /b 2
)
