@echo off
rem Convenience launcher for the spike. Pass any args through.
rem Examples:
rem   run.bat --mode baseline
rem   run.bat --mode cef-blank --duration 15
rem   run.bat --mode cef-stress --duration 30 --csv stress.csv
rem   run.bat --mode interactive
rem   run.bat --mode interactive --url https://example.com

setlocal
cd /d "%~dp0build"
if not exist cef-thread-spike.exe (
    echo Binary not found. Run build.bat first.
    exit /b 1
)
cef-thread-spike.exe %*
