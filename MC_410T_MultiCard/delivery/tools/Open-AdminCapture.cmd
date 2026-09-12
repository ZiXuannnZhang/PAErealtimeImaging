@echo off
setlocal
rem Explicit operator entry point. Run this file from an elevated terminal;
rem the application never self-elevates and never changes system capture state.
set "SCRIPT_DIR=%~dp0"
pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%start_system_capture_admin.ps1" %*
set "CAPTURE_EXIT=%ERRORLEVEL%"
if not "%CAPTURE_EXIT%"=="0" (
    echo.
    echo System capture did not complete successfully. See the reason above and the application status.
    echo After restarting the application, start listening and prepare a NEW system capture request.
    if not defined SYSTEM_CAPTURE_COMMAND_ADAPTER pause
)
exit /b %CAPTURE_EXIT%
