@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\build_msvc.ps1" %*
set EXIT=%ERRORLEVEL%
if %EXIT% neq 0 (
    echo.
    echo Build/Deploy fehlgeschlagen (exit %EXIT%).
    pause
    exit /b %EXIT%
)
echo.
pause
exit /b 0
