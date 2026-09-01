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
:: ---- 参数/codex = 只安装 CodexHarness（设置页「安装 CodexHarness…」按钮调用）----
set "CODEX_ONLY=0"
if /i "%~1"=="/codex" set "CODEX_ONLY=1"
:: ---- 参数/update-dsh <tag> = 更新 DeepSeekHarness 到指定 tag（fetch+reset 保留本地；
::      版本管理对话框「更新」调用，tag 由 AutoCode 经 ls-remote 解析后传入）----
set "DSH_UPDATE=0"
if /i "%~1"=="/update-dsh" set "DSH_UPDATE=1"

title AutoCode 环境安装
cd /d "%~dp0"
:: DSH 固定安装布局：脚本目录内（即 Execute 程序目录）DeepSeekHarness\deepseek-harness
::   子目录 - 绿色便携：发布只拷 Execute 目录，DSH 随目录整体带走；程序侧
::   CDeepSeekRunner::ProbeSiblingRoot 同布局探测并自动回填配置。
for %%i in ("%~dp0DeepSeekHarness\deepseek-harness") do set "DSH_ROOT=%%~fi"

if "!DSH_UPDATE!"=="1" goto :BANNER_DSH_UPDATE
if "!DSH_ONLY!"=="1" goto :BANNER_DSH
if "!PI_ONLY!"=="1" goto :BANNER_PI
if "!CODEX_ONLY!"=="1" goto :BANNER_CODEX
echo ============================================================
echo   AutoCode 环境安装
echo   一键安装：Git、Node.js LTS、Claude Code CLI
echo   可选安装：DeepSeekHarness - DeepSeek 官方 Agent 后端
echo   可选安装：PIAgent - pi coding-agent（本机源复制安装）
echo   可选接入：GLM 中继工具 - Claude Code 换国产模型
echo ============================================================
echo.
goto :ADMIN

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

:BANNER_CODEX
echo ============================================================
echo   CodexHarness 单独安装
echo   安装内容：@openai/codex@0.87.0（npm 本地工程安装，--save-exact 锁定版本）
echo   安装位置：脚本同级 Execute\CodexHarness
echo   版本说明：v0.88+ 已移除 wire_api="chat"，本后端锁定 0.87.0（chat 网关接入）
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

:ADMIN

:: ---- 0. 管理员权限检测（写 Program Files / 机器级 PATH 需要）----
net session >nul 2>&1
if !errorlevel! neq 0 (
    echo [提示] 安装需要管理员权限，正在尝试 UAC 提权...
    if "%~1"=="" (
        powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    ) else (
        powershell -NoProfile -Command "Start-Process '%~f0' -ArgumentList '%*' -Verb RunAs"
    )
    exit /b
)
echo [OK] 已获取管理员权限。
echo.

:: ---- 1. 选择 Claude Code 接入后端（单装/更新模式跳过）----
if "!DSH_UPDATE!"=="1" goto :UPDATE_DSH
if "!DSH_ONLY!"=="1" goto :DETECT
if "!PI_ONLY!"=="1" goto :DETECT
if "!CODEX_ONLY!"=="1" goto :CODEX
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
if /i "!BACKEND!"=="GLM" ( where chelper >nul 2>&1 && set "HAVE_HELPER=1" )

echo [检测]
if "!HAVE_NODE!"=="1"   (echo    node     已安装) else (echo    node     缺失)
if "!HAVE_GIT!"=="1"    (echo    git      已安装) else (echo    git      缺失)
if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" (
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
if "!DSH_ONLY!"=="0" if "!PI_ONLY!"=="0" (
    if "!HAVE_CLAUDE!"=="0" set "NEED=1"
    if /i "!BACKEND!"=="GLM" if "!HAVE_HELPER!"=="0" set "NEED=1"
)

if "!NEED!"=="0" (
    echo [检测] 所有必要组件已经就绪，无需重装。
    echo.
    if "!DSH_ONLY!"=="1" goto :DSH
    if "!PI_ONLY!"=="1" goto :PI
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
:: PI 单装模式：node 就绪即可（无需 git/npm 全局件），直接进安装段。
if "!PI_ONLY!"=="1" goto :PI

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

:: ---- 9. 国产 GLM 中继（选了 GLM 才装）----
if /i "!BACKEND!"=="GLM" (
    where chelper >nul 2>&1
    if !errorlevel! equ 0 (
        echo [OK] 国产中继 chelper 已安装，跳过。
    ) else (
        echo [安装] @z_ai/coding-helper ...
        call npm install -g @z_ai/coding-helper
        call :RefreshPath
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
    git clone --depth 1 https://github.com/deepseek-ai/deepseek-harness.git "!DSH_ROOT!"
    if !errorlevel! neq 0 (
        echo [错误] 克隆失败 - 请检查网络，GitHub 访问可能需要代理。
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
echo [安装] pnpm 依赖 - 数分钟，请耐心等待...
pushd "!DSH_ROOT!"
call pnpm install
if !errorlevel! neq 0 (
    popd
    echo [错误] pnpm install 失败 - 请检查网络代理后重跑本脚本。
    goto :DSH_END
)
echo [构建] 编译 lib - 仓库不含 lib 构建产物，必须执行...
call pnpm run build:lib
if !errorlevel! neq 0 (
    popd
    echo [错误] build:lib 失败 - 请查看上方输出排查 Node 版本（建议 22.19 以上或 24 以上）。
    goto :DSH_END
)
popd
echo.

:DSH_CORDIS
:: 部署 AutoCode 组合文件（正本在 AutoCode 仓库 agent/dsh，构建时同步到脚本同级 dsh\）。
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
if not defined PI_SRC set "PI_SRC=%~dp0..\PIAgent"
for %%i in ("!PI_SRC!") do set "PI_SRC=%%~fi"
for %%i in ("%~dp0PIAgent") do set "PI_DST=%%~fi"
if not exist "!PI_SRC!\pi\packages\coding-agent\dist\cli.js" (
    echo [错误] 源目录无效：!PI_SRC!\pi 下未找到 packages\coding-agent\dist\cli.js
    echo        可用环境变量 PI_SRC 指定 PIAgent 仓库根（含 pi\ 子目录）后重跑：
    echo          set PI_SRC=D:\path\to\PIAgent ^&^& install_env.bat /pi
    goto :PI_END
)
echo [安装] robocopy 复制 pi 运行时（源：!PI_SRC!\pi → 目标：!PI_DST!\pi）...
if not exist "!PI_DST!" mkdir "!PI_DST!"
robocopy "!PI_SRC!\pi" "!PI_DST!\pi" /E /NFL /NDL /NJH /NJS /NP /R:2 /W:2
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

:: ---- DSH 更新段（/update-dsh <tag>：fetch+reset 保留本地 + pnpm 重建 + 部署组合文件；
::      2026.09 任务 23183ccb。%~2 = 目标 tag 全名，由 AutoCode 侧 PickLatestTag 解析传入。
::      失败路径纪律：pushd 后所有退出必须先 popd；先 fetch 后 reset，fetch 失败零影响；
::      pnpm 阶段失败 = 半更新态（旧 bin.js 为 untracked 不受 reset 影响，仍可运行旧版），
::      重跑本更新即修复（幂等全流程）。）----
:UPDATE_DSH
if "%~2"=="" (
    echo [错误] 缺少目标 tag 参数。用法：install_env.bat /update-dsh ^<tag^>
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
echo [构建] 编译 lib ...
call pnpm run build:lib
if !errorlevel! neq 0 (
    popd
    echo [错误] build:lib 失败 - 处于半更新态（旧 bin.js 未删仍可运行），重跑本更新修复。
    goto :UPDATE_DSH_END
)
popd
:: 部署 AutoCode 组合文件（语句同 :DSH_CORDIS 段，此处重写不动原段）。
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
) else (
    echo [PIAgent] 未安装 - 可选组件，可在 AutoCode 设置页单独安装
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

:InstallGitDl
:: 无 winget 时从 GitHub git-for-windows latest release 取 64-bit 安装包静默安装。
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; try { $r=Invoke-RestMethod https://api.github.com/repos/git-for-windows/git/releases/latest; $a=$r.assets|Where-Object{$_.name -like '*-64-bit.exe' -and $_.name -notlike '*mini*'}|Select-Object -First 1; Write-Host ('Git '+$a.name); $p=$env:TEMP+'\git-setup.exe'; Invoke-WebRequest $a.browser_download_url -OutFile $p; Write-Host 'Installing...'; Start-Process $p -ArgumentList '/VERYSILENT','/NORESTART','/SP-' -Wait } catch { Write-Host ('[错误] '+$_.Exception.Message) }"
goto :eof
