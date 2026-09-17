@echo off
rem peekGIS launcher: delegates to peekgis.vbs (hidden launch, no black window flash).
rem This file lives in launcher/, the repo root is one level above.
rem NOTE: this .cmd is itself a console script, so launching it will briefly show a
rem console window. For a truly flash-free launch double-click peekgis.vbs or the exe.
setlocal enableextensions
set "ROOT=%~dp0.."
set "BIN=%ROOT%\build\windows\x64\release\bin\peekgis.exe"
set "VBS=%~dp0peekgis.vbs"

if not exist "%BIN%" (
    echo [peekGIS] not found: %BIN%
    echo           build first:  xmake build peekgis
    pause
    exit /b 1
)

if exist "%VBS%" (
    start "" wscript.exe "%VBS%"
) else (
    start "" /D "%ROOT%\build\windows\x64\release\bin" "%BIN%"
)
exit /b 0