@echo off
rem
rem Copyright (c) 2026, Daily
rem
rem Runs setup.ps1 from cmd.exe, whatever PowerShell's execution policy.
rem
rem Usage:
rem   set UE_ROOT=C:\path\to\UnrealEngine
rem   setup.bat
rem

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
