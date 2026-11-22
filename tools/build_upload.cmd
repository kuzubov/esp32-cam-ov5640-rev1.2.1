@echo off
rem Wrapper: runs build_upload.ps1 bypassing the PowerShell script execution policy
rem (for this run only, system settings are not changed). All arguments are passed through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_upload.ps1" %*
