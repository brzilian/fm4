@echo off
rem Starts the FM-4 webapp on Windows (no Python needed).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1"
if errorlevel 1 pause
