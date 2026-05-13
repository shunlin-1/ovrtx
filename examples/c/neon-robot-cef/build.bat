@echo off
cmake --build build --config Release --target neon-robot-cef --parallel
exit /b %errorlevel%
