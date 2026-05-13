@echo off
REM Launch neon-robot-cef. Pass through any extra args (e.g. --url, --usd).
pushd build\Release
neon-robot-cef.exe %*
set rc=%errorlevel%
popd
exit /b %rc%
