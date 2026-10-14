@echo off
rem Double-click entry point for the WSL2 installer. Arguments are passed
rem through, for example:  Install-BackupStudio.cmd -Remove
rem                         Install-BackupStudio.cmd -WhatIf
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-wsl.ps1" %*
if errorlevel 1 (
    echo.
    echo Installation failed; read the messages above.
)
pause
