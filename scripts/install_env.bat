@echo off
chcp 936 >nul
:: 本脚本须 GBK(ANSI) 编码 + CRLF 保存；否则 Windows 默认代码页 936 下中文会乱码，
:: 强行存 UTF-8 + chcp 65001 时 cmd.exe 解析多字节路径会按偏移截断出问题。
setlocal EnableDelayedExpansion
:: 缓存初始进程 PATH（winget 在 WindowsApps 等），RefreshPath 后追加回防丢失。
set "_ORIG_PATH=!PATH!"

:: ---- 参数：/dsh = 只安装 DeepSeekHarness（设置页「安装 DeepSeekHarness」按钮调用）----
set "DSH_ONLY=0"
if /i "%~1"=="/dsh" set "DSH_ONLY=1"
:: ---- 参数：/pi = 只安装 PIAgent（设置页「安装 PIAgent」按钮调用）----
set "PI_ONLY=0"
if /i "%~1"=="/pi" set "PI_ONLY=1"
:: ---- 参数/pi-npm = 联网安装 PIAgent（npm 全局装；设置页「联网安装（npm）」按钮
::      调用，2026.10.10 新增。修旧直连 cmd /k npm 的实害：提权子进程继承的是
::      AutoCode 启动时的旧环境，刚装完 Node 未重启主程序必报找不到 npm；本模式
::      在窗口内 :RefreshPath 读注册表 PATH 并缺 Node 时自动补装后再全局装）----
set "PI_NPM_ONLY=0"
if /i "%~1"=="/pi-npm" set "PI_NPM_ONLY=1"
:: ---- 参数/codex = 只安装 CodexHarness（设置页「安装 CodexHarness…」按钮调用）----
set "CODEX_ONLY=0"
if /i "%~1"=="/codex" set "CODEX_ONLY=1"
:: ---- 参数/hermes <repoUrl> = 联网安装 Hermes Harness（设置页 Hermes 页
::      「联网安装（git）」按钮调用；repoUrl=Hermes Harness git 仓库地址，第 2 参传入，
::      已装时为 fetch+reset 更新）----
set "HERMES_ONLY=0"
if /i "%~1"=="/hermes" set "HERMES_ONLY=1"
:: ---- 参数/update-dsh <tag> = 更新 DeepSeekHarness 到指定 tag（fetch+reset 保留本地；
::      版本管理对话框「更新」调用，tag 由 AutoCode 经 ls-remote 解析后传入）----
set "DSH_UPDATE=0"
if /i "%~1"=="/update-dsh" set "DSH_UPDATE=1"
:: ---- 参数/update-pi <Pi根> = 更新 PIAgent（版本管理对话框「更新」调用；Pi根=第 2 参
::      带引号，配置的当前根（含 pi\ 子目录）：git pull --ff-only + pnpm install/build
::      （dist 不入库必须重建）+ 同步 exe 侧部署副本（若与根不同目录））----
set "PI_UPDATE=0"
set "PI_UP_ROOT=%~2"
if /i "%~1"=="/update-pi" set "PI_UPDATE=1"
:: ---- 参数/hermes-update <Hermes根> = 更新既有 Hermes 安装（版本管理对话框「更新」
::      调用；Hermes根=第 2 参带引号，配置的当前根，覆盖默认的脚本同级 HermesHarness；
::      既有布局自动 fetch+reset——Node 形态含 pnpm 重建、Python 形态含补丁重敷）----
set "HERMES_UPD=0"
if /i "%~1"=="/hermes-update" set "HERMES_UPD=1"
if /i "%~1"=="/hermes-update" set "HERMES_ONLY=1"
:: ---- 参数/hermes-patch <root> = 给部署根 hermes_cli\oneshot.py 敷设 AutoCode
::      事件补丁（软编码：补丁源=脚本同级 hermes_oneshot_patch.py，venv python 执行；
::      幂等已含标记跳过。程序侧选 Hermes 后端保存时与 StartStage 兜底时自动调用）----
set "HERMES_PATCH=0"
if /i "%~1"=="/hermes-patch" set "HERMES_PATCH=1"

:: ---- 参数：/node = 仅安装/升级 Node.js LTS（后端版本管理「更新」的 Node 预检----
::      闸调用：PATH 无 node 或主版本过低（Claude Code 2.x 要求 22 以上）时拉起）----
set "NODE_ONLY=0"
if /i "%~1"=="/node" set "NODE_ONLY=1"

title AutoCode 环境安装
cd /d "%~dp0"
:: DSH 固定安装布局：脚本目录内（即 Execute 程序目录）DeepSeekHarness\deepseek-harness
::   子目录 - 绿色便携：发布只拷 Execute 目录，DSH 随目录整体带走；程序侧
::   CDeepSeekRunner::ProbeSiblingRoot 同布局探测并自动回填配置。
for %%i in ("%~dp0DeepSeekHarness\deepseek-harness") do set "DSH_ROOT=%%~fi"
:: /update-dsh 模式第 3 参可显式指定 DSH 根（程序侧传配置的 DshRoot，带引号）：
::   兼容非脚本同级的安装布局（如多实例共享外部 DSH 根）；%~3 去引号后整串生效。
if "!DSH_UPDATE!"=="1" if not "%~3"=="" set "DSH_ROOT=%~3"
:: Hermes 联网安装布局锚点：脚本同级 HermesHarness（程序侧 ResolveHermesCli 探测序
::   第 2 位；dist\cli.js 为安装完成判据）。仓库地址 = 第 2 参（程序侧传设置页所填值）。
for %%i in ("%~dp0HermesHarness") do set "HERMES_ROOT=%%~fi"
set "HERMES_URL=%~2"
if "!HERMES_URL!"=="" set "HERMES_URL=https://github.com/nousresearch/hermes-agent"
:: 2026.09.20 /hermes-update 模式：第 2 参是【Hermes 根】而非仓库地址——覆盖默认
::   HERMES_ROOT（兼容外部部署布局，同 /update-dsh 第 3 参口径）。
if "!HERMES_UPD!"=="1" if not "%~2"=="" set "HERMES_ROOT=%~2"

:: ---- 2026.10.10 提权前置：先探管理员并按需自提权，再打印横幅 ----
:: 修复：旧序「先横幅→:ADMIN 里 net session→自提权重跑」会把横幅在原窗口与提权
::       窗口各打印一次（异地部署「CodexHarness 单独安装」横幅出现两遍的根因）；
:: 探测补 fltmc 兜底：net session 依赖 Server/LanmanServer 服务，该服务被禁用的
::       机器上提权后仍探测失败→无限弹 UAC/新窗口；/relaunched 标记防循环重跑，
::       二次仍未提权则降级以当前权限继续（本地目录安装可完成，仅机器级安装受限）。
:: /hermes-patch 纯文件操作无需提权，保持直接进补丁段。
if "!HERMES_PATCH!"=="1" goto :HERMES_PATCH
goto :ELEVATE

:ELEVATE
set "AC_ADMIN=0"
net session >nul 2>&1
if not errorlevel 1 set "AC_ADMIN=1"
fltmc >nul 2>&1
if not errorlevel 1 set "AC_ADMIN=1"
:: 标记检测用纯 cmd 变量替换（不调 find.exe——PATH 混入 Git/Unix 工具目录时
::       find 会被劫持，防循环守卫失效；内置语法任何机器行为一致）。
set "AC_RELAUNCHED=0"
set "AC_ARGS=%*"
if defined AC_ARGS if not "!AC_ARGS!"=="!AC_ARGS:/relaunched=!" set "AC_RELAUNCHED=1"
set "AC_ARGS="
if "!AC_ADMIN!"=="1" goto :MODE_DISPATCH
if "!AC_RELAUNCHED!"=="1" goto :ELEV_DEGRADE
echo [提示] 安装需要管理员权限，正在尝试 UAC 提权...
if "%~1"=="" (
    powershell -NoProfile -Command "Start-Process '%~f0' -ArgumentList '/relaunched' -Verb RunAs"
) else (
    powershell -NoProfile -Command "Start-Process '%~f0' -ArgumentList '%* /relaunched' -Verb RunAs"
)
exit /b

:ELEV_DEGRADE
echo [提示] 未能获得管理员权限（UAC 被拒或系统策略限制），改以当前权限继续...
echo         机器级安装（winget/MSI/npm 全局）可能失败；本地目录安装可正常完成。
echo.
goto :MODE_DISPATCH

:: ---- 模式分派（横幅仅在最终执行的窗口打印一次）----
:MODE_DISPATCH
if "!DSH_UPDATE!"=="1" goto :BANNER_DSH_UPDATE
if "!PI_UPDATE!"=="1" goto :BANNER_PI_UPDATE
if "!HERMES_UPD!"=="1" goto :BANNER_HERMES
if "!DSH_ONLY!"=="1" goto :BANNER_DSH
if "!PI_ONLY!"=="1" goto :BANNER_PI
if "!PI_NPM_ONLY!"=="1" goto :BANNER_PI_NPM
if "!CODEX_ONLY!"=="1" goto :BANNER_CODEX
if "!HERMES_ONLY!"=="1" goto :BANNER_HERMES
if "!NODE_ONLY!"=="1" goto :BANNER_NODE
echo ============================================================
echo   AutoCode 环境安装
echo   一键安装：Git、Node.js LTS、Claude Code CLI
echo   可选安装：DeepSeekHarness - DeepSeek 官方 Agent 后端
echo   可选安装：PIAgent - pi coding-agent（本机源复制安装）
echo   可选接入：GLM 中继工具 - Claude Code 换国产模型
echo ============================================================
echo.
goto :ADMIN

:: ---- /hermes-patch：敷设 oneshot 事件补丁（纯文件操作，无需提权）----
:HERMES_PATCH
set "HP_ROOT=%~2"
if "!HP_ROOT!"=="" set "HP_ROOT=!HERMES_ROOT!"
set "HP_PY=!HP_ROOT!\hermes-agent\venv\Scripts\python.exe"
if not exist "!HP_PY!" set "HP_PY=!HP_ROOT!\venv\Scripts\python.exe"
if not exist "!HP_PY!" set "HP_PY=python"
"!HP_PY!" "%~dp0hermes_oneshot_patch.py" "!HP_ROOT!"
exit /b %ERRORLEVEL%

:BANNER_DSH
echo ============================================================
echo   DeepSeekHarness 单独安装
echo   安装内容：deepseek-harness 仓库 + pnpm 依赖 + AutoCode 组合文件
echo   安装位置：!DSH_ROOT!
echo   前置条件：Git 与 Node.js - 缺失时会自动安装
echo ============================================================
echo.
goto :ADMIN

:BANNER_PI
echo ============================================================
echo   PIAgent 单独安装
echo   安装内容：pi coding-agent 运行时（robocopy 复制本机已构建源）
echo   安装位置：脚本同级 PIAgent\pi
echo   前置条件：Node.js - 缺失时会自动安装
echo   源目录默认：脚本上级 PIAgent（含 pi\ 子目录），可用环境变量 PI_SRC 覆盖
echo ============================================================
echo.
goto :ADMIN

:: ---- PIAgent 联网安装横幅（/pi-npm 模式，2026.10.10）----
:BANNER_PI_NPM
echo ============================================================
echo   PIAgent 联网安装（npm 全局）
echo   安装内容：npm install -g @earendil-works/pi-coding-agent
echo   环境自愈：继承 PATH 旧时自动读注册表 PATH 刷新；缺 Node.js 时自动补装
echo   镜像源：https://registry.npmmirror.com（境内加速）
echo ============================================================
echo.
goto :ADMIN

:BANNER_CODEX
echo ============================================================
echo   CodexHarness 单独安装
echo   安装内容：@openai/codex@0.87.0（npm 本地工程安装，--save-exact 锁定版本）
echo   安装位置：脚本同级 Execute\CodexHarness
echo   版本说明：v0.88+ 已移除 wire_api="chat"，本后端锁定 0.87.0（chat 网关接入）
echo ============================================================
echo.
goto :ADMIN

:: ---- Hermes 联网安装横幅（/hermes 模式）----
:BANNER_HERMES
echo ============================================================
echo   Hermes Harness 联网安装
echo   安装内容：git 克隆仓库 + pnpm 依赖 + 构建 dist\cli.js
echo   安装位置：!HERMES_ROOT!
echo   仓库地址：!HERMES_URL!
echo   前置条件：Git 与 Node.js - 缺失时会自动安装
echo   已装更新：检测到 dist\cli.js 时改为 fetch+reset（保留本地文件）
echo ============================================================
echo.
goto :ADMIN

:: ---- DSH 更新横幅（/update-dsh 模式）----
:BANNER_DSH_UPDATE
echo ============================================================
echo   更新 DeepSeekHarness（fetch+reset 保留本地）
echo   安装位置：!DSH_ROOT!
echo   策略：git fetch 目标 tag + reset + pnpm 重建（node_modules 等本地文件不受影响）
echo ============================================================
echo.
goto :ADMIN


:: ---- Node.js 单独安装/升级横幅（/node 模式，2026.10.10）----
:BANNER_NODE
echo ============================================================
echo   Node.js LTS 安装/升级
echo   触发条件：PATH 无 node 或主版本过低（Claude Code 2.x 要求 22 以上）
echo   安装方式：winget 优先，失败自动转 nodejs.org LTS x64 MSI 直装
echo ============================================================
echo.
goto :ADMIN
:ADMIN

:: ---- 0. 管理员权限：已于 :ELEVATE 前置完成（2026.10.10 提权先于横幅，自提权
::      重跑不再重复打印横幅；net session+fltmc 双探测，此处仅报告状态）----
if "!AC_ADMIN!"=="1" (
    echo [OK] 已获取管理员权限。
    echo.
)

:: ---- 0.1 会话 PATH 刷新：读注册表 Machine+User 合并 PATH（:RefreshPath，会追加
::      回初始 PATH 防丢）。修「上一轮刚装完 Node、本窗口继承的还是旧 PATH」场景：
::      不刷则 :DETECT 误判 node 缺失而重装（2026.10.10，/pi-npm 前置自愈）----
call :RefreshPath

:: ---- 1. 选择 Claude Code 接入后端（单装/更新模式跳过）----
if "!DSH_UPDATE!"=="1" goto :UPDATE_DSH
if "!PI_UPDATE!"=="1" goto :PI_UPDATE
if "!DSH_ONLY!"=="1" goto :DETECT
if "!PI_ONLY!"=="1" goto :DETECT
if "!PI_NPM_ONLY!"=="1" goto :DETECT
if "!HERMES_ONLY!"=="1" goto :DETECT
if "!CODEX_ONLY!"=="1" goto :CODEX
if "!NODE_ONLY!"=="1" goto :NODE
echo ============================================================
echo   请选择 Claude Code 接入模型：
echo     [1] Anthropic 原版 - 官方直连 + Anthropic 账号或 API key
echo     [2] 国产 GLM - 本机推荐 - 内网畅通 - 需智谱 API key
echo ============================================================
choice /c 12 /n /m "请输入（1 或 2）: "
if !errorlevel! equ 2 (
    set "BACKEND=GLM"
) else (
    set "BACKEND=ANTHROPIC"
)
echo.
echo [OK] 已选择 !BACKEND!
echo.

:: ---- 2. 检测已安装项（缺失才装，已装不动）----
:DETECT
set "HAVE_NODE=0"
set "HAVE_GIT=0"
set "HAVE_CLAUDE=0"
set "HAVE_HELPER=0"
where node   >nul 2>&1 && set "HAVE_NODE=1"
where git    >nul 2>&1 && set "HAVE_GIT=1"
where claude >nul 2>&1 && set "HAVE_CLAUDE=1"
rem 2026.10.10 claude 检测双重验证（dell 实害终审：npm prefix 变更后 %APPDATA%\npm 遗留死
rem   shim 使 where 假阳性 → HAVE_CLAUDE=1 → NEED=0 → goto 跳过整个安装段——claude 段
rem   的死 shim 自动修复永远跑不到，重跑多少次都无效）。--version 失败判死 shim，
rem   置回未装走安装段的自动修复分支（清遗留 shim → 重装 → 复验）。
if "!HAVE_CLAUDE!"=="1" (
    call claude --version >nul 2>&1
    if !errorlevel! neq 0 (
        set "HAVE_CLAUDE=0"
        echo    claude   命令存在但无法执行（死 shim，指向的包已不在），将走自动修复
    )
)
if /i "!BACKEND!"=="GLM" ( where chelper >nul 2>&1 && set "HAVE_HELPER=1" )

echo [检测]
if "!HAVE_NODE!"=="1"   (echo    node     已安装) else (echo    node     缺失)
if "!HAVE_GIT!"=="1"    (echo    git      已安装) else (echo    git      缺失)
if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" if "!PI_NPM_ONLY!"=="0" if "!HERMES_ONLY!"=="0" (
    if "!HAVE_CLAUDE!"=="1" (echo    claude   已安装) else (echo    claude   缺失)
    if /i "!BACKEND!"=="GLM" (
        if "!HAVE_HELPER!"=="1" (echo    chelper  已安装) else (echo    chelper  缺失)
    )
)

:: winget 是否可用（有组件缺失时优先用它，不可用走直接下载）。
set "HAVE_WINGET=0"
where winget >nul 2>&1 && set "HAVE_WINGET=1"
if "!HAVE_WINGET!"=="1" (
    echo    winget   可用（优先静默安装）
) else (
    echo    winget   不可用（走直接下载安装）
)
echo.

:: 是否存在需要安装的组件。
set "NEED=0"
if "!HAVE_NODE!"=="0"   set "NEED=1"
if "!HAVE_GIT!"=="0"    set "NEED=1"
:: 2026.10.10 版本闸（全量模式）：claude-code@2.x 引擎要求 Node>=22——旧版残装
::   （如 32 位 0.x）存在但版本过低同样必须升级，否则 npm postinstall 新语法
::   （const { spawnSync } = require(...)）旧 Node 解析必 SyntaxError。
set "NODE_LOW=0"
if "!HAVE_NODE!"=="1" (
    for /f "tokens=1 delims=v." %%v in ('node -v 2^>nul') do if %%v LSS 22 set "NODE_LOW=1"
)
if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" if "!PI_NPM_ONLY!"=="0" if "!HERMES_ONLY!"=="0" if "!NODE_LOW!"=="1" set "NEED=1"
if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" if "!PI_NPM_ONLY!"=="0" if "!HERMES_ONLY!"=="0" (
    if "!HAVE_CLAUDE!"=="0" set "NEED=1"
    if /i "!BACKEND!"=="GLM" if "!HAVE_HELPER!"=="0" set "NEED=1"
)

if "!NEED!"=="0" (
    echo [检测] 所有必要组件已经就绪，无需重装。
    echo.
    if "!DSH_ONLY!"=="1" goto :DSH
    if "!PI_ONLY!"=="1" goto :PI
    if "!PI_NPM_ONLY!"=="1" goto :PI_NPM
    if "!HERMES_ONLY!"=="1" goto :HERMES
    goto :DSH_ASK
)

:: ---- 3. Node.js LTS（DSH 依赖：建议 22.19 以上或 24 以上）----
if "!HAVE_NODE!"=="0" (
    echo [安装] Node.js LTS ...
    set "NODE_OK=0"
    if "!HAVE_WINGET!"=="1" (
        echo   - 尝试 winget（若 winget 源受阻将自动转直接下载）...
        winget install --id OpenJS.NodeJS.LTS -e --source winget --accept-source-agreements --accept-package-agreements --disable-interactivity
        call :RefreshPath
        where node >nul 2>&1 && set "NODE_OK=1"
    )
    if "!NODE_OK!"=="0" (
        if "!HAVE_WINGET!"=="1" ( echo   - winget 未装成，改为直接下载 Node.js LTS 的 MSI... )
        call :InstallNodeDl
        call :RefreshPath
        where node >nul 2>&1 && set "NODE_OK=1"
    )
    if "!NODE_OK!"=="1" ( set "HAVE_NODE=1" ) else ( echo [错误] Node.js 不在 PATH，请关闭窗口后重试。 )
)
:: 2026.10.10 旧版升级闸（全量模式）：node 在但主版本 <22 时升级；单装模式
::   （/dsh /pi /hermes）保持原「存在即跳过」口径，由各自安装段把守。
if "!HAVE_NODE!"=="1" if "!NODE_LOW!"=="1" (
    if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" if "!PI_NPM_ONLY!"=="0" if "!HERMES_ONLY!"=="0" (
        call :EnsureNodeLts
        call :NodeVerMajor
        if !NODE_MAJOR_NUM! GEQ 22 (
            set "NODE_LOW=0"
        ) else (
            echo [错误] Node.js 升级未达标，继续安装 Claude Code 必失败，本次中止。
            echo        请按上方指引卸载旧版 Node.js 后重跑本脚本。
            goto :END
        )
    )
)
echo.

:: ---- 4. Git ----
if "!HAVE_GIT!"=="0" (
    echo [安装] Git ...
    set "GIT_OK=0"
    if "!HAVE_WINGET!"=="1" (
        echo   - 尝试 winget（若 winget 源失败将自动转直接下载）...
        winget install --id Git.Git -e --source winget --accept-source-agreements --accept-package-agreements --disable-interactivity
        call :RefreshPath
        where git >nul 2>&1 && set "GIT_OK=1"
    )
    if "!GIT_OK!"=="0" (
        if "!HAVE_WINGET!"=="1" ( echo   - winget 未装成，改为直接下载 Git 的 64-bit 安装包... )
        call :InstallGitDl
        call :RefreshPath
        where git >nul 2>&1 && set "GIT_OK=1"
    )
    if "!GIT_OK!"=="1" ( set "HAVE_GIT=1" ) else ( echo [错误] Git 不在 PATH，请关闭窗口后重试。 )
)
echo.

:: ---- 5. 确认 npm 可用（claude/chelper/pnpm 全走 npm）----
call :RefreshPath
where npm >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 找不到 npm（Node 未装成或 PATH 未刷新），请关闭本窗口后重开终端重试。
    goto :END
)
echo [OK] npm 可用。
echo.

:: ---- 6. 需要用 npm 安装的（claude / chelper / pnpm）统一 npm 全局目录 ----
set "DO_NPMCFG=0"
if "!DSH_ONLY!"=="1" set "DO_NPMCFG=1"
if "!HERMES_ONLY!"=="1" set "DO_NPMCFG=1"
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

:: ---- 7. npm 镜像源（境内加速，pnpm 同样读取该配置）----
echo [配置] npm 镜像源 = https://registry.npmmirror.com
call npm config set registry https://registry.npmmirror.com
echo.

:: DSH 单装模式：到此环境就绪，直接进安装段。
if "!DSH_ONLY!"=="1" goto :DSH
:: Hermes 联网安装模式：同 DSH（node+git+pnpm 就绪后进安装段）。
if "!HERMES_ONLY!"=="1" goto :HERMES
:: PI 单装模式：node 就绪即可（无需 git/npm 全局件），直接进安装段。
if "!PI_ONLY!"=="1" goto :PI
:: PI 联网安装模式：环境段就绪（PATH 已刷新/Node 已备/镜像已配），进安装段。
if "!PI_NPM_ONLY!"=="1" goto :PI_NPM

:: ---- 8. Claude Code CLI（claude 命令）----
:: 2026.10.10 验收加固（异地部署机实害：npm 装失败静默继续、跳过分支不验 shim
::   有效性——claude.cmd 存在但包损坏时误报已装）。装后 where claude 复查验收；
::   已装分支补 claude --version 快验（shim 指向的包被删/损坏可及时发现）。
:: 2026.10.10 prefix 受保护目录检测（dell 实害：prefix=C:\Program Files\npm，
::   npm install -g 写受保护目录必须管理员，未提权/降级运行必 EACCES 失败死循环）。
rem 2026.10.10 关键步骤落盘（异地支持：跑完把本目录 install_env_last.log 反馈开发者）
set "IEL=%~dp0install_env_last.log"
for /f "delims=" %%p in ('npm config get prefix 2^>nul') do set "NPM_PREFIX_X=%%p"
if exist "%CD%\.npmrc" (
    echo [%date% %time%] [重要] 工作目录存在项目级 .npmrc（优先级高于用户级，会顶掉 prefix 纠正）：>> "!IEL!"
    type "%CD%\.npmrc" >> "!IEL!" 2>nul
    echo. >> "!IEL!"
    echo         项目级 .npmrc 内容：
    type "%CD%\.npmrc" 2>nul
)
echo [%date% %time%] prefix=%NPM_PREFIX_X% >> "!IEL!"
echo %NPM_PREFIX_X% | findstr /i /c:"Program Files" >nul 2>&1
if not errorlevel 1 (
    echo [警告] npm 全局目录指向受保护目录（!NPM_PREFIX_X!）——npm install 写入需要
    echo         管理员权限，安装常因权限静默失败（dell 机实害循环根因）。
    choice /c YN /t 15 /d Y /m "自动改回用户目录（%APPDATA%\npm，推荐，15 秒后自动 Y）"
    if !errorlevel! equ 1 (
        call npm config set prefix "%APPDATA%\npm"
        echo [OK] npm 全局目录已改回 %APPDATA%\npm（写入无需管理员，装完命令直接可用）
        echo [%date% %time%] prefix 自动纠正 Y 已执行 >> "!IEL!"
    ) else (
        echo [跳过] 保持 !NPM_PREFIX_X!（若安装失败请提权重跑或手动改回）
    )
)

where claude >nul 2>&1
if !errorlevel! neq 0 (
    echo [安装] @anthropic-ai/claude-code ...
    call npm install -g @anthropic-ai/claude-code
    echo [%date% %time%] 安装分支 npm install rc=!errorlevel! >> "!IEL!"
    call :RefreshPath
    where claude >nul 2>&1
    if !errorlevel! neq 0 (
        echo [错误] Claude Code 安装失败（npm 报错见上方）。常见原因：网络不通、
        echo [%date% %time%] 结果=错误-安装失败 >> "!IEL!"
        echo         npm 源不可达、Node 版本过低。处理后重跑本脚本。
    ) else (
        call claude --version >nul 2>&1
        if !errorlevel! equ 0 (
            echo [OK] Claude Code 安装完成。
        echo [%date% %time%] 结果=OK-安装完成 >> "!IEL!"
        ) else (
            echo [错误] claude 命令已落盘但无法执行（包不完整，--version 失败）。
            echo         处理：npm install -g @anthropic-ai/claude-code 重装。
        )
    )
) else (
    call claude --version >nul 2>&1
    if !errorlevel! equ 0 (
        echo [OK] Claude Code 已安装，跳过。
        echo [%date% %time%] 结果=OK-已装跳过 >> "!IEL!"
    ) else (
        rem 2026.10.10 死 shim 自动修复（dell 实害：npm prefix 变更后 %APPDATA%
pm 遗留
        rem   死 shim 挡在 PATH 前——where 永远命中坏的，重装到新 prefix 也救不回）。
        rem   处理：删遗留 shim（claude/claude.cmd/claude.ps1，仅同目录 node_modules 下
        rem   无包真身时）→ 重装 → 复验。npm prefix 非默认时装到 prefix（可能需本窗口
        rem   的管理员权限），装完 where 命中新位置。
        echo [修复] claude 命令存在但无法执行（死 shim，指向的包已不在）——
        echo         自动清理遗留 shim 并重装...
        if not exist "%APPDATA%\npm\node_modules\@anthropic-ai\claude-code\bin\claude.exe" (
            del "%APPDATA%\npm\claude" "%APPDATA%\npm\claude.cmd" "%APPDATA%\npm\claude.ps1" 2>nul
        )

        call npm install -g @anthropic-ai/claude-code
        echo [%date% %time%] 死shim修复 npm install rc=!errorlevel! >> "!IEL!"
        call :RefreshPath
        call claude --version >nul 2>&1
        if !errorlevel! equ 0 (
            echo [OK] Claude Code 重装完成（已自动清理失效的旧 shim）。
        echo [%date% %time%] 结果=OK-死shim修复重装完成 >> "!IEL!"
        ) else (
            rem 重装成功但 PATH 仍解析不到：npm prefix 目录不在 PATH（prefix 变更常见后遗症），
            rem   用 npm root -g 探真身并给 PATH 指引（dell 实害：prefix=C:\Program Files\npm）。
            for /f "delims=" %%p in ('npm root -g 2^>nul') do set "NPM_ROOT_G=%%p"
            if exist "!NPM_ROOT_G!\@anthropic-ai\claude-code\bin\claude.exe" (
                echo [OK] Claude Code 已装到 npm 全局目录（!NPM_ROOT_G!）。
        echo [%date% %time%] 结果=OK-真身在但PATH缺 >> "!IEL!"
                echo [提示] 但该目录不在 PATH，claude 命令无法直接命中——把 npm prefix
                echo         所在目录（运行 npm config get prefix 查看）加入系统 PATH 后重开 cmd。
            ) else (
                echo [错误] 重装后仍未找到 claude 安装。请把本窗口全部内容反馈给开发者。
        echo [%date% %time%] 结果=错误-重装后仍无安装 >> "!IEL!"
            )
        )
    )
)
echo.

:: ---- 9. 国产 GLM 中继（选了 GLM 才装）----
if /i "!BACKEND!"=="GLM" (
    where chelper >nul 2>&1
    if !errorlevel! neq 0 (
        echo [安装] @z_ai/coding-helper ...
        call npm install -g @z_ai/coding-helper
        call :RefreshPath
        where chelper >nul 2>&1
        if !errorlevel! equ 0 (
            echo [OK] 国产中继 chelper 安装完成。
        ) else (
            echo [错误] chelper 安装失败（npm 报错见上方）。处理后重跑本脚本。
        )
    ) else (
        echo [OK] 国产中继 chelper 已安装，跳过。
    )
    echo.
)

:: ---- 9.5 可选：安装 DeepSeekHarness（Agent 备选后端，问询一次）----
:DSH_ASK
if exist "!DSH_ROOT!\packages\examples\jsonrpc-demo\lib\bin.js" (
    echo [OK] DeepSeekHarness 已安装：!DSH_ROOT!
    echo.
    goto :VERIFY
)
echo ============================================================
echo   可选组件：DeepSeekHarness - DeepSeek 官方 Agent 后端
echo   安装位置：!DSH_ROOT!
echo   需联网克隆 GitHub 仓库并下载 pnpm 依赖，耗时数分钟。
echo   不装也不影响 Claude Code 后端使用；之后可在 AutoCode
echo   设置页 DeepSeekHarness 参数设置 - 安装 DeepSeekHarness 随时补装。
echo ============================================================
choice /c YN /n /m "立即安装请按 Y，跳过请按 N："
if !errorlevel! equ 2 (
    echo [跳过] 未安装 DeepSeekHarness。
    echo.
    goto :VERIFY
)
echo.

:: ---- DeepSeekHarness 安装段（克隆 + pnpm 依赖构建 + 部署 AutoCode 组合文件）----
:DSH
:: 2026.09.13 代理支持：set AC_GIT_PROXY=http://127.0.0.1:7897 后 git 走单次代用。
set "GIT_PROXY_ARGS="
if defined AC_GIT_PROXY (
    set "GIT_PROXY_ARGS=-c http.proxy=!AC_GIT_PROXY! -c https.proxy=!AC_GIT_PROXY!"
    echo [代理] git 走 !AC_GIT_PROXY!（环境变量 AC_GIT_PROXY）
)
echo ============================================================
echo   安装 DeepSeekHarness
echo ============================================================
:: 前置：node / git 必须可用（前面块已尽力自动安装）。
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - DeepSeekHarness 需要 Node 22.19 以上或 24 以上，请先装好再重试。
    goto :DSH_END
)
where git >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Git - 克隆仓库需要 Git，请先装好再重试。
    goto :DSH_END
)
if exist "!DSH_ROOT!\apps\cli\lib\bin.js" (
    echo [OK] 检测到已安装（官方 SDK profile 布局）：!DSH_ROOT!
    echo        仅检查 AutoCode 组合层文件后结束。
    echo.
    goto :DSH_CORDIS
)
if exist "!DSH_ROOT!\packages\examples\jsonrpc-demo\lib\bin.js" (
    echo [OK] 检测到已安装：!DSH_ROOT!
    echo        仅更新 AutoCode 组合文件后结束。
    echo.
    goto :DSH_CORDIS
)
if exist "!DSH_ROOT!" (
    echo [提示] 目录已存在但未完成构建，继续依赖安装与构建...
) else (
    echo [安装] 克隆 deepseek-harness 仓库 - 浅克隆，约几十 MB ...
    git !GIT_PROXY_ARGS! clone --depth 1 https://github.com/deepseek-ai/deepseek-harness.git "!DSH_ROOT!"
    if !errorlevel! neq 0 (
        if exist "!DSH_ROOT!" rd /s /q "!DSH_ROOT!"
        echo [错误] 克隆失败 - 残留目录已清理，可安全重跑。
        echo        GitHub 访问不通时先设代理再重跑：set AC_GIT_PROXY=http://127.0.0.1:7897
        goto :DSH_END
    )
)
echo [安装] pnpm 包管理器 pnpm@11.7.0 ...
call npm install -g pnpm@11.7.0
call :RefreshPath
where pnpm >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] pnpm 安装失败 - 请关闭窗口重试，或手动执行 npm install -g pnpm@11.7.0
    goto :DSH_END
)
pushd "!DSH_ROOT!"
call :JunctionPrecheck
if !errorlevel! neq 0 (
    popd
    goto :DSH_END
)
echo [安装] pnpm 依赖 - 数分钟，请耐心等待...
call pnpm install
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm install 失败 - 请检查网络代理后重跑本脚本。
    goto :DSH_END
)
echo [构建] 完整构建（0.1.5 起 build:lib 依赖 native 先行，须整体跑；约 5~15 分钟）...
call pnpm run build
if !errorlevel! neq 0 (
    popd
    echo [错误] build 失败 - 请查看上方输出排查 Node 版本（建议 22.19 以上或 24 以上）。
    goto :DSH_END
)
popd
echo.

:DSH_CORDIS
:: 部署 AutoCode 组合层（正本在 AutoCode 仓库 agent/dsh，构建时同步到脚本同级 dsh\）。
:: 2026.09.10 双布局：新版（>=0.1.2，apps\cli 入口）组合层 = dsh\sdk.autocode.patch.yml
::   （启动时 --profile sdk --patch 绝对路径直接引用，无需部署进 DSH 仓库）；
::   旧版（<=0.1.1）仍部署 cordis.yml 到 examples 树（相对 node_modules 解析要求）。
if exist "!DSH_ROOT!\apps\cli\lib\bin.js" (
    if exist "%~dp0dsh\sdk.autocode.patch.yml" (
        echo [OK] 官方 SDK profile 布局：组合层 sdk.autocode.patch.yml 就位（--patch 直接引用）。
    ) else (
        echo [警告] 未找到 %~dp0dsh\sdk.autocode.patch.yml - 请确认 AutoCode 程序目录完整。
    )
    echo [OK] DeepSeekHarness 安装完成：!DSH_ROOT!
    echo        AutoCode 启动时会自动探测本目录并回填配置；
    echo        设置页也可点「重新检测」立即生效。
    echo.
    goto :DSH_END
)
if not exist "!DSH_ROOT!\examples\autocode-agent" mkdir "!DSH_ROOT!\examples\autocode-agent"
if exist "%~dp0dsh\cordis.autocode.yml" (
    copy /y "%~dp0dsh\cordis.autocode.yml" "!DSH_ROOT!\examples\autocode-agent\cordis.yml" >nul
    echo [OK] 已部署 AutoCode 组合文件 cordis.yml。
) else (
    echo [警告] 未找到 %~dp0dsh\cordis.autocode.yml - 请确认 AutoCode 程序目录完整。
)
if exist "!DSH_ROOT!\packages\examples\jsonrpc-demo\lib\bin.js" (
    echo [OK] DeepSeekHarness 安装完成：!DSH_ROOT!
    echo        AutoCode 启动时会自动探测本目录并回填配置；
    echo        设置页也可点「重新检测」立即生效。
) else (
    echo [错误] 未找到 bin.js - 安装未完成，请查看上方输出排查。
)
echo.

:DSH_END
goto :VERIFY

:: ---- CodexHarness 单独安装段（/codex：npm 本地工程安装到脚本同级 Execute\CodexHarness）----
:CODEX
echo ============================================================
echo   安装 CodexHarness（OpenAI Codex CLI 0.87.0）
echo ============================================================
:: 前置：node 需可用（版本 20 以上；未装时先用全量模式装 Node 再重跑本模式）。
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - Codex CLI 需要 Node 运行时。请先运行全量安装（装 Node）后重跑 install_env.bat /codex
    goto :CODEX_END
)
set "NODE_MAJOR=0"
for /f "tokens=1 delims=v." %%v in ('node -v') do set "NODE_MAJOR=%%v"
if !NODE_MAJOR! LSS 20 (
    echo [错误] Node 版本过低（主版本 !NODE_MAJOR!，需 20 以上）- 请升级 Node.js 后重跑。
    goto :CODEX_END
)
for %%i in ("%~dp0Execute\CodexHarness") do set "CODEX_DST=%%~fi"
if not exist "!CODEX_DST!" mkdir "!CODEX_DST!"
cd /d "!CODEX_DST!"
if not exist package.json (
    echo {"name":"autocode-codexharness","version":"1.0.0"} > package.json
)
:: ---- 2026.08.21 本机源离线兜底：脚本上级有 CodexHarness 安装源（同级约定，同 PIAgent）
::      时 robocopy 直接复制，跳过 npm（避免联网慢/卡死）；robocopy 失败再走 npm。
if not defined CODEX_SRC set "CODEX_SRC=%~dp0..\CodexHarness"
for %%i in ("!CODEX_SRC!") do set "CODEX_SRC=%%~fi"
if exist "!CODEX_SRC!\node_modules\@openai\codex\package.json" (
    echo [安装] 检测到本机源，robocopy 离线安装（源：!CODEX_SRC!）...
    robocopy "!CODEX_SRC!" "!CODEX_DST!" /E /NFL /NDL /NJH /NJS /NP /R:2 /W:2
    if !errorlevel! GEQ 8 (
        echo [警告] robocopy 失败（退出码 !errorlevel!）- 回退 npm 联网安装。
    ) else (
        goto :CODEX_VERIFY
    )
)
echo [安装] npm install @openai/codex@0.87.0 --save-exact ...
call npm install @openai/codex@0.87.0 --save-exact
if !errorlevel! neq 0 (
    cd /d "%~dp0"
    echo [错误] npm install 失败 - 查看上方输出（网络/npm 源问题），处理后重跑可续装。
    goto :CODEX_END
)
echo [验证] node node_modules\@openai\codex\bin\codex.js --version ...
call node node_modules\@openai\codex\bin\codex.js --version
if !errorlevel! neq 0 (
    cd /d "%~dp0"
    echo [错误] codex.js --version 验证失败 - 安装未完成，请查看上方输出。
    goto :CODEX_END
)
cd /d "%~dp0"
echo [OK] CodexHarness 安装完成（!CODEX_DST!）
echo        AutoCode 启动时会自动探测本目录并自动配置；
echo        设置页也可点「重新检测」立即生效。
echo.

:CODEX_END
goto :VERIFY

:: ---- Hermes Harness 联网安装段（/hermes <repoUrl>：git 克隆 + pnpm 依赖构建）----
:HERMES
echo ============================================================
echo   联网安装 Hermes Harness
echo   安装位置：!HERMES_ROOT!
echo   仓库地址：!HERMES_URL!
echo ============================================================
if "!HERMES_URL!"=="" (
    echo [错误] 缺少仓库地址参数 - 用法：install_env.bat /hermes ^<git仓库URL^>
    echo         请回 AutoCode 设置页 Hermes 页填写「仓库地址」后重试。
    goto :HERMES_END
)
where git >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Git - 克隆仓库需要 Git，请先安装后重试。
    goto :HERMES_END
)
:: 2026.09.13 代理支持：GitHub 直连不通时，先 set AC_GIT_PROXY=http://127.0.0.1:7897
::   再重跑本脚本 - git 以 -c http/https.proxy 单次代用，不污染全局 .gitconfig。
set "GIT_PROXY_ARGS="
if defined AC_GIT_PROXY (
    set "GIT_PROXY_ARGS=-c http.proxy=!AC_GIT_PROXY! -c https.proxy=!AC_GIT_PROXY!"
    echo [代理] git 走 !AC_GIT_PROXY!（环境变量 AC_GIT_PROXY）
)
set "PIP_PROXY_ARGS="
if defined AC_PIP_PROXY (
    set "PIP_PROXY_ARGS=--proxy !AC_PIP_PROXY!"
    echo [代理] pip 走 !AC_PIP_PROXY!（环境变量 AC_PIP_PROXY）
)

:: ---- 布局判定（双形态，探测序同 CHermesRunner::ResolveHermesCli）----
::   形态 A（Node Harness）    ：!HERMES_ROOT!\dist\cli.js
::   形态 B（Python hermes-agent，官方 install.ps1 布局）：
::     !HERMES_ROOT!\hermes-agent\{仓库+venv\Scripts\hermes.exe}
::   形态 B 平铺（2026.09 旧装）：!HERMES_ROOT!\{仓库+venv\Scripts\hermes.exe}
set "HFORM="
set "HREPO="
if exist "!HERMES_ROOT!\dist\cli.js" (
    set "HFORM=A"
    set "HREPO=!HERMES_ROOT!"
) else if exist "!HERMES_ROOT!\hermes-agent\run_agent.py" (
    set "HFORM=B"
    set "HREPO=!HERMES_ROOT!\hermes-agent"
) else if exist "!HERMES_ROOT!\run_agent.py" (
    set "HFORM=BFLAT"
    set "HREPO=!HERMES_ROOT!"
) else if exist "!HERMES_ROOT!\package.json" (
    set "HFORM=AHALF"
    set "HREPO=!HERMES_ROOT!"
) else (
    set "HFORM=NEW"
)

if "!HERMES_UPD!"=="1" if "!HFORM!"=="NEW" (
    echo [错误] 该路径不是有效的 Hermes 安装根（未检出 dist\cli.js /
    echo        hermes-agentun_agent.py）：
    echo        !HERMES_ROOT!
    echo        请到设置页 Hermes 页确认「根目录」配置后重试。
    goto :HERMES_END
)
if "!HFORM!"=="NEW" (
    echo [安装] 浅克隆 Hermes 仓库 - 需联网，约几十 MB ...
    git !GIT_PROXY_ARGS! clone --depth 1 "!HERMES_URL!" "!HERMES_ROOT!\hermes-agent"
    if !errorlevel! neq 0 (
        if exist "!HERMES_ROOT!\hermes-agent" rd /s /q "!HERMES_ROOT!\hermes-agent"
        echo [错误] 克隆失败 - 残留目录已清理，可安全重跑。
        echo        GitHub 访问不通时先设代理再重跑：set AC_GIT_PROXY=http://127.0.0.1:7897
        goto :HERMES_END
    )
    if exist "!HERMES_ROOT!\hermes-agent\pyproject.toml" (
        set "HFORM=B"
        set "HREPO=!HERMES_ROOT!\hermes-agent"
    ) else (
        rem Node 仓库：上移到根，保持形态 A 布局（!HERMES_ROOT!\dist\cli.js）
        echo [布局] Node 形态仓库 - 内容上移到安装根 ...
        robocopy "!HERMES_ROOT!\hermes-agent" "!HERMES_ROOT!" /E /MOVE /NFL /NDL /NJH /NJS /NP /R:2 /W:2 >nul
        rd /s /q "!HERMES_ROOT!\hermes-agent" 2>nul
        set "HFORM=AHALF"
        set "HREPO=!HERMES_ROOT!"
    )
)

:: ---- Python 形态（B / BFLAT）：venv + 依赖 + 补丁 + 环境变量 ----
if "!HFORM!"=="B" goto :HERMES_PY
if "!HFORM!"=="BFLAT" goto :HERMES_PY

:: ---- Node 形态（A=更新 / AHALF=续装）：pnpm 依赖与构建 ----
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - 请先运行完整安装（或单独装 Node 22.19+）后重试。
    goto :HERMES_END
)
if "!HFORM!"=="A" (
    echo [更新] 检测到既有安装，改为 git 拉取更新（fetch+reset，保留本地文件）...
    pushd "!HREPO!"
    git !GIT_PROXY_ARGS! fetch --all --tags
    if !errorlevel! neq 0 (
        popd
        echo [错误] git fetch 失败 - 请检查网络/代理后重跑（fetch 失败不影响既有安装）。
        goto :HERMES_END
    )
    git reset --hard origin/HEAD
    if !errorlevel! neq 0 (
        popd
        echo [错误] git reset 失败 - 请查看上方输出排查。
        goto :HERMES_END
    )
) else (
    echo [提示] 目录已存在但未完成构建，继续依赖安装与构建...
    pushd "!HREPO!"
)
call :JunctionPrecheck
if !errorlevel! neq 0 (
    popd
    goto :HERMES_END
)
echo [安装] pnpm 包管理器 pnpm@11.7.0 ...
call npm install -g pnpm@11.7.0
call :RefreshPath
where pnpm >nul 2>&1
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm 安装失败 - 请关闭窗口重试，或手动执行 npm install -g pnpm@11.7.0
    goto :HERMES_END
)
echo [安装] pnpm 依赖 - 数分钟，请耐心等待...
call pnpm install
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm install 失败 - 请检查网络代理后重跑本脚本（可续装）。
    goto :HERMES_END
)
findstr /c:"\"build":" package.json >nul 2>&1
if !errorlevel! equ 0 (
    echo [构建] 构建 dist - 仓库声明了 build 脚本，执行构建...
    call pnpm run build
    if !errorlevel! neq 0 (
        if exist "!HERMES_ROOT!\dist\cli.js" (
            echo [提示] build 脚本失败但 dist\cli.js 已存在，视为完成。
        ) else (
            popd
            echo [错误] 构建失败且缺少 dist\cli.js - 请查看上方输出排查 Node 版本（建议 22.19+）。
            goto :HERMES_END
        )
    )
) else (
    echo [提示] 该仓库 package.json 无 build 脚本，跳过构建步骤。
    ver >nul
)
popd
echo.
if exist "!HERMES_ROOT!\dist\cli.js" (
    echo [OK] Hermes 安装完成：!HERMES_ROOT!
    echo        回到 AutoCode 设置页 Hermes 页点「重新检测」即可生效。
) else (
    echo [错误] 未找到 dist\cli.js - 安装未完成，请查看上方输出排查。
)
goto :HERMES_END

:HERMES_PY
:: Python 主项目（NousResearch/hermes-agent 等）：官方布局 = 仓库内 venv。
echo [信息] Python 形态 Hermes（仓库：!HREPO!）
if exist "!HREPO!\venv\Scripts\hermes.exe" (
    echo [更新] 检测到既有 venv 安装，git 拉取更新（fetch+reset）...
    pushd "!HREPO!"
    git !GIT_PROXY_ARGS! fetch --all --tags
    if !errorlevel! neq 0 (
        popd
        echo [错误] git fetch 失败 - 请检查网络/代理后重跑（不影响既有安装）。
        goto :HERMES_END
    )
    git reset --hard origin/HEAD
    popd
    goto :HERMES_PY_FINISH
)
:: 新装：定位 Python 3.11 ~ 3.13（py 启动器优先，回退 python；商店假 python 被版本检查拦下）
set "HPY="
py -3 -c "import sys;sys.exit(0 if (3,11)<=sys.version_info[:2]<(3,14) else 1)" >nul 2>&1
if !errorlevel! equ 0 (
    set "HPY=py -3"
) else (
    python -c "import sys;sys.exit(0 if (3,11)<=sys.version_info[:2]<(3,14) else 1)" >nul 2>&1
    if !errorlevel! equ 0 set "HPY=python"
)
if "!HPY!"=="" (
    echo [错误] 未找到 Python 3.11~3.13 - hermes-agent 官方要求，请安装后重跑：
    echo        https://www.python.org/downloads/ （安装时勾选 Add python.exe to PATH 与 py launcher）
    goto :HERMES_END
)
echo [安装] 创建 venv ...
pushd "!HREPO!"
!HPY! -m venv venv
if !errorlevel! neq 0 (
    popd
    echo [错误] venv 创建失败 - 请查看上方输出。
    goto :HERMES_END
)
echo [安装] pip install -e ".[all]" - 数分钟，请耐心等待...
venv\Scripts\python.exe -m pip install !PIP_PROXY_ARGS! -e ".[all]"
if !errorlevel! neq 0 (
    popd
    echo [错误] 依赖安装失败 - 可重跑本脚本续装；代理环境先 set AC_PIP_PROXY=http://127.0.0.1:7897
    goto :HERMES_END
)
popd

:HERMES_PY_FINISH
if not exist "!HREPO!\venv\Scripts\hermes.exe" (
    echo [错误] venv 内缺少 hermes.exe - 安装未完成，请查看上方输出。
    goto :HERMES_END
)
echo [验证] hermes --version ...
"!HREPO!\venv\Scripts\hermes.exe" --version
echo [补丁] 敷设 AutoCode 事件补丁（hermes_oneshot_patch.py，幂等）...
"!HREPO!\venv\Scripts\python.exe" "%~dp0hermes_oneshot_patch.py" "!HERMES_ROOT!"
if !errorlevel! neq 0 (
    echo [警告] 补丁敷设失败（多为上游改版锚点失配，见上方原因行）- 后端仍可运行，
    echo        仅影响中间过程/用量事件输出；升级 AutoCode 后可重跑 /hermes-patch 重试。
)
setx HERMES_HOME "!HERMES_ROOT!" >nul 2>&1
echo [OK] Hermes 安装完成（Python 形态）：!HREPO!
echo        已写入用户环境变量 HERMES_HOME；回 AutoCode 设置页点「重新检测」即可生效。

:HERMES_END
goto :VERIFY
goto :VERIFY

:: ---- PIAgent 单独安装段（2026.08.19 第三后端；robocopy 本机源 → 脚本同级 PIAgent\pi）----
:PI
echo ============================================================
echo   安装 PIAgent（pi coding-agent）
echo ============================================================
:: 前置：node 必须可用（前面块已尽力自动安装）。
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - PIAgent 需要 Node 运行时，请先装好再重试。
    goto :PI_END
)
:: 源：默认脚本上级 PIAgent（发布目录旁的源码仓库），环境变量 PI_SRC 可覆盖。
::   PI_SRC 兼容两种布局（2026.10.10）：PIAgent 仓库根（含 pi\ 子目录），或直接
::   指向 pi 仓库根（本身含 packages\coding-agent\dist\cli.js）。
if not defined PI_SRC set "PI_SRC=%~dp0..\PIAgent"
for %%i in ("!PI_SRC!") do set "PI_SRC=%%~fi"
for %%i in ("%~dp0PIAgent") do set "PI_DST=%%~fi"
set "PI_SRC_PI=!PI_SRC!\pi"
if exist "!PI_SRC_PI!\packages\coding-agent\dist\cli.js" goto :PI_SRC_READY
set "PI_SRC_PI=!PI_SRC!"
if exist "!PI_SRC_PI!\packages\coding-agent\dist\cli.js" goto :PI_SRC_READY
echo [错误] 源目录无效：!PI_SRC!\pi 下未找到 packages\coding-agent\dist\cli.js
echo        可用环境变量 PI_SRC 指定 PIAgent 仓库根（含 pi\ 子目录）后重跑：
echo          set PI_SRC=D:\path\to\PIAgent ^&^& install_env.bat /pi
echo        本机无源码时可改走联网安装（需能访问 npm 源）：
echo          install_env.bat /pi-npm
goto :PI_END
:PI_SRC_READY
echo [安装] robocopy 复制 pi 运行时（源：!PI_SRC_PI! → 目标：!PI_DST!\pi）...
if not exist "!PI_DST!" mkdir "!PI_DST!"
robocopy "!PI_SRC_PI!" "!PI_DST!\pi" /E /NFL /NDL /NJH /NJS /NP /R:2 /W:2
if !errorlevel! GEQ 8 (
    echo [错误] robocopy 失败（退出码 !errorlevel!）- 请查看上方输出排查（磁盘空间/占用/权限）。
    goto :PI_END
)
if exist "!PI_DST!\pi\packages\coding-agent\dist\cli.js" (
    echo [OK] PIAgent 安装完成：!PI_DST!
    echo        AutoCode 启动时会自动探测本目录并回填配置；
    echo        设置页也可点「重新检测」立即生效。
) else (
    echo [错误] 未找到 cli.js - 安装未完成，请查看上方输出排查。
)
echo.

:PI_END
goto :VERIFY

:: ---- PIAgent 联网安装段（/pi-npm 单装模式，2026.10.10 新增：设置页「联网安装
::      （npm）」按钮改走本模式——旧直连 cmd /k npm install -g 继承的是 AutoCode
::      进程启动时的环境（刚装完 Node 未重启主程序时为旧 PATH）必报找不到 npm；
::      本模式经公共段就绪：:ADMIN 已 :RefreshPath 读注册表 PATH、:DETECT 缺
::      Node 时自动补装、镜像源已配置，再执行全局安装）----
:PI_NPM
echo ============================================================
echo   联网安装 PIAgent（npm install -g @earendil-works/pi-coding-agent）
echo ============================================================
where npm >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 找不到 npm - 请关闭本窗口，重开终端后重试（或先跑环境安装）。
    goto :PI_NPM_END
)
echo [安装] npm install -g @earendil-works/pi-coding-agent ...
call npm install -g @earendil-works/pi-coding-agent
call :RefreshPath
for /f "delims=" %%p in ('npm root -g 2^>nul') do set "NPM_ROOT_G=%%p"
if exist "!NPM_ROOT_G!\@earendil-works\pi-coding-agent\dist\cli.js" (
    echo [OK] PIAgent 联网安装完成（npm 全局：!NPM_ROOT_G!）。
    echo        回到 AutoCode 设置页点「重新检测」即可生效（PIAgent 根目录可留空）。
) else (
    echo [错误] 未找到 dist\cli.js - 安装未完成，请查看上方 npm 输出
    echo        （npm 源可达性/写权限/磁盘空间）排查。
)
echo.

:PI_NPM_END
goto :VERIFY

:: ---- /update-pi 横幅（2026.09.20 版本管理「更新 PIAgent」）----
:BANNER_PI_UPDATE
echo ============================================================
echo   更新 PIAgent（git pull + pnpm install/build）
echo   更新根：!PI_UP_ROOT!
echo   策略：git pull --ff-only（保留本地提交）
echo         → pnpm 重建 dist（dist 不入库，必须重建）
echo         → 同步 exe 侧部署副本（若与更新根不同目录）
echo ============================================================
echo.
goto :ADMIN

:: ---- /update-pi 更新段：按配置的 Pi 根自动更新（用户需求：旧 PI_SRC 兜底
::      指向 exe 上级 PIAgent，外部源仓（如 QT\PIAgent）必报「源目录无效」）----
:PI_UPDATE
echo ============================================================
echo   更新 PIAgent（根：!PI_UP_ROOT!）
echo ============================================================
if "!PI_UP_ROOT!"=="" (
    echo [错误] 缺少 Pi 根参数。用法：install_env.bat /update-pi ^<Pi根^>
    goto :PI_UPDATE_END
)
where git >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Git - 更新需要 Git，请先安装后重试。
    goto :PI_UPDATE_END
)
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - 构建需要 Node 运行时，请先安装后重试。
    goto :PI_UPDATE_END
)
set "PI_UP_PI=!PI_UP_ROOT!\pi"
if not exist "!PI_UP_PI!\package.json" (
    echo [错误] 路径不是有效的 PIAgent 根（缺 pi\package.json）：!PI_UP_ROOT!
    echo        请到设置页 PIAgent 页确认「根目录」配置后重试。
    goto :PI_UPDATE_END
)
set "PI_UP_REPO="
if exist "!PI_UP_PI!\.git" set "PI_UP_REPO=!PI_UP_PI!"
if "!PI_UP_REPO!"=="" if exist "!PI_UP_ROOT!\.git" set "PI_UP_REPO=!PI_UP_ROOT!"
if "!PI_UP_REPO!"=="" (
    echo [错误] 未找到 git 仓库（!PI_UP_ROOT! 与其下 pi\ 均无 .git）。
    echo        该根可能是纯拷贝：请在源仓更新后用「安装 PIAgent…（本机源）」重部署。
    goto :PI_UPDATE_END
)
echo [更新] git pull --ff-only（保留本地提交；分叉/冲突会失败并提示）...
pushd "!PI_UP_REPO!"
git pull --ff-only
if !errorlevel! neq 0 (
    popd
    echo [错误] git pull 失败 - 请到 !PI_UP_REPO! 手动处理（git status 查看分叉/
    echo        冲突，提交或 rebase 后）重跑本更新。
    goto :PI_UPDATE_END
)
popd
where pnpm >nul 2>&1
if !errorlevel! neq 0 (
    echo [安装] pnpm 包管理器 pnpm@11.7.0 ...
    call npm install -g pnpm@11.7.0
    call :RefreshPath
    where pnpm >nul 2>&1
    if !errorlevel! neq 0 (
        echo [错误] pnpm 安装失败 - 请手动执行 npm install -g pnpm@11.7.0 后重跑。
        goto :PI_UPDATE_END
    )
)
pushd "!PI_UP_PI!"
echo [更新] pnpm install（依赖同步，数分钟）...
call pnpm install
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm install 失败 - 请查看上方输出（网络/镜像）后重跑（可续跑）。
    goto :PI_UPDATE_END
)
echo [更新] pnpm build（dist 不入库，必须重建；数分钟）...
call pnpm run build
if !errorlevel! neq 0 (
    popd
    echo [错误] 构建失败 - 请查看上方输出排查（Node 版本建议 22+）。
    goto :PI_UPDATE_END
)
popd
if not exist "!PI_UP_PI!\packages\coding-agent\dist\cli.js" (
    echo [错误] 构建后仍未找到 packages\coding-agent\dist\cli.js - 请查看上方输出。
    goto :PI_UPDATE_END
)
:: 部署副本同步：exe 侧 PIAgent 与更新根不同目录时才有意义（同目录=根即部署位，跳过）。
for %%i in ("%~dp0PIAgent") do set "PI_DST=%%~fi"
if /i not "!PI_DST!"=="!PI_UP_ROOT!" (
    echo [同步] exe 侧部署副本 robocopy（!PI_UP_PI! → !PI_DST!\pi）...
    if not exist "!PI_DST!" mkdir "!PI_DST!"
    robocopy "!PI_UP_PI!" "!PI_DST!\pi" /E /NFL /NDL /NJH /NJS /NP /R:2 /W:2
    if !errorlevel! GEQ 8 (
        echo [错误] robocopy 失败（退出码 !errorlevel!）- 主更新已完成，副本未同步。
    )
)
echo [OK] PIAgent 更新完成：!PI_UP_ROOT!
echo        回到版本管理窗点「刷新版本」；AutoCode 配置的根未改动。
echo.

:PI_UPDATE_END
goto :VERIFY

:: ---- DSH 更新段（/update-dsh <tag>：fetch+reset 保留本地 + pnpm 重建 + 部署组合文件；
::      2026.09 任务 23183ccb。%~2 = 目标 tag 全名，由 AutoCode 侧 PickLatestTag 解析传入。
::      %~3（可选，带引号）= DSH 根，缺省用脚本同级 DeepSeekHarness\deepseek-harness。
::      失败路径纪律：pushd 后所有退出必须先 popd；先 fetch 后 reset，fetch 失败零影响；
::      pnpm 阶段失败 = 半更新态（旧 bin.js 为 untracked 不受 reset 影响，仍可运行旧版），
::      重跑本更新即修复（幂等全流程）。）----
:UPDATE_DSH
if "%~2"=="" (
    echo [错误] 缺少目标 tag 参数。用法：install_env.bat /update-dsh ^<tag^> [^<dsh根^>]
    echo        例：install_env.bat /update-dsh dsh-v0.1.2-alpha.3
    goto :UPDATE_DSH_END
)
echo ============================================================
echo   更新 DeepSeekHarness 到 %~2
echo ============================================================
echo.
where node >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Node.js - 请先运行完整模式安装（install_env.bat）装好 Node 后重试。
    goto :UPDATE_DSH_END
)
where git >nul 2>&1
if !errorlevel! neq 0 (
    echo [错误] 缺少 Git - 请先运行完整模式安装（install_env.bat）装好 Git 后重试。
    goto :UPDATE_DSH_END
)
if not exist "!DSH_ROOT!\package.json" (
    echo [错误] 未找到本机安装 !DSH_ROOT!\package.json - 请先在 AutoCode 设置页
    echo        「安装 DeepSeekHarness…」完成安装后再更新。
    goto :UPDATE_DSH_END
)
:: 版本变量显式初始化为空（读失败时空值护卫跳过对比只报另一侧）。
set "OLD_VER="
set "NEW_VER="
pushd "!DSH_ROOT!"
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content -Raw package.json|ConvertFrom-Json).version"`) do set "OLD_VER=%%v"
echo [更新] 当前版本：!OLD_VER!
echo [更新] git fetch 目标 tag（浅拉取 refspec，浅/全克隆均适用）...
git fetch --depth 1 origin "+refs/tags/%~2:refs/tags/%~2"
if !errorlevel! neq 0 (
    popd
    echo [错误] fetch 失败（网络/代理），工作区未做任何修改。
    goto :UPDATE_DSH_END
)
git reset --hard FETCH_HEAD
if !errorlevel! neq 0 (
    popd
    echo [错误] git reset 失败 - 请查看上方输出排查后重跑本更新。
    goto :UPDATE_DSH_END
)
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content -Raw package.json|ConvertFrom-Json).version"`) do set "NEW_VER=%%v"
echo [更新] 源码已切换到：!NEW_VER!
echo [更新] 清理旧构建残留与半态（官方 clean 脚本；跨大版本更新必做——
echo        手工删 lib 会留 tsbuildinfo/中间态不一致，tsc -b 会跳过 emit 导致构建失败）...
call pnpm run clean
echo [安装] pnpm 包管理器 pnpm@11.7.0 ...
call npm install -g pnpm@11.7.0
call :RefreshPath
where pnpm >nul 2>&1
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm 安装失败 - 请关闭窗口重试，或手动执行 npm install -g pnpm@11.7.0
    goto :UPDATE_DSH_END
)
echo [更新] pnpm 依赖安装 - 数分钟，请耐心等待...
call pnpm install
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm install 失败 - 处于半更新态（源码已到新 tag、构建未完成），
    echo        重跑本更新修复；期间旧版本仍可运行。
    goto :UPDATE_DSH_END
)
echo [构建] 完整构建（0.1.5 起 build:lib 依赖 native 先行，须整体跑；约 5~15 分钟）...
call pnpm run build
if !errorlevel! neq 0 (
    popd
    echo [错误] build 失败 - 处于半更新态，重跑本更新修复（clean 已清残留会全量重建）。
    goto :UPDATE_DSH_END
)
popd
:: 部署/校验 AutoCode 组合层（2026.09.10 双布局，同 :DSH_CORDIS 段口径）。
if exist "!DSH_ROOT!\apps\cli\lib\bin.js" (
    if exist "%~dp0dsh\sdk.autocode.patch.yml" (
        echo [OK] 官方 SDK profile 布局验收通过：apps\cli 入口 + 组合层 sdk.autocode.patch.yml 就位。
    ) else (
        echo [警告] 未找到 %~dp0dsh\sdk.autocode.patch.yml - 请确认 AutoCode 程序目录完整。
    )
) else (
    if not exist "!DSH_ROOT!\examples\autocode-agent" mkdir "!DSH_ROOT!\examples\autocode-agent"
    if exist "%~dp0dsh\cordis.autocode.yml" (
        copy /y "%~dp0dsh\cordis.autocode.yml" "!DSH_ROOT!\examples\autocode-agent\cordis.yml" >nul
        echo [OK] 已部署 AutoCode 组合文件 cordis.yml。
    ) else (
        echo [警告] 未找到 %~dp0dsh\cordis.autocode.yml - 请确认 AutoCode 程序目录完整。
    )
    if not exist "!DSH_ROOT!\packages\examples\jsonrpc-demo\lib\bin.js" (
        echo [错误] 未找到 bin.js - 更新未完成，请查看上方输出排查。
        goto :UPDATE_DSH_END
    )
    if not exist "!DSH_ROOT!\examples\node_modules\@deepseek-ai\dsh-agent-spine-demo\package.json" (
        echo [错误] 组合依赖的 dsh-agent-spine-demo 不可达 - 该版本与 AutoCode 不兼容
        echo        请改用官方 SDK profile 版本（dsh-v0.1.2+，AutoCode 已适配）或回退：
        echo        install_env.bat /update-dsh dsh-v0.1.0-rc.7 "<DSH 根目录>"
        goto :UPDATE_DSH_END
    )
)
if "!OLD_VER!"=="" (
    echo [OK] 已更新到 v!NEW_VER!（旧版本读取失败，跳过对比）。
) else if "!NEW_VER!"=="" (
    echo [OK] 已完成更新（新版本读取失败；源码已到 %~2）。
) else if "!OLD_VER!"=="!NEW_VER!" (
    echo [OK] 已是最新 v!NEW_VER!（已重新拉取并重建校验）。
) else (
    echo [OK] 已更新：v!OLD_VER! → v!NEW_VER!
)
echo.

:UPDATE_DSH_END
goto :VERIFY

:: ---- Node.js LTS 单独安装/升级段（/node：版本管理对话框 Node 预检闸调用）----
:NODE
echo ============================================================
echo   Node.js LTS 安装/升级（Claude Code 2.x 要求 Node 22 以上）
echo ============================================================
echo.
call :EnsureNodeLts
echo.
echo   提示 - 窗口出现 [OK] Node.js 已就绪 才算完成；完成后请关闭并重开
echo   AutoCode（让新 PATH 生效），再回后端版本管理窗点更新。
echo ============================================================
goto :END

:: ---- 10. 安装结果验证 ----
:VERIFY
echo ============================================================
echo   安装结果
echo ============================================================
call :ShowVer git --version
call :ShowVer node --version
call :ShowVer npm --version
where pnpm >nul 2>&1
if !errorlevel! equ 0 (
    echo [pnpm]
    pnpm --version
    echo.
)
if exist "!DSH_ROOT!\packages\examples\jsonrpc-demo\lib\bin.js" (
    echo [DeepSeekHarness] 已安装：!DSH_ROOT!
) else (
    echo [DeepSeekHarness] 未安装 - 可选组件，可在 AutoCode 设置页单独安装
)
if exist "%~dp0PIAgent\pi\packages\coding-agent\dist\cli.js" (
    echo [PIAgent] 已安装：%~dp0PIAgent\pi
) else if exist "!NPM_ROOT_G!\@earendil-works\pi-coding-agent\dist\cli.js" (
    echo [PIAgent] 已安装（npm 全局）：!NPM_ROOT_G!\@earendil-works\pi-coding-agent
) else (
    echo [PIAgent] 未安装 - 可选组件，可在 AutoCode 设置页单独安装
)
if exist "%~dp0HermesHarness\dist\cli.js" (
    echo [Hermes] 已安装：%~dp0HermesHarness
) else (
    echo [Hermes] 未安装 - 可在 AutoCode 设置页 Hermes 页联网安装（git）或本机源安装
)
if "!DSH_ONLY!"=="0" (
    call :ShowVer claude --version
    if /i "!BACKEND!"=="GLM" (
        where chelper >nul 2>&1
        if !errorlevel! equ 0 (
            echo [chelper] 已安装，首次使用需 chelper 初始化。
        ) else (
            echo [缺失] chelper
        )
    )
)
echo.

:: ---- 11. 后续操作提示（DSH 单装模式无需 Claude 配置指引）----
if "!DSH_ONLY!"=="1" goto :END
if "!PI_ONLY!"=="1" goto :END
if "!HERMES_ONLY!"=="1" goto :END
if "!DSH_UPDATE!"=="1" goto :END
echo ============================================================
echo   后续配置（需要的话按提示完成）：
echo ============================================================
if /i "!BACKEND!"=="GLM" (
    echo   你选择了「国产 GLM」接入，请按下面配置：
    echo     1. 开一个新的命令行窗口（cmd 或 PowerShell）
    echo     2. 输入：chelper
    echo     3. 按提示输入「智谱 API key」（在本脚本后面）
    echo     4. 去编辑器里选 Claude Code - 安装/更新 - 完成安装
    echo   配置完成后，claude 命令将路由到 GLM 模型。
) else (
    echo   你选择了「Anthropic 原版」接入，首次使用请按下面登录：
    echo     1. 开一个新的命令行窗口（cmd 或 PowerShell）
    echo     2. 输入：claude
    echo     3. 按提示用 Anthropic 账号登录，或环境变量配 ANTHROPIC_API_KEY
)
echo.
echo   提 - claude / chelper 命令若未识别，请关闭并重开命令行窗口（让 PATH 生效）。
echo ============================================================

:END
echo.
pause
exit /b

:: ==================== 子函数 ====================

:JunctionPrecheck
:: 2026.09.13 pnpm 前置能力预检：pnpm 依赖目录联接（reparse point）。
:: 受限令牌（沙箱/助手会话/组策略禁 reparse）下会 EPERM，提前给出明确指引，
:: 而不是让 pnpm 在深处报 symlink 错误。返回 1=环境不可用。
set "_JT_TEST=%TEMP%\ac_jt_%RANDOM%"
mklink /J "!_JT_TEST!" "%TEMP%" >nul 2>&1
if exist "!_JT_TEST!" (
    rd "!_JT_TEST!" >nul 2>&1
    set "_JT_TEST="
    exit /b 0
)
set "_JT_TEST="
echo [错误] 当前环境禁止创建目录联接（junction）- pnpm 无法安装依赖。
echo        常见原因：在助手/沙箱会话或受限令牌中运行了本脚本。
echo        解决：回到 AutoCode 设置页点「安装」按钮（独立管理员窗口运行），
echo        或自行开一个 cmd 窗口直接运行本脚本（正常用户令牌不受限）。
exit /b 1

:RefreshPath
:: 把注册表两处 Machine + User PATH 前置合并，再追加初始进程 PATH（_ORIG_PATH）防丢失。
:: 直接覆盖会因当前是管理员账户而丢原用户 PATH（含 winget 的 WindowsApps），
:: 导致 winget 在第二次调用时找不到。
set "M_PATH="
set "U_PATH="
for /f "usebackq tokens=2,*" %%A in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PATH 2^>nul`) do set "M_PATH=%%B"
for /f "usebackq tokens=2,*" %%A in (`reg query "HKCU\Environment" /v PATH 2^>nul`) do set "U_PATH=%%B"
set "PATH=!M_PATH!"
if defined U_PATH set "PATH=!PATH!;!U_PATH!"
set "PATH=!PATH!;!_ORIG_PATH!"
goto :eof

:ShowVer
:: %1=命令  %2=版本参数；已安装则打印版本，未安装则提示缺失。
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
:: 无 winget 时从 nodejs.org 取最新 LTS 的 x64 MSI 静默安装。
:: 引号按 PowerShell 字符串规则写（外双内单），避开 cmd/PS 引号冲突。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; try { $i=Invoke-RestMethod https://nodejs.org/dist/index.json; $v=($i|Where-Object{$_.lts -ne $false}|Select-Object -First 1).version; Write-Host ('Node LTS '+$v); $p=$env:TEMP+'\node-lts.msi'; Invoke-WebRequest ('https://nodejs.org/dist/'+$v+'/node-'+$v+'-x64.msi') -OutFile $p; Write-Host 'Installing MSI...'; Start-Process msiexec -ArgumentList @('/i',$p,'/qb','/norestart') -Wait } catch { Write-Host ('[错误] '+$_.Exception.Message) }"
goto :eof

:EnsureNodeLts
:: 2026.10.10 /node 模式与全量模式共用：确保 PATH 上的 node 主版本 >=22。
::   已达标直接过；否则 winget（可用时）→ nodejs.org LTS x64 MSI 直装兜底；
::   装后 NodeSessionFix（重读注册表 PATH + 会话前置 C:\Program Files\nodejs，
::   防 32 位旧目录在系统 PATH 排前遮蔽新装）再复验；仍 <22 给卸旧指引。
call :NodeVerMajor
if !NODE_MAJOR_NUM! GEQ 22 (
    echo [OK] Node.js 已就绪：!NODE_VER!
    goto :eof
)
echo [升级] Node.js 版本过低（!NODE_VER!，主版本 !NODE_MAJOR_NUM!，需 22 以上）...
where winget >nul 2>&1
if !errorlevel! equ 0 (
    echo   - 尝试 winget（winget 源失败将自动转直装）...
    winget install --id OpenJS.NodeJS.LTS -e --source winget --accept-source-agreements --accept-package-agreements --disable-interactivity
)
call :NodeSessionFix
call :NodeVerMajor
if !NODE_MAJOR_NUM! GEQ 22 goto :ENL_DONE
echo   - winget 未装成，改为直装 Node.js LTS x64 MSI ...
call :InstallNodeDl
call :NodeSessionFix
call :NodeVerMajor
if !NODE_MAJOR_NUM! GEQ 22 goto :ENL_DONE
echo.
echo [错误] Node.js 安装后仍未达标（当前 !NODE_VER!，需 22 以上）。
echo        常见原因：机器上有更老的 Node（如 32 位 0.x，C:\Program Files (x86)\nodejs）
echo        在系统 PATH 中排在前面，遮蔽了新装版本。
echo        处理：控制面板卸载旧版 Node.js，重开本窗口重跑本脚本。
goto :eof
:ENL_DONE
echo [OK] Node.js 已就绪：!NODE_VER!
goto :eof

:NodeVerMajor
:: 探测当前 PATH 首个 node 的完整版本与主版本（node 缺失/异常 = 0）。
::   NODE_VER 形如 v22.19.0；NODE_MAJOR_NUM = 去 v 后第一段数字（v0.12.2 → 0）。
set "NODE_VER=未知"
set "NODE_MAJOR_NUM=0"
where node >nul 2>&1
if !errorlevel! neq 0 goto :eof
for /f "delims=" %%v in ('node -v 2^>nul') do set "NODE_VER=%%v"
if "!NODE_VER!"=="未知" goto :eof
for /f "tokens=1 delims=v." %%v in ("!NODE_VER!") do set "NODE_MAJOR_NUM=%%v"
goto :eof

:NodeSessionFix
:: 装后 PATH 会话修正：重读注册表 PATH（MSI 写机器级需重读才可见），再把新 x64
::   Node 固定目录前置到会话 PATH——本窗口后续 npm 用新 Node；系统级持久顺序仍
::   以注册表为准（旧目录排前时需卸旧版，见 EnsureNodeLts 的指引）。
call :RefreshPath
if exist "C:\Program Files\nodejs\node.exe" set "PATH=C:\Program Files\nodejs;!PATH!"
goto :eof

:InstallGitDl
:: 无 winget 时从 GitHub git-for-windows latest release 取 64-bit 安装包静默安装。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; try { $r=Invoke-RestMethod https://api.github.com/repos/git-for-windows/git/releases/latest; $a=$r.assets|Where-Object{$_.name -like '*-64-bit.exe' -and $_.name -notlike '*mini*'}|Select-Object -First 1; Write-Host ('Git '+$a.name); $p=$env:TEMP+'\git-setup.exe'; Invoke-WebRequest $a.browser_download_url -OutFile $p; Write-Host 'Installing...'; Start-Process $p -ArgumentList '/VERYSILENT','/NORESTART','/SP-' -Wait } catch { Write-Host ('[错误] '+$_.Exception.Message) }"
goto :eof
