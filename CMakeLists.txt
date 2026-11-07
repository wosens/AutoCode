cmake_minimum_required(VERSION 3.21)

# 提示：若 CMake 找不到 Qt6，使用 CMakePresets.json 的 "qt6" preset，
# 或显式 -DCMAKE_PREFIX_PATH=D:/ProgramData/QT6.8.3/6.8.3/msvc2022_64
if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
    set(QT6_HINT "D:/ProgramData/QT6.8.3/6.8.3/msvc2022_64")
    if(EXISTS "${QT6_HINT}")
        list(APPEND CMAKE_PREFIX_PATH "${QT6_HINT}")
    endif()
endif()

project(AutoCode VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode / UTF-8 执行字符集（源码以 UTF-8 with BOM 保存）
if(MSVC)
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    # 2026.09.10 Release 崩溃定位基础设施：Release 也产完整 PDB（/Zi + /DEBUG:FULL），
    #   并用 /OPT:NOICF 关掉 identical COMDAT folding——v2.2.147 前后两次崩溃同一 RVA
    #   但行为已变，ICF 把不同函数折叠同体是定位黑盒的主因；REF 保留（去未引用）。
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:NOICF;/INCREMENTAL:NO>")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 COMPONENTS Core Gui Widgets Network Test REQUIRED)
enable_testing()

qt_standard_project_setup()

qt_add_executable(AutoCode WIN32
    main.cpp
    core/CCrashDump.h
    core/CCrashDump.cpp
    core/CFatalWitness.h
    core/CFatalWitness.cpp
    core/TaskTypes.h
    core/CModelEntry.h
    core/CVersion.h
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    # 2026-10-14 任务编排设计：类型/档位/阶段编排配置（orchestration.json + 动态阶段注册）
    core/COrchConfig.h
    core/COrchConfig.cpp
    # 2026-11-07 编辑→查找/替换：纯搜索/替换引擎 + 共用对话框 + 结果 Tab 页
    core/CFileSearcher.h
    core/CFileSearcher.cpp
    ui/CFindReplaceDialog.h
    ui/CFindReplaceDialog.cpp
    ui/CFindResultsPage.h
    ui/CFindResultsPage.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CAgentRunnerFactory.h
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CMcpAskServer.h
    agent/CMcpAskServer.cpp
    orch/CTaskOrchestrator.h
    orch/CTaskOrchestrator.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    ui/CMainWindow.h
    ui/CMainWindow.cpp
    ui/CTaskListPanel.h
    ui/CTaskListPanel.cpp
    ui/CExecutionPanel.h
    ui/CExecutionPanel.cpp
    ui/CAsstTabBar.h
    ui/CAsstTabBar.cpp
    ui/CAsstReportDialog.h   # 2026-11-04 助手分析报告页（右键「查看分析报告」）
    ui/CAsstReportDialog.cpp
    ui/CProjectCostDialog.h  # 2026-11-05 工程总消耗统计窗口（工程菜单）
    ui/CProjectCostDialog.cpp
    ui/CTitleBar.h
    ui/CTitleBar.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    # 2026-10-17 对话流自绘三期（doc/对话流视图自绘改造方案.md）：ModelView 引擎
    #   四件套——显示条目模型/纯 painter delegate/QTextDocument LRU 池/QListView 视图
    #   （SBlockData.h 数据结构随 CStreamPresenter.h 分发）
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStageBar.h
    ui/CStageBar.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CRecentProjectsDialog.h
    ui/CRecentProjectsDialog.cpp
    ui/CRecentChipsPanel.h
    ui/CRecentChipsPanel.cpp
    ui/CAskUserDialog.h
    ui/CAskUserDialog.cpp
    ui/CNewTaskDialog.h
    ui/CNewTaskDialog.cpp
    # 2026-10-14 任务编排设计窗口（功能菜单入口）
    ui/COrchDesignDialog.h
    ui/COrchDesignDialog.cpp
    ui/CVersionDialog.h
    ui/CVersionDialog.cpp
    ui/CSettingsDialog.h
    ui/CSettingsDialog.cpp
    ui/CProjectExplorerPanel.h
    ui/CProjectExplorerPanel.cpp
    ui/CRollbackDialog.h
    ui/CRollbackDialog.cpp
    ui/CDiffViewDialog.h
    ui/CDiffViewDialog.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    ui/CAboutDialog.h
    ui/CAboutDialog.cpp
    ui/CTipStyle.h
    ui/CTipStyle.cpp
    # SQLite 历史库层（纯 C API，直接编 sqlite3.c）
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    # 2026-11-05 工程总消耗聚合（history.db 任务量 + assistant 档助手量，菜单统计用）
    db/CProjectUsage.h
    db/CProjectUsage.cpp
    sqlite3.h
    sqlite3.c
    # 测试沙箱（2026-11-01 动态库化）：实现整体退到 GhostSandbox.dll（TestSandbox\），
    # exe 只留宿主门面（加载/转发）+ CLI 解析（子进程模式不加载沙箱 dll）；
    # ABI 边界见 sandbox/SandboxAbi.h，接入文档 doc/测试沙箱接入开发指南.md。
    sandbox/SandboxAbi.h
    sandbox/CSandboxHost.h
    sandbox/CSandboxHost.cpp
    sandbox/SandboxCli.h
    sandbox/SandboxCli.cpp
    # 2026-09-30 代码审查接入（ReviewSdk.dll 动态加载 + 异步审查服务 + 右键审查对话框）
    review/CReviewSdkClient.h
    review/CReviewSdkClient.cpp
    review/CReviewService.h
    review/CReviewService.cpp
    ui/CCodeReviewDialog.h
    ui/CCodeReviewDialog.cpp
    # FR-10 应用图标资源：.qrc（经 AUTORCC 编入，运行期 QIcon(":/icons/app.ico")）
    # 与 .rc（Windows exe 资源图标，资源管理器显示）。
    resources/app.qrc
    resources/app.rc
)

# sqlite3.c 是官方 amalgamation 单文件：必须按 C 编译（C++ 编译会让定义得到 C++ 改名，
# 与 sqlite3.h 里 extern "C" 的 C 链接声明不匹配 → 链接报无法解析的外部符号）。
# 同时 /W0 抑制其海量无害警告。
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c PROPERTIES
        LANGUAGE C COMPILE_FLAGS "/W0")
endif()

# zstd 静态库（备份压缩，#2）：第三方预编译静态库 libzstd_static.lib（x64/MSVC）。
# 头文件位于 third_party/zstd；CProjectManager.cpp 调用其 C API 做逐文件压缩/解压。
# CProjectManager.cpp 同时被 AutoCode / test_projectmanager / test_rollback 编译，
# 三者均需此 include 目录与静态库（否则链接缺 ZSTD_* 符号）。
set(ZSTD_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd)
set(ZSTD_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/libzstd_static.lib)

target_include_directories(AutoCode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/db
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox
    ${CMAKE_CURRENT_SOURCE_DIR}/review   # 2026-09-30 ReviewSdk 接入
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen   # AutoCodeVersion.h（PRE_BUILD 生成，见下方版本号段）
)

target_link_libraries(AutoCode PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network dwmapi Shell32 mfplat mfreadwrite mfuuid ole32 ${ZSTD_LIB} frwdevice)

# ---- fileread.dll 适配层（2026.09.13 文件读取统一走 fileread.dll，用户指令）----
# CFrwDevice（QIODevice 适配器）：open→FRW_OpenW / readData→FRW_Read / close→FRW_Close，
# 三件套 fileread.dll+code.exe+filereadwrite.h 位于 third_party/fileread（动态加载免 .lib）。
# 缺失/失效时兜底 QFile 直读；无 Q_OBJECT 无需 moc。所有编译生产 .cpp（含已改读点）的
# 目标均需链接本库（静态库自带 include 目录，frwreadwrite.h 仅被库自身包含）。
add_library(frwdevice STATIC
    core/CFrwDevice.h
    core/CFrwDevice.cpp
)
target_include_directories(frwdevice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fileread/include
)
target_link_libraries(frwdevice PUBLIC Qt6::Core)

# ---- 三段式版本号 v主.次.编译号（如 v1.1.38）----
# 2026.08.24 起按 %USERPROFILE%\.claude\CLAUDE.md「Version Number」规则：
#   计数依据 = 工程根 readme.txt 修改记录条数（编译号=条数；每满 20 条次版本 +1；
#   每满 100 条主版本 +1 且次版本清零），GenVersion.cmake 自动推导三段。
#   readme.txt 首行维护「版本：vX.Y.N」，追加 Change Log 时同步更新。
# 生成的 AutoCodeVersion.h 供 core/CVersion.h 包含，标题栏经 AutoCodeVersionText() 显示。
add_custom_command(TARGET AutoCode PRE_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DREADME_FILE=${CMAKE_CURRENT_SOURCE_DIR}/readme.txt
        -DGEN_DIR=${CMAKE_BINARY_DIR}/gen
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/GenVersion.cmake
    COMMENT "Generate AutoCodeVersion.h (from readme.txt entry count)"
)

# 输出到原 Execute 目录，沿用旧习惯
# VS 是 multi-config 生成器，默认会在 RUNTIME_OUTPUT_DIRECTORY 下加 <CONFIG> 子目录；
# 这里用 per-config 属性把 Release 直出 Execute/ 根（Debug 仍落 Execute/Debug/）。
set_target_properties(AutoCode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_SOURCE_DIR}/Execute
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ============================================================================
# GhostSandbox.dll —— 首个测试沙箱动态库（2026-11-01 沙箱动态库化）。
#   即原「内置幽灵沙箱」整体（Core/Manager/Recorder/Dialog + ABI 适配层）；
#   输出到 Execute/TestSandbox/（主程序 QLibrary 按全路径加载，[Sandbox] Type 路由）；
#   私有依赖/配置目录 Execute/TestSandbox/GhostSandbox\（资产正本 sandbox/assets/）。
# WINDOWS_EXPORT_ALL_SYMBOLS：C++ 类/sbx:: 函数导出给单测直调；跨模块数据导入
#   不可靠（Core 的 prop 名已函数化）；C ABI 导出在 SandboxAbi.h 里显式 dllexport。
# ============================================================================
add_library(GhostSandbox SHARED
    sandbox/SandboxAbi.h
    sandbox/CGhostSandboxEntry.cpp
    sandbox/CSandboxCore.h
    sandbox/CSandboxCore.cpp
    sandbox/CSandboxManager.h
    sandbox/CSandboxManager.cpp
    sandbox/CSandboxRecorder.h
    sandbox/CSandboxRecorder.cpp
    sandbox/CSandboxDialog.h
    sandbox/CSandboxDialog.cpp
    sandbox/SandboxCli.h
    sandbox/SandboxCli.cpp
)
target_include_directories(GhostSandbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox
)
target_link_libraries(GhostSandbox PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets dwmapi Shell32 user32
    mfplat mfreadwrite mfuuid ole32 oleaut32
)
set_target_properties(GhostSandbox PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox
    LIBRARY_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox
)
# 沙箱私有目录（同名文件夹）：依赖与自有配置正本随建随更（不删用户自加文件）。
add_custom_command(TARGET GhostSandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:GhostSandbox>/GhostSandbox"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/assets/GhostSandbox"
        "$<TARGET_FILE_DIR:GhostSandbox>/GhostSandbox"
    COMMENT "Sync TestSandbox/GhostSandbox private dir"
)

# ============================================================================
# MsbSandbox —— 第二个测试沙箱（2026-11-01：Rust SDK→cdylib 接入 microsandbox）。
#   源码 msbsandbox/（Cargo 工程，C ABI 见 SandboxAbi.h）；可选 target：需要
#   Rust 工具链（cargo）且首次构建联网拉 crates（已配 rsproxy 镜像），故不入
#   ALL/默认构建——手动 cmake --build . --target MsbSandbox。
#   产物归置同 GhostSandbox 口径（2026-11-01 用户指令）：一切构建产物只落
#   Execute/TestSandbox/——cargo 中间产物走 CARGO_TARGET_DIR（源码树零残留；
#   固定盘根短路径 E:\msb-target：长路径下 cc-rs 调 MSVC lib.exe 报 1104
#   ——AutoCode\build\msbsandbox-target 实测复现，盘根短路径实测通过），
#   dll+pdb 部署到 TestSandbox；runtime（msb.exe+libkrunfw.dll，
#   ≈70MB，首次 cargo 构建时 SDK 自动装到 ~/.microsandbox）构建后自动同步到
#   TestSandbox/MsbSandbox/runtime/（已存在则 copy_if_different 增量）。资产
#   正本 sandbox/assets/MsbSandbox/（README；runtime 二进制不入仓库）。
# ============================================================================
file(TO_NATIVE_PATH "E:/msb-target" MSB_CARGO_TARGET)
# runtime 同步命令（~/.microsandbox 存在时才追加；首次构建后重跑 cmake 补齐）
set(MSB_RUNTIME_CMDS "")
if(EXISTS "$ENV{USERPROFILE}/.microsandbox/bin/msb.exe")
    list(APPEND MSB_RUNTIME_CMDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$ENV{USERPROFILE}/.microsandbox/bin/msb.exe"
            "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox/runtime/bin/msb.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$ENV{USERPROFILE}/.microsandbox/lib/libkrunfw.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox/runtime/lib/libkrunfw.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$ENV{USERPROFILE}/.microsandbox/lib/libkrunfw.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox/runtime/bin/libkrunfw.dll")
endif()
add_custom_target(MsbSandbox
    # ① 资产正本 → 私有目录
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/assets/MsbSandbox"
        "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox"
    # ② cargo 构建（中间产物钉进 build/msbsandbox-target，源码树不留 target/）
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${MSB_CARGO_TARGET}"
        cargo build --release
    # ③ 部署 dll + pdb（与 GhostSandbox 同目录口径）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/msb-target/release/MsbSandbox.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/msb-target/release/MsbSandbox.pdb"
        "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox.pdb"
    # ④ runtime 同步：~/.microsandbox（SDK download-binaries 安装位）→ 私有目录
    #    （源缺失时静默跳过——首次构建已装，重建不重复下载）
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox/runtime/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Execute/TestSandbox/MsbSandbox/runtime/lib"
    # ④ runtime 同步（configure 期探测 ~/.microsandbox，存在才生成拷贝命令；
    #    cmd 内嵌 if exist 的引号在 VERBATIM 下损坏，故用 configure 期分支）。
    #    msb.exe 静态导入 libkrunfw.dll：bin 旁须有副本（否则 spawn 即 0xC0000135；
    #    注意 msb.exe 还静态导入 winhvplatform.dll——WHP 未启用的机器上 msb.exe
    #    本就无法启动，属预期，启用后自愈）。
    ${MSB_RUNTIME_CMDS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/msbsandbox"
    COMMENT "Build & deploy MsbSandbox.dll (Rust cdylib, needs cargo + network)"
    VERBATIM
)

# 环境安装脚本随 exe 输出到 Execute 目录（功能菜单 → 环境安装 调用）
add_custom_command(TARGET AutoCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_env.bat"
        "$<TARGET_FILE_DIR:AutoCode>/install_env.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/hermes_oneshot_patch.py"
        "$<TARGET_FILE_DIR:AutoCode>/hermes_oneshot_patch.py"
    # 2026.09.13 助手护栏：exe 同级 CLAUDE.md（CAgentRunnerBase::
    #   SharedGlobalContextContent 前置注入给 pi 助手）——防助手直接
    #   edit GBK 的 config.ini 损坏中文、防代跑长安装任务被停滞检测杀。
    #   不覆盖用户手放的 exe 同级 CLAUDE.md：部署目录已有同名文件时跳过。
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/agent_env_guard.md"
        "$<TARGET_FILE_DIR:AutoCode>/agent_env_guard.md"
    COMMAND cmd /c "if not exist \"$<TARGET_FILE_DIR:AutoCode>\\CLAUDE.md\" copy /y \"$<TARGET_FILE_DIR:AutoCode>\\agent_env_guard.md\" \"$<TARGET_FILE_DIR:AutoCode>\\CLAUDE.md\" >nul"
    # 2026.10.21 后端自愈知识库：exe 同级 harness.md（后端版本管理右键「自动修复」
    #   提示词引用；修复 Agent 按它了解五后端启动方式/验证命令/常见故障）。
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/harness.md"
        "$<TARGET_FILE_DIR:AutoCode>/harness.md"
    COMMENT "Copy install_env.bat to output dir"
)

# 2026.08.15 prompts 模板同步到 Execute 目录：源保留在工程根（便于编辑/版本管理），
# 构建后同步到 exe 同级 prompts/；运行期 GetPromptsDir 只读 exe 目录（便携原则，
# Execute 整目录拷走即自包含）。
add_custom_command(TARGET AutoCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/prompts"
        "$<TARGET_FILE_DIR:AutoCode>/prompts"
    COMMENT "Sync prompts/ to output dir"
)

# 2026.08.17 帮助文档同步到 Execute 目录：正本在工程根 help/（版本管理），
# 构建后同步到 exe 同级 help/；「帮助」菜单打开 exe 同级 help/使用说明.html。
add_custom_command(TARGET AutoCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/help"
        "$<TARGET_FILE_DIR:AutoCode>/help"
    COMMENT "Sync help/ to output dir"
)

# 2026.08.18 DSH 组合文件正本同步到 Execute/dsh/：install_env.bat 的 /dsh 安装段
#   从脚本同级 dsh\cordis.autocode.yml 取正本，部署到 DSH 仓库 examples/autocode-agent/
#   cordis.yml（组合文件必须在 examples/ 树内才能解析 node_modules，见 CDeepSeekRunner）。
add_custom_command(TARGET AutoCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:AutoCode>/dsh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/agent/dsh/cordis.autocode.yml"
        "$<TARGET_FILE_DIR:AutoCode>/dsh/cordis.autocode.yml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/agent/dsh/sdk.autocode.patch.yml"
        "$<TARGET_FILE_DIR:AutoCode>/dsh/sdk.autocode.patch.yml"
    # 2026-10-16 DSH mkdtemp 断继承修复：exe 同级 dsh\pyshim\sitecustomize.py
    #   （CDeepSeekRunner 前挂 PYTHONPATH 注入受限 python 子进程——tempfile.mkdtemp
    #   的 0o600 保护型 DACL 断继承致 pip 类写入全拒，详见 agent/dsh/pyshim/）。
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:AutoCode>/dsh/pyshim"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/agent/dsh/pyshim/sitecustomize.py"
        "$<TARGET_FILE_DIR:AutoCode>/dsh/pyshim/sitecustomize.py"
    COMMENT "Sync DSH cordis.autocode.yml + sdk.autocode.patch.yml + pyshim to output dir"
)

# ---- 单元测试（CProjectManager 最近打开历史 MRU 逻辑）----
# 仅测试业务逻辑，不依赖 GUI；config.ini 落在测试 exe 同级，用例内自行清理。
qt_add_executable(test_projectmanager
    tests/test_projectmanager.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CAgentRunnerFactory.h
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    ui/CSettingsDialog.h
    ui/CSettingsDialog.cpp
    sandbox/CSandboxHost.h
    sandbox/CSandboxHost.cpp      # 2026-11-01 沙箱类型枚举（TestSandbox\*.dll）
    ui/CVersionDialog.h
    ui/CVersionDialog.cpp
)
target_include_directories(test_projectmanager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_projectmanager PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(test_projectmanager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_projectmanager COMMAND test_projectmanager)

# ---- 单元测试（多助手 Tab：任务点击反复切档不挂死/不泄漏幽灵 Tab）----
# 复现「点击任务卡死」路径：LoadAssistantsFromJson ↔ ResetAssistants 与
# 「+」占位页的 currentChanged 再入。GUI 测试（QWidget）。
qt_add_executable(test_execpanel
    tests/test_execpanel.cpp
    ui/CRecentProjectsDialog.h
    ui/CRecentProjectsDialog.cpp
    ui/CRecentChipsPanel.h
    ui/CRecentChipsPanel.cpp
    ui/CExecutionPanel.h
    ui/CExecutionPanel.cpp
    ui/CAsstTabBar.h
    ui/CAsstTabBar.cpp
    ui/CAsstReportDialog.h   # 2026-11-04 助手分析报告页（CExecutionPanel 依赖，测试目标同步）
    ui/CAsstReportDialog.cpp
    ui/CAskUserDialog.h
    ui/CAskUserDialog.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    # 2026-10-17 对话流自绘三期（doc/对话流视图自绘改造方案.md）：ModelView 引擎
    #   四件套——显示条目模型/纯 painter delegate/QTextDocument LRU 池/QListView 视图
    #   （SBlockData.h 数据结构随 CStreamPresenter.h 分发）
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStageBar.h
    ui/CStageBar.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    # 2026-11-07 查找结果 Tab 页（CExecutionPanel 依赖；引擎同编）
    core/CFileSearcher.h
    core/CFileSearcher.cpp
    ui/CFindReplaceDialog.h
    ui/CFindReplaceDialog.cpp
    ui/CFindResultsPage.h
    ui/CFindResultsPage.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    ui/CTipStyle.h
    ui/CTipStyle.cpp
    # 2026-10-31 文件预览语法页读 [View] CodeSyntax（CFilePreviewWidget →
    #   CProjectManager::LoadCodeSyntax 静态；口径同 test_projectmanager/test_rollback
    #   的多目标编译——工厂引用四 Runner 构造，全 Runner 需一并编入）
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h   # 2026.10.15 CExecutionPanel::AsstContextLimit 引用 ContextWindowForModel
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(test_execpanel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project   # 2026-10-31 CProjectManager.h（语法高亮开关读取）
    ${ZSTD_INCLUDE_DIR}   # 2026-10-31 CProjectManager.cpp 备份压缩依赖（口径同 test_rollback）
    ${CMAKE_BINARY_DIR}/gen   # 2026.10.11 AutoCodeVersion.h（启动页版本号经 CRecentProjectsDialog 源问入）
)
target_link_libraries(test_execpanel PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(test_execpanel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_execpanel COMMAND test_execpanel)

# 2026-11-07 CodeStyle 双 DLL 镜像到测试运行目录（Execute\Release；探针/测试共
#   用；无 DLL 时跳过——语法页自动回退纯文本不影响其余用例）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Execute/CodeView.dll")
    add_custom_command(TARGET test_execpanel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Execute/CodeView.dll"
            "$<TARGET_FILE_DIR:test_execpanel>/CodeView.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Execute/CodeParser.dll"
            "$<TARGET_FILE_DIR:test_execpanel>/CodeParser.dll"
        COMMENT "Mirror CodeStyle DLLs for tests")
endif()

# ---- 单元测试（CTaskListPanel 左栏两表切换口径：再次执行切当前任务/终态切历史任务）----
# 2026.09.10 v2.2.153 回归：SelectTask 切「当前任务」页+发选中信号（主窗口链终点
# SetCurrentTask 自动落「日志」页，见 test_execpanel）；SelectHistoryTask 切「历史
# 任务」页选中刚结束任务（OnTaskFinished 终态链）。GUI 测试（QWidget，offscreen）。
qt_add_executable(test_tasklistpanel
    tests/test_tasklistpanel.cpp
    ui/CTaskListPanel.h
    ui/CTaskListPanel.cpp
    ui/CProjectExplorerPanel.h
    ui/CProjectExplorerPanel.cpp
    sandbox/CSandboxHost.h
    sandbox/CSandboxHost.cpp      # 2026-11-01 豁免登记经宿主单例（沙箱实现在 GhostSandbox.dll）
    core/StageDef.h
    core/StageDef.cpp
    core/TaskTypes.h
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(test_tasklistpanel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox
)
target_link_libraries(test_tasklistpanel PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test Shell32 frwdevice GhostSandbox)
# 2026-11-01 沙箱动态库化：import-lib 链 GhostSandbox.dll；Windows 加载器按 exe 目录
# 搜 DLL，而 dll 落 TestSandbox\ —— 拷一份到测试输出目录（同配置目录，Debug/Release 均可）。
add_custom_command(TARGET test_tasklistpanel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:GhostSandbox>" "$<TARGET_FILE_DIR:test_tasklistpanel>/GhostSandbox.dll"
    COMMENT "Mirror GhostSandbox.dll for test_tasklistpanel"
)
set_target_properties(test_tasklistpanel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_tasklistpanel COMMAND test_tasklistpanel)

# ---- 单元测试（CTaskHistoryDB：SQLite 历史库 Upsert/LoadAll/Delete 往返）----
qt_add_executable(test_historydb
    tests/test_historydb.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    core/StageDef.h
    core/StageDef.cpp
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    db/CProjectUsage.h
    db/CProjectUsage.cpp
    sqlite3.h
    sqlite3.c
)
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c PROPERTIES
        LANGUAGE C COMPILE_FLAGS "/W0")
endif()
target_include_directories(test_historydb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/db
)
target_link_libraries(test_historydb PRIVATE Qt6::Core Qt6::Test frwdevice)
set_target_properties(test_historydb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_historydb COMMAND test_historydb)

# ---- 2026-11-07 在文件中查找/替换引擎回归（编辑→查找/替换）：纯逻辑 + 临时目录，
#      不依赖 GUI——匹配三开关/多根遍历/排除目录/编码探测回写/替换漂移校验。----
qt_add_executable(test_filesearcher
    tests/test_filesearcher.cpp
    core/CFileSearcher.h
    core/CFileSearcher.cpp
)
target_include_directories(test_filesearcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(test_filesearcher PRIVATE Qt6::Core Qt6::Test)
set_target_properties(test_filesearcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_filesearcher COMMAND test_filesearcher)

# ---- 单元测试（be3fd900：Verify 阶段定义/流程编入 + guide 指纹扫描器纯逻辑）----
# 覆盖 AC2.1/2.2/2.4、AC-N1、AC1.2/1.4/1.5。纯逻辑 + 临时文件系统，不依赖 GUI / Claude。
qt_add_executable(test_verify_flow
    tests/test_verify_flow.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    core/TaskTypes.h
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    core/CModelEntry.h
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    # 2026.09.19 后端切换清会话链回归（testBackendSwitchClearsSessions）需要完整编排器：
    #   CTaskOrchestrator + CProjectManager + 五 Runner + db（同 test_orchpi 依赖面，
    #   但本目标挂 ctest 自动跑——纯离线构造，不碰真机 pi/spark）。
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CAgentRunnerFactory.h
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CMcpAskServer.h
    agent/CMcpAskServer.cpp
    orch/CTaskOrchestrator.h
    orch/CTaskOrchestrator.cpp
    # 2026-09-30 编排器新增 ReviewSdk 预审依赖（同 AutoCode 目标；无 dll 时预审自动跳过）
    review/CReviewSdkClient.h
    review/CReviewSdkClient.cpp
    review/CReviewService.h
    review/CReviewService.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    sqlite3.h
    sqlite3.c
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    # 2026-10-17 自绘三期：CStreamPresenter 的 ModelView 引擎分支引用四件套（链接需要）
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
)
target_include_directories(test_verify_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/review
    ${CMAKE_CURRENT_SOURCE_DIR}/db
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_verify_flow PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Test ${ZSTD_LIB} frwdevice)
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c PROPERTIES LANGUAGE C COMPILE_FLAGS "/W0")
endif()
set_target_properties(test_verify_flow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_verify_flow COMMAND test_verify_flow)

# ---- 单元测试（96931df4 / FR-1 验证结论落盘前置不变量）----
# 覆盖 AC-1.1/1.2/1.3 的稳定前置：Verify 在 Feature/Bugfix 流程内位置正确、
# sOutput=="verify.md"、结论提取正则容忍全/半角冒号+空白、统一行与验证结论行可由前缀区分。
# 纯逻辑（core 阶段定义 + 正则），不依赖 GUI / Claude / 私有方法 RecordVerifySummary。
qt_add_executable(test_record_verify
    tests/test_record_verify.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    core/TaskTypes.h
)
target_include_directories(test_record_verify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(test_record_verify PRIVATE Qt6::Core Qt6::Test frwdevice)
set_target_properties(test_record_verify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_record_verify COMMAND test_record_verify)

# ---- 单元测试（FR-6 代码回退：备份 / 恢复 / 版本清单）----
# 覆盖 AC-6.1/6.2、AC-6.4、AC-N。纯逻辑 + 临时文件系统，不依赖 GUI / Claude。
qt_add_executable(test_rollback
    tests/test_rollback.cpp
    ui/CRollbackDialog.h
    ui/CRollbackDialog.cpp
    ui/CDiffViewDialog.h
    ui/CDiffViewDialog.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CAgentRunnerFactory.h
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp   # StageDef 的 ResolveFlow 引用 GetFlowStages（2026.08.15 流程档位）
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(test_rollback PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_rollback PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(test_rollback PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_rollback COMMAND test_rollback)





# ---- 一次性 / 批量导入工具（把工程 .autodev/tasks 灌进全局历史库 history.db）----
# 开发期辅助工具，非测试、非主程序；仅 CMake 目标，不入 vcxproj。
qt_add_executable(import_tasks
    tools/import_tasks.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    core/StageDef.h
    core/StageDef.cpp
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    sqlite3.h
    sqlite3.c
)
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c PROPERTIES
        LANGUAGE C COMPILE_FLAGS "/W0")
endif()
target_include_directories(import_tasks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/db
)
target_link_libraries(import_tasks PRIVATE Qt6::Core frwdevice)
set_target_properties(import_tasks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ---- 集成测试（CDeepSeekRunner：联网真实调用 DSH 组合 + glm 中继，不挂 ctest）----
# 手动跑: Execute/Release/test_dshrunner.exe（覆盖握手/三信号流/多轮续会话/注入轮）。
# 2026.09.09 v2.1.133 编排器+pi+本地 spark 真机端到端（超限自动压缩自愈链验证；
# 不挂 ctest——依赖本地 llama.cpp 8080 与真实网关往返）。
qt_add_executable(test_orchpi
    tests/test_orchpi.cpp
    core/TaskTypes.h
    core/CModelEntry.h
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CAgentRunnerFactory.h
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CMcpAskServer.h
    agent/CMcpAskServer.cpp
    orch/CTaskOrchestrator.h
    orch/CTaskOrchestrator.cpp
    # 2026-09-30 编排器新增 ReviewSdk 预审依赖（同 AutoCode 目标；无 dll 时预审自动跳过）
    review/CReviewSdkClient.h
    review/CReviewSdkClient.cpp
    review/CReviewService.h
    review/CReviewService.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    sqlite3.h
    sqlite3.c
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    # 2026-10-17 自绘三期：CStreamPresenter 的 ModelView 引擎分支引用四件套（链接需要）
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
)
target_include_directories(test_orchpi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/review
    ${CMAKE_CURRENT_SOURCE_DIR}/db
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_orchpi PRIVATE Qt6::Core Qt6::Network Qt6::Widgets ${ZSTD_LIB} frwdevice)
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c PROPERTIES LANGUAGE C COMPILE_FLAGS "/W0")
endif()
set_target_properties(test_orchpi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

qt_add_executable(test_dshrunner
    tests/test_dshrunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
)
target_include_directories(test_dshrunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
)
target_link_libraries(test_dshrunner PRIVATE Qt6::Core Qt6::Network frwdevice)
set_target_properties(test_dshrunner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
# 2026.09.10 同步 DSH 组合层到测试 exe 同级（Profile 布局的 --patch 引用 exe 目录
#   dsh\sdk.autocode.patch.yml；与 AutoCode 主程序的 POST_BUILD 同口径）。
add_custom_command(TARGET test_dshrunner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:test_dshrunner>/dsh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/agent/dsh/sdk.autocode.patch.yml"
        "$<TARGET_FILE_DIR:test_dshrunner>/dsh/sdk.autocode.patch.yml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/agent/dsh/cordis.autocode.yml"
        "$<TARGET_FILE_DIR:test_dshrunner>/dsh/cordis.autocode.yml"
    COMMENT "Sync DSH overlays to test_dshrunner dir"
)

# ---- 集成测试（CClaudeRunner：联网真实调用本机 claude CLI + 配置网关，不挂 ctest）----
# 手动跑: Execute/test_clauderunner.exe（2026.09.23 v3.1.225 修复回归锚：GLM 网关
#   assistant 帧 usage 全 0，上下文占用只能来自 message_delta 兜底——断言运行中
#   sigContextFill(>0) 必达 + 收尾 LastContextFill/LastModel 非空）。
qt_add_executable(test_clauderunner
    tests/test_clauderunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
)
target_include_directories(test_clauderunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
)
target_link_libraries(test_clauderunner PRIVATE Qt6::Core Qt6::Network frwdevice)
set_target_properties(test_clauderunner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
# 2026-10-08 stub 模式挂 ctest（不联网）：claude 命令指向批处理桩，验证共享大模型表
#   env 注入三态（实例槽覆盖优先 / 跟随开关+当前条目 / 关=不注入）。无参仍为联网手动模式。
add_test(NAME test_clauderunner_stub COMMAND test_clauderunner -stub)

# ---- Pi 后端离线单元测试（不联网，挂 ctest）----
qt_add_executable(test_pirunner
    tests/test_pirunner.cpp
    core/CModelEntry.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
)
target_include_directories(test_pirunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
)
target_link_libraries(test_pirunner PRIVATE Qt6::Core Qt6::Network frwdevice)
add_test(NAME test_pirunner COMMAND test_pirunner)

# ---- 后端版本管理纯函数离线单元测试（不联网不弹窗，挂 ctest；任务 23183ccb）----
qt_add_executable(test_versiondlg
    tests/test_versiondlg.cpp
    core/CModelEntry.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    ui/CVersionDialog.h
    ui/CVersionDialog.cpp
)
target_include_directories(test_versiondlg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(test_versiondlg PRIVATE Qt6::Core Qt6::Network Qt6::Widgets frwdevice)
add_test(NAME test_versiondlg COMMAND test_versiondlg)

# ---- CodexHarness 后端离线单元测试（不联网，挂 ctest）----
qt_add_executable(test_codexrunner
    tests/test_codexrunner.cpp
    core/CModelEntry.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
)
target_include_directories(test_codexrunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
)
target_link_libraries(test_codexrunner PRIVATE Qt6::Core Qt6::Network frwdevice)
add_test(NAME test_codexrunner COMMAND test_codexrunner)

# ---- Hermes 第五后端离线单元测试（不联网，挂 ctest；任务 3213cdbf）----
qt_add_executable(test_hermesrunner
    tests/test_hermesrunner.cpp
    core/CModelEntry.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(test_hermesrunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/orch
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${ZSTD_INCLUDE_DIR}
)
target_link_libraries(test_hermesrunner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(test_hermesrunner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_hermesrunner COMMAND test_hermesrunner)

# ---- CFrwDevice（fileread.dll 适配器）离线单元测试（2026.09.13，不联网，挂 ctest）----
qt_add_executable(test_frwdevice
    tests/test_frwdevice.cpp
)
target_include_directories(test_frwdevice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(test_frwdevice PRIVATE Qt6::Core Qt6::Test frwdevice)
set_target_properties(test_frwdevice PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_frwdevice COMMAND test_frwdevice)

# ---- 2026-09-30 ReviewSdk 接入回归：动态加载/元信息/mock-LLM 全链/错误码/相对路径。
#      纯离线（canned 模型应答）；exe 同目录无 ReviewSdk.dll 时全部 QSKIP（0 通过）。
qt_add_executable(test_reviewsdk
    tests/test_reviewsdk.cpp
    review/CReviewSdkClient.h
    review/CReviewSdkClient.cpp
)
target_include_directories(test_reviewsdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/review
)
# 注：exe 输出到 Execute/（与 ReviewSdk.dll 同目录，运行期动态加载）。
target_link_libraries(test_reviewsdk PRIVATE Qt6::Core Qt6::Test)
set_target_properties(test_reviewsdk PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
# 运行依赖：exe 同目录需 ReviewSdk.dll（多配置生成器落在 Release/ 子目录，从
#   Execute/ 根镜像；Qt6Core.dll 依赖不镜像——与测试 exe 自身的 Qt 一并经 PATH
#   解析，混入另一份副本会与 PATH 里的 Qt 组件版本冲突致 0xc0000409）。
#   源不存在时静默跳过——测试制全部 QSKIP，不红。
add_custom_command(TARGET test_reviewsdk POST_BUILD
    COMMAND cmd /c "if exist \"${CMAKE_CURRENT_SOURCE_DIR}\\Execute\\ReviewSdk.dll\" copy /y \"${CMAKE_CURRENT_SOURCE_DIR}\\Execute\\ReviewSdk.dll\" \"$<TARGET_FILE_DIR:test_reviewsdk>\\ReviewSdk.dll\" >nul"
    COMMENT "Mirror ReviewSdk.dll for test_reviewsdk"
)
add_test(NAME test_reviewsdk COMMAND test_reviewsdk)

# ---- 内置沙箱测试（2026-09-26）：需要真实 Win32 窗口（幽灵化/截图/投递消息/MP4 录像），
#      刻意不进 offscreen 清单；无窗口环境（无桌面会话）自动 QSKIP ----
qt_add_executable(test_sandbox
    tests/test_sandbox.cpp
)
# 2026-11-01 沙箱动态库化：直调符号（sbx::*/CSandboxManager）经 import lib 来自
# GhostSandbox.dll（WINDOWS_EXPORT_ALL_SYMBOLS 全导出）。
target_include_directories(test_sandbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sandbox
)
target_link_libraries(test_sandbox PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test GhostSandbox)
add_custom_command(TARGET test_sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:GhostSandbox>" "$<TARGET_FILE_DIR:test_sandbox>/GhostSandbox.dll"
    COMMENT "Mirror GhostSandbox.dll for test_sandbox"
)
set_target_properties(test_sandbox PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_sandbox COMMAND test_sandbox)

# ==== 对话流 Model/View 自绘引擎模型层回归（2026-10-17 自绘三期）====
# CChatModel 折叠/映射/回填纯逻辑 + CStreamPresenter ModelView 分支集成
#（Bind(model,view2)/Append/BackfillResult/Rebuild/EnsureCoverage 壳转发）。
#（2026-10-19 顺序修正：目标定义必须在 978 set_tests_properties / 989 foreach
#   POST_BUILD 引用之前——上一会话把目标块放文件尾，配置期"找不到 test"炸配置。）
qt_add_executable(test_chatmodel
    tests/test_chatmodel.cpp
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
)
target_include_directories(test_chatmodel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(test_chatmodel PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test frwdevice)
set_target_properties(test_chatmodel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_chatmodel COMMAND test_chatmodel)

# ==== 栈溢出复现探针（2026-10-23 三连崩 0xC00000FD：切助手同步递归 resize→重排→校正→resize）====
qt_add_executable(probe_sorec
    tests/probe_sorec.cpp
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
)
target_include_directories(probe_sorec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(probe_sorec PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets frwdevice)
set_target_properties(probe_sorec PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ==== 复制浮层不透明底像素探针（2026-11-03：双击选择态被选文字背景不得透明，透明则与
#     底下 delegate 原条目文字叠加成重影）——手动运行：Execute\probe_ovbg.exe，
#     退出码 0=不透明通过 / 3=透明失败，产出 ovbg_*.png 供人眼复核。====
qt_add_executable(probe_ovbg
    tests/probe_ovbg.cpp
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
)
target_include_directories(probe_ovbg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(probe_ovbg PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test frwdevice)
set_target_properties(probe_ovbg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ==== 后端版本管理窗口宽度稳定性探针（2026-11-03：点「验证所有后端」窗口被撑宽——
#     状态行单行 QLabel 无 wordWrap，min 宽随文字长度顶高布局）——手动运行：
#     Execute\probe_vdlgwidth.exe，退出码 0=宽度稳定 / 3=变宽，不挂 ctest
#    （构造对话框会拉起真实 CLI 探测进程，关闭即由析构收敛）。====
# 2026-11-06 查找结果窗拖动 bug 探针（真鼠标点击 .cpp 行 → 捕获/焦点/定位），手动不挂 ctest。
# 2026-11-06 HotNews 工程三报复现探针（7000+命中连点卡死/经搜索乱码/Ctrl+F 前置），手动不挂 ctest。
qt_add_executable(probe_hotnews
    tests/probe_hotnews.cpp
    ui/CFindResultsPage.h
    ui/CFindResultsPage.cpp
    core/CFileSearcher.h
    core/CFileSearcher.cpp
    ui/CExecutionPanel.h
    ui/CExecutionPanel.cpp
    ui/CAsstTabBar.h
    ui/CAsstTabBar.cpp
    ui/CAsstReportDialog.h
    ui/CAsstReportDialog.cpp
    ui/CAskUserDialog.h
    ui/CAskUserDialog.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStageBar.h
    ui/CStageBar.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    ui/CTipStyle.h
    ui/CTipStyle.cpp
    ui/CFindReplaceDialog.h
    ui/CFindReplaceDialog.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(probe_hotnews PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen
)
target_link_libraries(probe_hotnews PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(probe_hotnews PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

qt_add_executable(probe_findwin
    tests/probe_findwin.cpp
    ui/CFindResultsPage.h
    ui/CFindResultsPage.cpp
    core/CFileSearcher.h
    core/CFileSearcher.cpp
    ui/CExecutionPanel.h
    ui/CExecutionPanel.cpp
    ui/CAsstTabBar.h
    ui/CAsstTabBar.cpp
    ui/CAsstReportDialog.h
    ui/CAsstReportDialog.cpp
    ui/CAskUserDialog.h
    ui/CAskUserDialog.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/SBlockData.h
    ui/CChatModel.h
    ui/CChatModel.cpp
    ui/CChatDocPool.h
    ui/CChatDocPool.cpp
    ui/CChatDelegate.h
    ui/CChatDelegate.cpp
    ui/CChatListView.h
    ui/CChatListView.cpp
    ui/CStageBar.h
    ui/CStageBar.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    ui/CTipStyle.h
    ui/CTipStyle.cpp
    ui/CFindReplaceDialog.h
    ui/CFindReplaceDialog.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(probe_findwin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen
)
target_link_libraries(probe_findwin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(probe_findwin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# 2026-11-06 查找定位双 bug 探针（代码文件定位/鼠标捕获），手动运行不挂 ctest：
#   Execute\probe_findpos.exe，退出码 0=全过 / 3=复现失败 / 4=模块不在位跳过。
qt_add_executable(probe_findpos
    tests/probe_findpos.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(probe_findpos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen
)
target_link_libraries(probe_findpos PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network ${ZSTD_LIB} frwdevice)
set_target_properties(probe_findpos PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# 2026-11-07 大单行 JSON 纯文本页耗时探针（查找结果连点 chat.json 行卡死量化），手动运行不挂 ctest。
qt_add_executable(probe_bigline
    tests/probe_bigline.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(probe_bigline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen
)
target_link_libraries(probe_bigline PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network ${ZSTD_LIB} frwdevice)
set_target_properties(probe_bigline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# 2026-11-07 Ctrl+F 作用域裁决探针（预览页文件内查找 vs 主窗全局查找同键裁决），手动运行不挂 ctest。
qt_add_executable(probe_ctrlf
    tests/probe_ctrlf.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    ui/CCodeSyntaxView.h
    ui/CCodeSyntaxView.cpp
    project/CProjectManager.h
    project/CProjectManager.cpp
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CTaskInfo.h
    core/CTaskInfo.cpp
)
target_include_directories(probe_ctrlf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/project
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen
)
target_link_libraries(probe_ctrlf PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB} frwdevice)
set_target_properties(probe_ctrlf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

qt_add_executable(probe_vdlgwidth
    tests/probe_vdlgwidth.cpp
    core/CModelEntry.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
    agent/CClaudeRunner.h
    agent/CClaudeRunner.cpp
    agent/CDeepSeekRunner.h
    agent/CDeepSeekRunner.cpp
    agent/CPiAgentRunner.h
    agent/CPiAgentRunner.cpp
    agent/CCodexRunner.h
    agent/CCodexRunner.cpp
    agent/CCodexProxy.h
    agent/CCodexProxy.cpp
    agent/CHermesRunner.h
    agent/CHermesRunner.cpp
    ui/CVersionDialog.h
    ui/CVersionDialog.cpp
)
target_include_directories(probe_vdlgwidth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_link_libraries(probe_vdlgwidth PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Test frwdevice)
set_target_properties(probe_vdlgwidth PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ==== qFatal 见证链路验证探针（2026-10-28 静默退出加固：装见证→qFatal→验遗言文件）====
# 手动运行（不挂 ctest——它故意 fastfail 退 3）：Execute\probe_qtwitness.exe 运行后
# 进程即逝，验证同目录 dump\_qfatal_witness.txt 出现 witness-probe 行即链路通。
qt_add_executable(probe_qtwitness
    tests/probe_qtwitness.cpp
    core/CFatalWitness.h
    core/CFatalWitness.cpp
)
target_include_directories(probe_qtwitness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(probe_qtwitness PRIVATE Qt6::Core)
set_target_properties(probe_qtwitness PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
# ---- 测试统一无头运行：Qt 控件测试（如 test_execpanel 实例化 CExecutionPanel 面板）
# 在 offscreen 平台跑，不再弹出真实窗口抢占桌面焦点；QTest 事件模拟照常工作 ----
set_tests_properties(
    test_projectmanager test_execpanel test_tasklistpanel test_historydb test_verify_flow
    test_record_verify test_rollback test_pirunner test_versiondlg test_codexrunner
    test_hermesrunner test_frwdevice test_chatmodel
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ---- fileread 三件套随所有输出目录（exe 与全部测试）——CFrwDevice 运行时依赖
#（2026.09.13 文件读取统一走 fileread.dll；dll 缺失时兜底 QFile 直读不瘫）----
# 2026-11-05 占用容忍：改走 scripts/CopyLockedTolerant.cmake（内容同跳过；目标被
#   运行中实例锁定时按 CLAUDE.md「Exe Update」规则：旧件改名入同目录 ~temp →
#   复制新件；仍失败仅 WARNING 不破构建，下次构建 MD5 比对自动重试）。
foreach(tgt AutoCode test_projectmanager test_execpanel test_tasklistpanel test_historydb
            test_verify_flow test_record_verify test_rollback import_tasks test_orchpi
            test_dshrunner test_pirunner test_versiondlg test_codexrunner test_hermesrunner
            test_frwdevice test_sandbox test_chatmodel)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fileread/bin/fileread.dll"
            "-DDST=$<TARGET_FILE_DIR:${tgt}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/CopyLockedTolerant.cmake"
        COMMAND ${CMAKE_COMMAND}
            "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fileread/bin/code.exe"
            "-DDST=$<TARGET_FILE_DIR:${tgt}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/CopyLockedTolerant.cmake"
        COMMENT "Deploy fileread.dll/code.exe next to ${tgt} (lock-tolerant)")
endforeach()

# ==== 任务编排设计核心链路回归（2026-10-14，由临时探针 probe_orch 转正）====
qt_add_executable(test_orchconfig
    tests/test_orchconfig.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    core/COrchConfig.h
    core/COrchConfig.cpp
)
target_include_directories(test_orchconfig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_orchconfig PRIVATE Qt6::Core)
add_test(NAME test_orchconfig COMMAND test_orchconfig)
