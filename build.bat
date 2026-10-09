@echo off
rem Builds with the CMake and Ninja that ship with VS Build Tools (the MinGW cmake cannot verify GitHub's certificate)
set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
cmake --build build || exit /b 1
