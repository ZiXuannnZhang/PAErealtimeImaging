@echo off
setlocal
rem Explicit operator entry point. Run this file from an elevated terminal;
rem the application never self-elevates and never changes system capture state.
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%start_system_capture_admin.ps1" %*
exit /b %ERRORLEVEL%
