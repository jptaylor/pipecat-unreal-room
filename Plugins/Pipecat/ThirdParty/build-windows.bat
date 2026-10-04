@echo off
rem
rem Copyright (c) 2026, Daily
rem
rem Runs build-windows.ps1 from cmd.exe, whatever PowerShell's execution policy.
rem
rem Usage:
rem   set UE_ROOT=C:\path\to\UnrealEngine
rem   set PIPECAT_CLIENT_CXX=C:\path\to\pipecat-client-cxx
rem   build-windows.bat
rem

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1"
