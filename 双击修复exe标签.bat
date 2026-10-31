@echo off
chcp 65001 >nul
set "ROOT=E:\wosens\Programs\VisualC++\QT\AutoCode"

echo [1/4] 当场修复全部 exe 标签（约 10 秒）...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\FixExeLabelGuard.ps1"
if errorlevel 1 echo (脚本有告警，继续看后面验证结果)

echo.
echo [2/4] 注册守卫计划任务（每 5 分钟自动巡检，以后部署新 exe 自动修复）...
schtasks /Create /TN "AutoCodeExeLabelGuard" /TR "wscript.exe %ROOT%\scripts\FixExeLabelGuard.vbs" /SC MINUTE /MO 5 /F

echo.
echo [3/4] 触发一次任务，验证链路...
schtasks /Run /TN "AutoCodeExeLabelGuard"

echo.
echo [4/4] 验证结果（两行都应显示 Medium Mandatory Level）:
icacls "%ROOT%\Execute - 副本\AutoCode1.exe" | findstr /C:"Mandatory"
icacls "%ROOT%\Execute\AutoCode.exe" | findstr /C:"Mandatory"

echo.
echo ============================================================
echo  显示 Medium 即修复完成：请关闭 AutoCode，重新打开周报工程。
echo  若仍显示 Low：把本窗口截图发给助手。
echo ============================================================
pause
