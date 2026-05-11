@echo off
setlocal
cd /d "%~dp0build"
if not exist cef-panel.exe (
    echo Binary not found. Run build.bat first.
    exit /b 1
)
.\cef-panel.exe %*
