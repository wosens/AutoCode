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
    core/TaskTypes.h
    core/CModelEntry.h
    core/CVersion.h
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
    ui/CTitleBar.h
    ui/CTitleBar.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CRecentProjectsDialog.h
    ui/CRecentProjectsDialog.cpp
    ui/CAskUserDialog.h
    ui/CAskUserDialog.cpp
    ui/CNewTaskDialog.h
    ui/CNewTaskDialog.cpp
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
    ui/CAboutDialog.h
    ui/CAboutDialog.cpp
    # SQLite 历史库层（纯 C API，直接编 sqlite3.c）
    db/CTaskHistoryDB.h
    db/CTaskHistoryDB.cpp
    sqlite3.h
    sqlite3.c
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
    ${ZSTD_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/gen   # AutoCodeVersion.h（PRE_BUILD 生成，见下方版本号段）
)

target_link_libraries(AutoCode PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network dwmapi ${ZSTD_LIB})

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

# 环境安装脚本随 exe 输出到 Execute 目录（功能菜单 → 环境安装 调用）
add_custom_command(TARGET AutoCode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_env.bat"
        "$<TARGET_FILE_DIR:AutoCode>/install_env.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/hermes_oneshot_patch.py"
        "$<TARGET_FILE_DIR:AutoCode>/hermes_oneshot_patch.py"
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
    COMMENT "Sync DSH cordis.autocode.yml to output dir"
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
target_link_libraries(test_projectmanager PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB})
set_target_properties(test_projectmanager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_projectmanager COMMAND test_projectmanager)

# ---- 单元测试（多助手 Tab：任务点击反复切档不挂死/不泄漏幽灵 Tab）----
# 复现「点击任务卡死」路径：LoadAssistantsFromJson ↔ ResetAssistants 与
# 「+」占位页的 currentChanged 再入。GUI 测试（QWidget）。
qt_add_executable(test_execpanel
    tests/test_execpanel.cpp
    ui/CExecutionPanel.h
    ui/CExecutionPanel.cpp
    ui/CMessageView.h
    ui/CMessageView.cpp
    ui/CMarkdownPreloader.h
    ui/CMarkdownPreloader.cpp
    ui/CStreamPresenter.h
    ui/CStreamPresenter.cpp
    ui/CMessageBlock.h
    ui/CMessageBlock.cpp
    ui/CArtifactView.h
    ui/CArtifactView.cpp
    ui/CFilePreviewWidget.h
    ui/CFilePreviewWidget.cpp
    agent/CAgentRunnerFactory.h
    agent/CAgentRunnerBase.h
    agent/CAgentRunnerBase.cpp
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
)
target_link_libraries(test_execpanel PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB})
set_target_properties(test_execpanel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_execpanel COMMAND test_execpanel)

# ---- 单元测试（CTaskHistoryDB：SQLite 历史库 Upsert/LoadAll/Delete 往返）----
qt_add_executable(test_historydb
    tests/test_historydb.cpp
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
target_include_directories(test_historydb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/db
)
target_link_libraries(test_historydb PRIVATE Qt6::Core Qt6::Test)
set_target_properties(test_historydb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_historydb COMMAND test_historydb)

# ---- 单元测试（be3fd900：Verify 阶段定义/流程编入 + guide 指纹扫描器纯逻辑）----
# 覆盖 AC2.1/2.2/2.4、AC-N1、AC1.2/1.4/1.5。纯逻辑 + 临时文件系统，不依赖 GUI / Claude。
qt_add_executable(test_verify_flow
    tests/test_verify_flow.cpp
    core/StageDef.h
    core/StageDef.cpp
    core/CStageMachine.h
    core/CStageMachine.cpp
    core/TaskTypes.h
    project/CGuideFingerprintScanner.h
    project/CGuideFingerprintScanner.cpp
)
target_include_directories(test_verify_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/project
)
target_link_libraries(test_verify_flow PRIVATE Qt6::Core Qt6::Test)
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
target_link_libraries(test_record_verify PRIVATE Qt6::Core Qt6::Test)
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
target_link_libraries(test_rollback PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB})
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
target_link_libraries(import_tasks PRIVATE Qt6::Core)
set_target_properties(import_tasks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

# ---- 集成测试（CDeepSeekRunner：联网真实调用 DSH 组合 + glm 中继，不挂 ctest）----
# 手动跑: Execute/Release/test_dshrunner.exe（覆盖握手/三信号流/多轮续会话/注入轮）。
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
target_link_libraries(test_dshrunner PRIVATE Qt6::Core Qt6::Network)
set_target_properties(test_dshrunner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)

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
target_link_libraries(test_pirunner PRIVATE Qt6::Core Qt6::Network)
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
target_link_libraries(test_versiondlg PRIVATE Qt6::Core Qt6::Network Qt6::Widgets)
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
target_link_libraries(test_codexrunner PRIVATE Qt6::Core Qt6::Network)
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
target_link_libraries(test_hermesrunner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test ${ZSTD_LIB})
set_target_properties(test_hermesrunner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Execute
)
add_test(NAME test_hermesrunner COMMAND test_hermesrunner)

# ---- 测试统一无头运行：Qt 控件测试（如 test_execpanel 实例化 CExecutionPanel 面板）
# 在 offscreen 平台跑，不再弹出真实窗口抢占桌面焦点；QTest 事件模拟照常工作 ----
set_tests_properties(
    test_projectmanager test_execpanel test_historydb test_verify_flow
    test_record_verify test_rollback test_pirunner test_versiondlg test_codexrunner
    test_hermesrunner
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
