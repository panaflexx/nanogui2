@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_win_pkg.ps1" %*
exit /b %ERRORLEVEL%
