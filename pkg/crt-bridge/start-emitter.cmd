@echo off
cd /d "%~dp0"
start "" "%~dp0crt-bridge-emitter.exe" --config "%~dp0crt-bridge.cfg" %*
