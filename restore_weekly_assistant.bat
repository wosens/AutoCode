@echo off
rem ============================================================
rem  restore_weekly_assistant.bat
rem  Restores the wiped 10-12 assistant archive of the weekly
rem  report project from the claude session transcript.
rem  (ASCII-only content per project convention)
rem ============================================================

tasklist /FI "IMAGENAME eq AutoCode1.exe" 2>nul | findstr /I "AutoCode1.exe" >nul
if %errorlevel%==0 (
    echo Please close AutoCode first, then run this file again.
    pause
    exit /b 1
)
tasklist /FI "IMAGENAME eq AutoCode.exe" 2>nul | findstr /I "AutoCode.exe" >nul
if %errorlevel%==0 (
    echo Please close AutoCode first, then run this file again.
    pause
    exit /b 1
)

echo Restoring assistant archive from claude transcript ...
python "E:\wosens\Programs\VisualC++\QT\AutoCode\build\restore_zb_asst.py"
if errorlevel 1 (
    echo.
    echo RESTORE FAILED - screenshot this window.
) else (
    echo.
    echo RESTORE OK - reopen the project in AutoCode, the restored
    echo assistant tab is named: 生成个人工作周报
)
pause
