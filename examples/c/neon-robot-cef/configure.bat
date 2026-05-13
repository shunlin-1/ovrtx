@echo off
REM Configure neon-robot-cef on Windows. Downloads CEF (~250 MB) on first run.
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
if errorlevel 1 (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
if errorlevel 1 (
    echo Could not find vcvars64.bat. Edit configure.bat to point at your VS install.
    exit /b 1
)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
exit /b %errorlevel%
