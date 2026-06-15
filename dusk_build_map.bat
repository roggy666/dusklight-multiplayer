@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "P1=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "P2=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%P1%;%P2%;%PATH%"
cd /d "C:\OGames\Dolphin\dusklight"
cmake -S . -B build/windows-msvc-relwithdebinfo "-DCMAKE_EXE_LINKER_FLAGS=/MAP"
cmake --build build/windows-msvc-relwithdebinfo
