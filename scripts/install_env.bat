@echo off
chcp 936 >nul
:: 本脚本以 GBK(ANSI) 编码 + CRLF 换行保存；中文 Windows 默认代码页 936 直接正确显示。
:: 切勿改成 UTF-8 + chcp 65001：cmd.exe 解析含多字节中文的批处理会按错偏移截断命令行。
setlocal EnableDelayedExpansion
:: 保留初始进程 PATH（含 winget 的 WindowsApps 等），RefreshPath 会追加它防丢失。
set "_ORIG_PATH=!PATH!"
title AutoCode 环境安装
cd /d "%~dp0"

echo ============================================================
echo   AutoCode 环境安装
echo   将安装：Git、Node.js（LTS）、Claude Code CLI
echo   （可选）智谱 GLM 桥接工具，让 Claude Code 走智谱模型
echo ============================================================
echo.

:: ---- 0. 管理员权限检测（写 Program Files / 改系统 PATH 需要）----
net session >nul 2>&1
if !errorlevel! neq 0 (
    echo [提示] 安装需要管理员权限，正在请求 UAC 提权...
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)
echo [OK] 已取得管理员权限。
echo.

:: ---- 1. 选择 Claude Code 后端 ----
echo ============================================================
echo   请选择 Claude Code 后端模型：
echo     [1] Anthropic 原版（需联网环境 + Anthropic 账号/API key）
echo     [2] 智谱 GLM（国内推荐，网络畅通），需自备 API key
echo ============================================================
choice /c 12 /n /m "请输入（1 或 2）: "
if !errorlevel! equ 2 (
    set "BACKEND=GLM"
) else (
    set "BACKEND=ANTHROPIC"
)
echo.
echo [OK] 已选择：!BACKEND!
echo.

:: ---- 2. 检测已安装组件（已装的跳过，不重复安装）----
set "HAVE_NODE=0"
set "HAVE_GIT=0"
set "HAVE_CLAUDE=0"
set "HAVE_HELPER=0"
where node   >nul 2>&1 && set "HAVE_NODE=1"
where git    >nul 2>&1 && set "HAVE_GIT=1"
where claude >nul 2>&1 && set "HAVE_CLAUDE=1"
if /i "!BACKEND!"=="GLM" ( where chelper >nul 2>&1 && set "HAVE_HELPER=1" )

echo [检测]
if "!HAVE_NODE!"=="1"   (echo    node     已安装) else (echo    node     缺失)
if "!HAVE_GIT!"=="1"    (echo    git      已安装) else (echo    git      缺失)
if "!HAVE_CLAUDE!"=="1" (echo    claude   已安装) else (echo    claude   缺失)
if /i "!BACKEND!"=="GLM" (
    if "!HAVE_HELPER!"=="1" (echo    chelper  已安装) else (echo    chelper  缺失)
)

:: winget 是否可用（非致命：缺失时改走直链下载）
set "HAVE_WINGET=0"
where winget >nul 2>&1 && set "HAVE_WINGET=1"
if "!HAVE_WINGET!"=="1" (
    echo    winget   可用（优先用它安装）
) else (
    echo    winget   不可用（将走直链下载兜底）
)
echo.

:: 是否还有需要安装的组件
set "NEED=0"
if "!HAVE_NODE!"=="0"   set "NEED=1"
if "!HAVE_GIT!"=="0"    set "NEED=1"
if "!HAVE_CLAUDE!"=="0" set "NEED=1"
if /i "!BACKEND!"=="GLM" if "!HAVE_HELPER!"=="0" set "NEED=1"

if "!NEED!"=="0" (
    echo [完成] 所有必要组件均已就绪，无需安装。
    echo.
    goto :VERIFY
)

:: ---- 3. Node.js（LTS，Claude Code 要求 Node ^>= 18）----
if "!HAVE_NODE!"=="0" (
    echo [安装] Node.js LTS ...
    set "NODE_OK=0"
    if "!HAVE_WINGET!"=="1" (
        echo   - 尝试 winget（仅 winget 源，避开 msstore 协议；失败自动转直链）...
        winget install --id OpenJS.NodeJS.LTS -e --source winget --accept-source-agreements --accept-package-agreements --disable-interactivity
        call :RefreshPath
        where node >nul 2>&1 && set "NODE_OK=1"
    )
    if "!NODE_OK!"=="0" (
        if "!HAVE_WINGET!"=="1" ( echo   - winget 未装成，改用直链下载 Node.js LTS（MSI）... )
        call :InstallNodeDl
        call :RefreshPath
        where node >nul 2>&1 && set "NODE_OK=1"
    )
    if "!NODE_OK!"=="1" ( set "HAVE_NODE=1" ) else ( echo [警告] Node.js 仍不在 PATH，请关闭窗口重试。 )
)
echo.

:: ---- 4. Git ----
if "!HAVE_GIT!"=="0" (
    echo [安装] Git ...
    set "GIT_OK=0"
    if "!HAVE_WINGET!"=="1" (
        echo   - 尝试 winget（仅 winget 源；失败自动转直链）...
        winget install --id Git.Git -e --source winget --accept-source-agreements --accept-package-agreements --disable-interactivity
        call :RefreshPath
        where git >nul 2>&1 && set "GIT_OK=1"
    )
    if "!GIT_OK!"=="0" (
        if "!HAVE_WINGET!"=="1" ( echo   - winget 未装成，改用直链下载 Git（64-bit）... )
        call :InstallGitDl
        call :RefreshPath
        where git >nul 2>&1 && set "GIT_OK=1"
    )
    if "!GIT_OK!"=="1" ( set "HAVE_GIT=1" ) else ( echo [警告] Git 仍不在 PATH，请关闭窗口重试。 )
)
echo.

:: ---- 5. 确认 npm 就绪 ----
call :RefreshPath
where npm >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 仍找不到 npm（Node 未装好或 PATH 未刷新）。请关闭本窗口，重开终端再试。
    goto :END
)
echo [OK] npm 就绪。
echo.

:: ---- 6. 仅当需要 npm 安装（claude / chelper）时，统一 npm 全局目录 ----
set "DO_NPMCFG=0"
if "!HAVE_CLAUDE!"=="0" set "DO_NPMCFG=1"
if /i "!BACKEND!"=="GLM" if "!HAVE_HELPER!"=="0" set "DO_NPMCFG=1"
if "!DO_NPMCFG!"=="1" (
    set "NPM_GLOBAL=C:\Program Files\npm"
    echo [配置] npm 全局目录 = !NPM_GLOBAL!
    if not exist "!NPM_GLOBAL!" mkdir "!NPM_GLOBAL!"
    icacls "!NPM_GLOBAL!" /grant "Users:(OI)(CI)M" /T >nul 2>&1
    call npm config set prefix "!NPM_GLOBAL!"
    call npm config set cache "!NPM_GLOBAL!\cache"
    powershell -NoProfile -Command "$cur=[Environment]::GetEnvironmentVariable('Path','Machine'); if($cur -notlike '*Program Files\npm*'){ [Environment]::SetEnvironmentVariable('Path', $cur + ';C:\Program Files\npm', 'Machine'); Write-Host '[OK] PATH added' } else { Write-Host '[OK] PATH already present' }"
    call :RefreshPath
    echo.
)

:: ---- 7. npm 镜像源（国内加速，总是设置无妨）----
echo [配置] npm 镜像源 = https://registry.npmmirror.com
call npm config set registry https://registry.npmmirror.com
echo.

:: ---- 8. Claude Code CLI（claude 命令）----
where claude >nul 2>&1
if !errorlevel! equ 0 (
    echo [OK] Claude Code 已安装，跳过。
) else (
    echo [安装] @anthropic-ai/claude-code ...
    call npm install -g @anthropic-ai/claude-code
    call :RefreshPath
)
echo.

:: ---- 9. 智谱 GLM 桥接（仅 GLM）----
if /i "!BACKEND!"=="GLM" (
    where chelper >nul 2>&1
    if !errorlevel! equ 0 (
        echo [OK] 智谱桥接 chelper 已安装，跳过。
    ) else (
        echo [安装] @z_ai/coding-helper ...
        call npm install -g @z_ai/coding-helper
        call :RefreshPath
    )
    echo.
)

:: ---- 10. 安装结果验证 ----
:VERIFY
echo ============================================================
echo   安装结果
echo ============================================================
call :ShowVer git --version
call :ShowVer node --version
call :ShowVer npm --version
call :ShowVer claude --version
if /i "!BACKEND!"=="GLM" (
    where chelper >nul 2>&1
    if !errorlevel! equ 0 (
        echo [chelper] 已安装，运行 chelper 完成配置：
    ) else (
        echo [缺失] chelper
    )
)
echo.

:: ---- 11. 后续操作提示 ----
echo ============================================================
echo   后续配置（需要联网，请按提示完成）：
echo ============================================================
if /i "!BACKEND!"=="GLM" (
    echo   你选择了「智谱 GLM」后端，请按以下配置：
    echo     1. 打开一个新的命令行窗口（cmd 或 PowerShell）
    echo     2. 输入：chelper
    echo     3. 按提示输入「智谱 API key」（便于本脚本保存）
    echo     4. 点选「编辑工具」-^>「Claude Code」-^>「重新安装」。
    echo   配置完成后，claude 命令将走智谱 GLM 模型。
) else (
    echo   你选择了「Anthropic 原版」后端，首次使用请登录：
    echo     1. 打开一个新的命令行窗口（cmd 或 PowerShell）
    echo     2. 输入：claude
    echo     3. 按提示用 Anthropic 账号登录（或设置环境变量 ANTHROPIC_API_KEY）
)
echo.
echo   ※ 若 claude / chelper 命令仍未识别，请关闭并重开命令行窗口（让新 PATH 生效）。
echo ============================================================

:END
echo.
pause
exit /b

:: ==================== 子过程 ====================

:RefreshPath
:: 从注册表重读 Machine + User PATH 前置合并；并追加初始进程 PATH（_ORIG_PATH）防丢失。
:: 直接覆盖会因提权后 HKCU 是管理员账户，丢掉原用户 PATH（含 winget 的 WindowsApps），
:: 导致 winget 第二次调用报「不是内部命令」。
set "M_PATH="
set "U_PATH="
for /f "usebackq tokens=2,*" %%A in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PATH 2^>nul`) do set "M_PATH=%%B"
for /f "usebackq tokens=2,*" %%A in (`reg query "HKCU\Environment" /v PATH 2^>nul`) do set "U_PATH=%%B"
set "PATH=!M_PATH!"
if defined U_PATH set "PATH=!PATH!;!U_PATH!"
set "PATH=!PATH!;!_ORIG_PATH!"
goto :eof

:ShowVer
:: %1=命令  %2=版本参数；已安装则打印版本，未安装则提示缺失
where %1 >nul 2>&1
if !errorlevel! equ 0 (
    echo [%1]
    %1 %2
    echo.
) else (
    echo [缺失] %1
)
goto :eof

:InstallNodeDl
:: 无 winget 兜底：从 nodejs.org 取最新 LTS 的 x64 MSI 静默安装。
:: 单引号包 PowerShell 字符串、外层双引号包 -Command，避免 cmd/PS 引号冲突。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocol]::Tls12; try { $i=Invoke-RestMethod https://nodejs.org/dist/index.json; $v=($i|Where-Object{$_.lts -ne $false}|Select-Object -First 1).version; Write-Host ('Node LTS '+$v); $p=$env:TEMP+'\node-lts.msi'; Invoke-WebRequest ('https://nodejs.org/dist/'+$v+'/node-'+$v+'-x64.msi') -OutFile $p; Write-Host 'Installing MSI...'; Start-Process msiexec -ArgumentList @('/i',$p,'/qb','/norestart') -Wait } catch { Write-Host ('[错误] '+$_.Exception.Message) }"
goto :eof

:InstallGitDl
:: 无 winget 兜底：从 GitHub git-for-windows latest release 取 64-bit 安装包静默安装。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocol]::Tls12; try { $r=Invoke-RestMethod https://api.github.com/repos/git-for-windows/git/releases/latest; $a=$r.assets|Where-Object{$_.name -like '*-64-bit.exe' -and $_.name -notlike '*mini*'}|Select-Object -First 1; Write-Host ('Git '+$a.name); $p=$env:TEMP+'\git-setup.exe'; Invoke-WebRequest $a.browser_download_url -OutFile $p; Write-Host 'Installing...'; Start-Process $p -ArgumentList '/VERYSILENT','/NORESTART','/SP-' -Wait } catch { Write-Host ('[错误] '+$_.Exception.Message) }"
goto :eof
