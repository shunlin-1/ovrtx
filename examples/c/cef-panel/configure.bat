@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to load vcvars64
    exit /b 1
)
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
exit /b %errorlevel%
