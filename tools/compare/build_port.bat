@echo off
rem Build ited.exe for the comparison harness into <repo>\build-hdos (Release).
rem Usage: tools\compare\build_port.bat [repo root]   (default: this repo)
setlocal
set PORT=%~1
if "%PORT%"=="" set PORT=%~dp0..\..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%PORT%" -B "%PORT%\build-hdos" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%PORT%\build-hdos" --target ited || exit /b 1
