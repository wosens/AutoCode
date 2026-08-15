AutoCode — 全自动开发编排器 修改记录
======================================

[2026-08-11] 初始化工程：基于 Claude Code headless CLI 的全自动开发/问题解决编排器（C++/Qt6）
  - 新增构建系统：CMakeLists.txt、CMakePresets.json（替代旧 .vcxproj 手工维护；旧 AutoCode.vcxproj/.sln 及空脚手架 AutoCode.cpp/.h/.ui/.qrc 暂保留）
  - 核心层 core/：TaskTypes.h（任务/阶段/状态枚举）、StageDef.h/.cpp（13 阶段元数据表）、CTaskInfo.h/.cpp（任务数据模型）、CStageMachine.h/.cpp（阶段流转 + 评审驳回路由状态机）
  - Agent 层 agent/：CClaudeRunner.h/.cpp（QProcess 调 claude，流式解析 stream-json）
  - 编排层 orch/：CTaskOrchestrator.h/.cpp（按状态机驱动阶段、解析 Pass/Reject 决策、迭代上限、落盘 record.md）
  - 工程管理 project/：CProjectManager.h/.cpp（.autodev 目录结构、guide.md 自动生成、record.md 追加、任务私有目录隔离）
  - UI 层 ui/：CMainWindow.h/.cpp（主窗口+菜单+任务编排整合）、CTaskListPanel.h/.cpp（任务列表）、CExecutionPanel.h/.cpp（阶段进度+实时日志+cost）
  - 业务模板 prompts/：01_intake.md ~ 13_bug_fix.md（13 阶段 prompt，评审阶段强制“决策：通过/驳回”格式供解析）
  - 文档 doc/执行方案.md（架构、调用契约、状态机、目录结构、MVP 范围、后续 Phase 路线）
  - 入口 main.cpp 改为 CMainWindow

构建：
  cmake --preset qt6-debug
  cmake --build build --config Debug
  产物：Execute/Debug/AutoCode.exe

设计要点：
  - 编排用确定性 C++ 状态机，不用 LLM 当路由
  - 每个阶段 = 一次 claude headless 调用，读上游 .md 写下游 .md
  - 只读阶段用 --allowedTools "Read,Grep,Glob" 硬约束
  - 任务产物按 .autodev/tasks/<id>/ 隔离，避免并发覆盖
  - feature 流(1-10) 与 bugfix 流(1,2,11,12,13,9,10) 分离

[2026-08-11] 同步 AutoCode.vcxproj + .filters 以支持 VS2022 调试：移除旧空脚手架(AutoCode.cpp/.h/.ui/.qrc)的工程引用(文件保留)，纳入新代码(core/agent/orch/project/ui)，补充 AdditionalIncludeDirectories(.;core;agent;orch;project;ui) 与 /utf-8；MSBuild Debug 编译通过(0 警告 0 错误) - AutoCode.vcxproj, AutoCode.vcxproj.filters

[2026-08-11] 修复评审阶段卡死 + 任务持久化(记忆) + 纳入编排器生成的 MRU 功能
  - Bug 修复(任务 d70e3211 在第3阶段需求评审卡死，根因双重)：① 评审/分析阶段工具集缺 Write，无法写 review_*.md 报告文件 → StageDef 重构为显式三档工具集 ToolsDocs(Read,Grep,Glob,Write)/ToolsDev(+Edit,Bash)/ToolsTest(+Bash 无Edit)；② claude 每步 Write/Edit/Bash 弹权限确认打断全自动 → CClaudeRunner 加 --dangerously-skip-permissions - core/StageDef.h/.cpp, agent/CClaudeRunner.cpp, orch/CTaskOrchestrator.cpp, project/CProjectManager.cpp
  - 任务持久化(记忆)：终止后重启可从上次阶段继续。CTaskInfo 增 SaveToFile/LoadFromFile(序列化 .autodev/tasks/<id>/task.json) + 关键 setter emit sigChanged 触发回写；CProjectManager 增 LoadAllTasks 扫描全部 task.json；CMainWindow 打开工程自动载入、sigChanged 即时回写、析构兜底落盘、删任务清 task.json、Running 态(易失)加载时回退 Paused - core/CTaskInfo.h/.cpp, project/CProjectManager.h/.cpp, ui/CMainWindow.h/.cpp
  - 工程文件：将编排器自生成的「最近打开工程(MRU)」CRecentProjectsDialog.h/.cpp 纳入 vcxproj/.filters(QtMoc+ClCompile)；Release|x64 编译通过(0 警告 0 错误)，产物 Execute/AutoCode.exe - AutoCode.vcxproj, AutoCode.vcxproj.filters

[2026-08-11] 新增「最近打开」工程历史子菜单与全量对话框（首次引入 exe 同级 config.ini 持久化） - CProjectManager.{h,cpp}, CMainWindow.{h,cpp}, CRecentProjectsDialog.{h,cpp}, CMakeLists.txt

[2026-08-11] UI 优化：费用/token 显示、任务列表分页、日志精简
  - 费用/token 显示：cost→「费用」(两位小数)，in/out→「输入/输出」，token 数值用 K/M/G 单位(1000 进制，非 1024) - ui/CExecutionPanel.cpp
  - 任务列表分页：QTabWidget 分「当前任务」(未终结: Pending/Running/Paused/Stopped) 与「历史任务」(终态: Done/Failed) 两页，每行末尾增加创建时间；状态变化时在两页间自动迁移 - ui/CTaskListPanel.h/.cpp
  - 日志精简：过滤 system/session 噪音行；工具入参与结果截断到 80 字符；助手文本截断到 600 字符；Release|x64 编译通过(0 警告 0 错误) - orch/CTaskOrchestrator.cpp

[2026-08-11] 新增「功能」菜单 →「环境安装」：一键安装 Claude Code 运行环境(Git / Node.js / Claude Code CLI)
  - scripts/install_env.bat：检测 UAC 提权，用 winget 安装 Git.Git / OpenJS.NodeJS.LTS，刷新 PATH 后 npm i -g @anthropic-ai/claude-code，末尾验证版本并提示首次登录；支持已装跳过
  - ui/CMainWindow.{h,cpp}：CreateMenus 增「功能(&F)」菜单 +「环境安装...」Action；OnInstallEnv 以独立 cmd 窗口 startDetached 启动 exe 同目录的 install_env.bat
  - CMakeLists.txt：POST_BUILD copy_if_different 把 scripts/install_env.bat 输出到 Execute/ - scripts/install_env.bat, ui/CMainWindow.h/.cpp, CMakeLists.txt

[2026-08-11] 执行面板日志区升级为 Claude Code 风格对话流（Markdown 助手块 + 可折叠工具卡片 + 事件徽章）
  - 编排层去截断 + 结构化信号：CTaskOrchestrator 新增 sigAssistantText/sigToolUse/sigToolResult 三个信号携带全文上抛 UI，不再在编排层把助手文本截 600/工具入参与结果截 80；sigStageLog 现仅承载系统/原始行 - orch/CTaskOrchestrator.h/.cpp
  - 新控件 ui/CMessageView（QScrollArea 对话流容器：跟随尾部、用户上滚暂停跟随、超 ~1500 块丢弃最旧）；ui/CMessageBlock（按种类切换布局：Assistant=Markdown 富文本+品牌色 speaker+左侧强调条+自适应高度；ToolUse=可折叠卡片、标题摘要取 file_path/command 等、Edit/MultiEdit 显示红绿 diff、错误结果自动展开标红；Event=彩色徽章按 kind 映射颜色；System/Error=弱化小字）
  - CExecutionPanel 由 QPlainTextEdit 平铺日志改为 CMessageView 结构化对话流，按任务缓存 block 数据、切换任务时重建整条流、工具结果回填到前一张未闭合卡片；CMainWindow 直连三个新信号到面板；CMakeLists 纳入 4 个新源文件 - ui/CExecutionPanel.h/.cpp, ui/CMessageView.h/.cpp, ui/CMessageBlock.h/.cpp, ui/CMainWindow.cpp, CMakeLists.txt
  - 附带修复 test_projectmanager 链接（CProjectManager 引用 CTaskInfo，测试目标补 CTaskInfo.cpp）；Debug 编译通过(0 警告 0 错误) + ctest 100% 通过 + 启动冒烟无崩溃

[2026-08-11] 修复 VS2022(.vcxproj)构建报一堆“无法解析的外部符号”：CMessageView/CMessageBlock 两个新类(均含 Q_OBJECT)未纳入 AutoCode.vcxproj，导致 CExecutionPanel 链接缺符号 + MOC 没跑。补 QtMoc(ui\CMessageView.h、ui\CMessageBlock.h) + ClCompile(对应 .cpp)，.filters 同步归入 ui 筛选器；MSBuild Debug|x64 编译链接通过。注：CMake 路径本就正常(AUTOMOC 自动包含)，仅 .vcxproj 需手工同步 - AutoCode.vcxproj, AutoCode.vcxproj.filters

[2026-08-11] 环境安装脚本完善：支持后端选择(Anthropic / 智谱GLM) + npm 全局目录统一到 C:\Program Files\npm
  - scripts/install_env.bat：开头 choice 选后端(1=Anthropic 原版 / 2=智谱 GLM)；npm config prefix/cache 指向 C:\Program Files\npm 并 icacls 授 Users 写权限、PowerShell 安全追加系统 PATH(读-判断-写，不覆盖)；设 npmmirror 镜像；装 @anthropic-ai/claude-code；选 GLM 则装 @z_ai/coding-helper(chelper) 并提示用户自备 key 运行 chelper 配置
  - 修正原手动步骤 3 处问题：①去掉多余的「node_modules\npm\bin 加入环境变量」(nodejs.msi 已自动把 nodejs 目录加入 PATH) ②权限只给 C:\Program Files\npm 一处(不再重复给 nodejs 目录授权) ③智谱配置命令确认为 chelper；Node 用 winget LTS 滚动版本(非锁 24.14.0，Claude Code 只要求 Node>=18)
  - 不含 C++ 改动，复用 OnInstallEnv 菜单入口；脚本手工同步到 Execute/Debug/ 便于立即测试，CMake POST_BUILD 保证后续构建自动同步 - scripts/install_env.bat

[2026-08-11] 新增全局任务历史库(SQLite) + 历史任务页多列改造(费用/输入/输出)
  - 新层 db/CTaskHistoryDB(.h/.cpp)：纯 sqlite3 C API 封装，库落 <exe 目录>/history.db(GetModuleFileNameW 取 exe 路径)；Open() 建库建表(14 离散列,不存 stage history 数组)；Upsert(INSERT OR REPLACE,幂等)/LoadAll(按 created_at 倒序)/Delete；prepare_v2+bind_*+step+finalize，TEXT 用 SQLITE_TRANSIENT 安全拷贝
  - 编译集成：sqlite3.c 必须「按 C 编译」(C++ 编译会让定义得 C++ 改名、与 sqlite3.h 的 extern "C" 声明不匹配→链接报无法解析的外部符号)，CMake set LANGUAGE C + /W0；vcxproj 给 sqlite3.c 加 /w(MSBuild 默认 .c→C 编译器)。CMakeLists 增 C 语言 + db 源/include；AutoCode.vcxproj+.filters 同步 db 文件+sqlite3.c+db include+db 筛选器
  - core/CTaskInfo 增 SetUsage/SetStageIter 绝对值 setter(供 DB 恢复,非累加)
  - ui/CTaskListPanel：历史页 QListWidget→QTreeWidget 8 列(任务名称|类型|阶段|状态|费用|输入|输出|创建时间),数值列右对齐、名称列 Stretch;新增 SetHistoryTasks(面板内按 Done/Failed 终态过滤)+OnHistorySelectionChanged;当前表只放非终态,历史表只看 DB 数据;FormatTokens(K/M/G)复用
  - ui/CMainWindow：持有 unique_ptr<CTaskHistoryDB>,构造开库(失败不阻断);OpenProjectDir 加载 task.json 循环里逐条 Upsert(=导入 .autodev/tasks)+末尾 ReloadHistory;SaveTask 镜像写 task.json 后追加 Upsert;OnTaskFinished 进终态后 ReloadHistory;OnDeleteTask 重写按 id 删 DB + 区分活跃/历史副本(活跃删本工程 task.json,历史副本不动)+刷新两表;析构释放历史副本
  - tests/test_historydb.cpp:新增,验证 Upsert/LoadAll 字段往返+幂等+多条+Delete+空 id 安全;ctest 100%(2/2);CMake Debug + MSBuild vcxproj Debug 均编译链接通过(0 错);启动冒烟确认 exe 同级生成合法 history.db(SQLite format 3 头) - db/CTaskHistoryDB.{h,cpp}, core/CTaskInfo.{h,cpp}, ui/CTaskListPanel.{h,cpp}, ui/CMainWindow.{h,cpp}, CMakeLists.txt, AutoCode.vcxproj, AutoCode.vcxproj.filters, tests/test_historydb.cpp

[2026-08-11] 新增 .autodev/tasks 批量导入工具 + 还原 d70e3211/task.json，解决历史库始终为空
  - 根因：CProjectManager::LoadAllTasks 显式跳过无 task.json 的目录，而 d70e3211 早期任务仅留阶段 .md(intake/requirements/design/plan/cr)、无 task.json，故打开工程时从未被导入，history.db 一直空表
  - 还原 .autodev/tasks/d70e3211/task.json：从 intake.md 推断 id/名称/原始需求/类型(Feature)/createdAt，阶段定 Plan(6)，状态定 Done(4)(readme 已确认该「最近打开」功能当日落地，且仅终态 Done/Failed 才进历史页)；cost/in/out 无可恢复数据置 0；UTF-8 无 BOM(与 SaveToFile 输出一致，QJsonDocument 不吃 BOM)
  - 新增 tools/import_tasks.cpp + CMake 目标 import_tasks：扫 <工程目录>/.autodev/tasks/*/task.json 逐条 CTaskInfo::LoadFromFile + CTaskHistoryDB::Upsert 灌入 exe 同级 history.db，末尾 LoadAll 回读校验打印；仅 CMake 目标(开发期工具，不入 vcxproj/主程序)
  - 运行验证：分别置于 Execute/ 与 Execute/Debug/ 各跑一次，Debug 与 Release 两个 AutoCode.exe 同级的历史库均写入 d70e3211，LoadAll 回读 1 条字段往返一致 - tools/import_tasks.cpp, CMakeLists.txt, .autodev/tasks/d70e3211/task.json

[2026-08-11] 执行面板日志区改为 Tab：第一页「日志」(原 CMessageView 对话流)，第二页「阶段产物」
  - Tab2 思路：按选中任务的流程阶段(CStageMachine::GetFlowStages)，列出 .autodev/tasks/<id>/ 下已生成的阶段 .md 产物(intake.md/requirements.md/design.md/plan.md/cr.md 等)，左列清单(阶段中文名+文件名) + 右栏只读内容(QPlainTextEdit, Consolas 等宽)
  - 选中清单项读取文件：按 CLAUDE.md 编码规则 BOM→UTF-8→系统 ANSI(中文 GBK) 兜底；切换任务(SetCurrentTask)同步刷新产物页
  - 仅改 CExecutionPanel.h/.cpp(无新文件、无构建脚本改动)；CMake Debug + MSBuild vcxproj Debug 均 0 错(moc 重跑 CExecutionPanel.h) - ui/CExecutionPanel.h, ui/CExecutionPanel.cpp

[2026-08-12] 修复 install_env.bat 在无 winget 机器上硬性中止的问题（环境已齐却被拦住）
  - 根因：原脚本开头无条件检测 winget，缺失即 goto :END 中止，根本没先看要装的东西是否已在；当前机器(Win10 21H2)无 winget(App Installer 未装)，但 node/git/claude 早已齐全，被误拦
  - 改造：①先检测 node/git/claude(/chelper) 逐项报告，全部已装则直接跳到验证、不触发安装；②winget 缺失不再致命，仅在「确实要装 Node/Git 且无 winget」时走直链下载兜底(Node: nodejs.org index.json 取最新 LTS 的 x64 MSI 静默装；Git: GitHub latest release 取 64-bit 安装包 /VERYSILENT 装)；claude/chelper 本就走 npm、不依赖 winget；③npm 全局目录配置改为仅当确需 npm 安装时才执行，避免改动已可用的现有全局配置
  - 脚本改 UTF-8(无 BOM)+chcp 65001(cmd 正确显示中文)；已实测两条 PS 兜底探测成功(Node LTS v24.19.0 / Git 2.55.0.3-64-bit)；手工同步到 Execute/ 与 Execute/Debug/ - scripts/install_env.bat

[2026-08-12] 修复 Release|x64(Qt5.14.2)编译错误，使代码 Qt5/Qt6 双兼容
  - AutoCode.vcxproj：Debug/Release 两个 ItemDefinitionGroup 显式加 <LanguageStandard>stdcpp17</LanguageStandard>。原 Release 走 Qt5 无 /std:c++17 注入、MSVC 退回 C++14，致 std::optional(CStageMachine.h/CTaskOrchestrator.cpp)解析失败并级联一堆语法错
  - core/CTaskInfo.cpp：inTok/outTok 由 Qt6-only 的 QJsonValue::toInteger() 改为 toVariant().toLongLong()(Qt5/Qt6 双兼容且保 qint64 精度) + 补 #include <QVariant>
  - agent/CClaudeRunner.cpp：补 #include <QVariant>(Qt5 下 toVariant() 返回不完整类型，Qt6 间接拉入故原本能过)
  - core/TaskTypes.h：#if QT_VERSION<6 条件提供 qHash(EStage,uint) 重载(Qt6 对枚举已内置 qHash；Qt5 的 QSet<EStage> 需手动提供，CExecutionPanel.cpp 用到)，并条件 include <QHash>
  - 验证：MSBuild vcxproj Release|x64(Qt5.14.2) 0 错 → Execute/AutoCode.exe；回归 vcxproj Debug|x64 + CMake Debug(Qt6.8.3) 均 0 错，Qt5/Qt6 双兼容 - AutoCode.vcxproj, core/TaskTypes.h, core/CTaskInfo.cpp, agent/CClaudeRunner.cpp

[2026-08-12] 环境安装脚本转 ANSI(GBK)+CRLF + Release|x64 产物直出 Execute/ 根
  - scripts/install_env.bat：去掉 chcp 65001(中文系统 cmd 默认 936 代码页即可正确显示中文)，文件由 UTF-8 转为 ANSI(GBK) 编码 + CRLF 换行(符合 Windows .bat 规范)；同步到 Execute/install_env.bat
  - CMakeLists.txt：AutoCode 目标加 RUNTIME_OUTPUT_DIRECTORY_RELEASE=${CMAKE_CURRENT_SOURCE_DIR}/Execute，覆盖 VS multi-config 默认的 <CONFIG> 子目录行为，使 Release 直出 Execute/ 根(Debug 仍落 Execute/Debug/)
  - 编译 Release|x64 通过：Execute/AutoCode.exe(1.37MB) + Execute/install_env.bat；清理 Execute/Release/ 副产品(test_*/import_tasks)；后续编译建议 cmake --build build --config Release --target AutoCode 仅出主程序 - scripts/install_env.bat, CMakeLists.txt

[2026-08-12] 修复 install_env.bat 在 Win7 的语法错误 + winget 缺失友好提示
  - 根因：if(...) 括号块内的 echo 含半角圆括号「(App Installer)」「(cmd 或 PowerShell)」，cmd 把 ) 当成块结束 → 报“此时不应有」。后重试”；全部改全角（）避免截断块
  - winget 缺失分支增强提示：明确 winget 仅 Win10 1809+、Claude Code 需 Node 18+(仅 Win8.1+)，Windows 7 无法安装并运行 Claude Code，引导升级到 Win10+
  - scripts/install_env.bat 同步转 GBK+CRLF 到 Execute/install_env.bat；不含 C++ 改动 - scripts/install_env.bat

[2026-08-12] 阶段产物查看支持 Markdown 渲染 + 清理 Qt5 兼容遗留（工程转 Qt6 only）
  - Markdown 渲染：新增 ui/CArtifactView（QTextBrowser 子类，封装 .md 产物读取 + 编码兜底 + setMarkdown 渲染，标题/列表/表格/代码块/任务列表生效）；CExecutionPanel「阶段产物」页右栏由 QPlainTextEdit 纯文本改为 CArtifactView，OnOutputSelected 瘦身为单行调用，BOM/GBK 兜底逻辑下沉到 CArtifactView；CMakeLists 纳入新文件 - ui/CArtifactView.h/.cpp(新), ui/CExecutionPanel.h/.cpp, CMakeLists.txt
  - Qt6 only 清理：vcxproj Debug/Release 的 <QtInstall> 均已切 6.8.3_msvc2022_64，源码不再迁就 Qt5。CTaskInfo.cpp/CClaudeRunner.cpp 的 toVariant().toLongLong() 改回 QJsonValue::toInteger() 并删多余 #include <QVariant>；TaskTypes.h 删 #if QT_VERSION<6 的 qHash(EStage) 守卫与条件 <QHash>（Qt6 对枚举已内置 qHash） - core/TaskTypes.h, core/CTaskInfo.cpp, agent/CClaudeRunner.cpp
  - 验证：CMake Debug(Qt6.8.3) 编译链接通过(0 错 0 警)，AUTOMOC 正确处理 CArtifactView 的 Q_OBJECT

[2026-08-12] 修复 6 项日志/UI/执行体验问题（右键菜单漆黑、启动按钮语义、未选任务不显示日志、阶段切换日志丢失、反复提示缺 requirements.md、任务日志归档）
  - #1 日志右键菜单漆黑：main.cpp 给 QApplication 设全局 QMenu 浅色样式表（白底深字+浅蓝选中项），修 Windows 深色应用模式下 QTextBrowser/QPlainTextEdit 默认右键菜单不可读 - main.cpp
  - #2 启动按钮语义：CTaskListPanel 新增 UpdateButtons()，按选中任务状态映射「启动/继续/运行中…」文案与启用态(Pending=启动/Paused,Stopped=继续/Running=运行中并禁用)，选中变化与刷新时同步 - ui/CTaskListPanel.h/.cpp
  - #3 未选任务也显示日志：CMainWindow::OnStartTask 启动后自动 SelectTask+SetCurrentTask；CTaskListPanel 新增 SelectTask() - ui/CMainWindow.cpp, ui/CTaskListPanel.h/.cpp
  - #4 阶段切换日志被清空(核心Bug)：根因 PopulateCurrent 的 clear() 触发 currentItemChanged(nullptr)→OnSelectionChanged 把 m_pCur 置空并 emit sigTaskSelected(nullptr)→执行面板清空（每轮阶段推进必复现）。修复：clear+重建包 QSignalBlocker、先存 m_pCur 再用局部指针 RestoreSelection；OnSelectionChanged 加 nullptr 防御 - ui/CTaskListPanel.cpp
  - #5 反复提示缺 requirements.md：CTaskOrchestrator::RunCurrentStage 加 EnsureStageInputs 阶段输入自愈预检，声明输入缺失则反查生产阶段(sOutput 匹配)回退重跑并发 need_input 事件，guide.md 等无生产者输入跳过；计入 StageIter 复用既有升级机制防死循环 - orch/CTaskOrchestrator.h/.cpp
  - #6 任务日志归档：CProjectManager 新增 AppendTaskLog 落 .autodev/tasks/<id>/run.log(UTF-8 首建 BOM)；CTaskOrchestrator 新增 WriteRunLog(带时间戳分类) + StageEvent(统一事件日志入口，10 处原 emit sigStageEvent 改走此)，在 On* runner 槽与阶段生命周期点写完整流水(system/assistant/tool_use/tool_result/event) - project/CProjectManager.h/.cpp, orch/CTaskOrchestrator.h/.cpp
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj/.filters 无需同步；验证：CMake Debug(Qt6.8.3) + vcxproj Release|x64 均编译链接通过(0 错 0 警)

[2026-08-12] 主窗口位置/大小/最大化态持久化到 exe 同级 config.ini [Window] 段
  - project/CProjectManager.{h,cpp}：新增 SaveWindowGeometry/LoadWindowGeometry（宽字符 WritePrivateProfileStringW/GetPrivateProfileIntW，复用 ConfigFilePath()）；Load 用 INT_MIN 哨兵区分「缺键」与「合法 0」，无文件/缺键返回 false（调用方用默认几何）
  - ui/CMainWindow.{h,cpp}：构造 resize 后调 LoadWindowGeometry + IsGeometryOnScreen 屏幕外回退（换屏/分辨率变化不出屏）恢复几何；新增 closeEvent，最大化态存 normalGeometry()（否则存成铺满全屏尺寸、还原后窗口过大）；无新源文件/Q_OBJECT，CMake/vcxproj 无需改动
  - 附带修复 CProjectManager::AppendTaskLog 的 QIODevice::Raw 编译错误（Qt 无此枚举值；删 |Raw 保留 QIODevice::Append 二进制追加，符合「不开 Text」注释意图）
  - 验证：CMake Release|x64 编译链接通过 → Execute/AutoCode.exe；test_projectmanager 回归无报错 - project/CProjectManager.{h,cpp}, ui/CMainWindow.{h,cpp}


[2026-08-12] 修复评审决策误用过期 review_requirements.md 导致任务被反复驳回
  - orch/CTaskOrchestrator.{h,cpp}：ParseReviewDecision 原无条件读取评审输出文件，上一轮迭代遗留的旧「决策：驳回」（本轮评审未重写）会被误当本轮结论，导致输入已就绪后仍反复驳回、耗尽迭代预算升级暂停
  - 修复：RunCurrentStage 启动阶段前记 m_dtStageStart=QDateTime::currentDateTime()；ParseReviewDecision 改以本轮 result+助手文本为基准，仅当评审输出文件 mtime>=m_dtStageStart（本轮重写）才并入判定；过期文件不再覆盖本轮结论
  - 验证：vcxproj Release|x64 编译 0 错 0 警（链接需先关闭运行中的 AutoCode.exe） - orch/CTaskOrchestrator.{h,cpp}

[2026-08-12] 新增开发可靠性策略全套：max-turns 自动续跑 + 编译校验门 + 动态轮次预算 + 清晰失败诊断 + prompt 护栏（根因：任务 cb9cda31 开发阶段命中 --max-turns 60，subtype=error_max_turns 被当真错误判死整任务 Failed，且 stage_failed 消息为空）
  - 续跑(max-turns) + 编译修复(compile-fix) 共用同一原语 --resume <session_id> + 针对性续跑 prompt，由编排器 ReRunResume() 统一发起；单阶段共享续跑/修复计数 m_iResumeCount 封顶 k_iMaxResume=3，阶段真正推进(ProceedToNext)或评审驳回回退时清零 - orch/CTaskOrchestrator.{h,cpp}
  - 策略1+4 续跑与诊断：CClaudeRunner.StartStage 增可选 a_sResumeSid 尾参(非空则 argv 追加 --resume)；result 分支落 m_sLastSubtype/m_sLastSessionId + 新增 LastSubtype/LastSessionId/LastWasMaxTurns getter；CTaskInfo 增持久化字段 lastSessionId/resumeCount(SaveToFile/LoadFromFile 同步)；Advance 的 !ok 分支前置续跑判定(max-turns 且阶段∈{Develop,BugFix,Test} 且未超上限→resume，否则判死)，stage_failed 消息改 subtype 感知(max-turns→「轮次上限且续跑次数已用尽」，避免空消息) - agent/CClaudeRunner.{h,cpp}, core/CTaskInfo.{h,cpp}, orch/CTaskOrchestrator.{h,cpp}
  - 策略5 编译门：Develop/BugFix 成功后、CodeReview 前跑一次构建(成员 QProcess* m_pBuildProc 异步、不阻塞 UI、不新增 Q_OBJECT 类)。ResolveBuildCommand 优先读 .autodev/build.txt 首行→否则根目录唯一 *.sln 拼 msbuild /p:Configuration=Debug /p:Platform=x64→都无则跳过(compile_skip)；LocateMsBuild 用 PATH→vswhere 兜底；OnBuildFinished 通过→ProceedToNext，失败且有预算→错误尾部喂回 ReRunResume(compile_fail_retry)，预算耗尽→放行下游(compile_fail，不硬判死，下游 CodeReview/Test 继续把关)；StopTask 同步 kill m_pBuildProc，OnBuildFinished 对已停止任务直接吞掉防竞态 - orch/CTaskOrchestrator.{h,cpp}
  - 策略3 动态预算：EstimateDevTurns 读 .autodev/tasks/<id>/plan.md 计任务标识(T\\d+ 与 ##/### 标题)得 iTasks，qBound(40, 40+10*iTasks, 150)；MaxTurnsFor 由静态改为 Develop→EstimateDevTurns、BugFix/Test→80、其余 30 - orch/CTaskOrchestrator.{h,cpp}
  - 策略2 prompt 护栏：prompts/08_develop.md 增「预算与避坑」(轮次预算有限、核心代码优先/非必要工作后置、禁编辑 ReadMe.txt/.rc/.rc2 等 GBK 固定编码文件、变更记录写 {{TASK_DIR}}/changelog.md、乱码记录一行后跳过、每任务 TodoWrite 勾选)；13_bug_fix.md 增同款编码避坑 + 修复后基本编译自测；10_test.md 增「先编译再跑测试、编译失败按代码缺陷驳回」 - prompts/08_develop.md, prompts/13_bug_fix.md, prompts/10_test.md
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj/.filters 无需同步；验证：vcxproj Release|x64 编译链接通过(0 错 0 警) → Execute/AutoCode.exe
[2026-08-12] Execute/ 运行时 Qt5→Qt6 切换：清除 Qt5Core/Gui/Svg/Widgets.dll、libEGL/libGLESv2.dll 及 platforms/styles/imageformats/iconengines 插件（2020 旧 Qt5 件），windeployqt6 --release --no-translations 重部署 Qt6Core/Gui/Widgets/Svg/Network + opengl32sw + D3Dcompiler_47 + 各插件目录；冒烟 AutoCode.exe 启动正常 - Execute/
[2026-08-12] UI/交互增强:提示词助手agent(独立CClaudeRunner会话,日志路由至助手Tab,上下文大小按钮+摘要压缩,fork链撤销最后一条提问) + 当前/历史列表选择粘性(仅空白点击取消) + 失败任务继续执行按钮 + 全部列表/控件字体统一11号 - orch/CTaskOrchestrator.{h,cpp}, agent/CClaudeRunner.{h,cpp}, ui/CExecutionPanel.{h,cpp}, ui/CMainWindow.{h,cpp}, ui/CTaskListPanel.{h,cpp}, ui/CMessageBlock.cpp
[2026-08-12] 助手交互优化4项:①历史页右键"再次执行"(去掉常驻"继续执行"按钮,复用OnReviveTask续跑语义) ②发送提示词后自动激活助手Tab ③输入框QLineEdit→QPlainTextEdit多行(随内容行数自动向上扩展,上限6行,发送后回单行;Ctrl+Enter发送/Enter换行) ④助手运行态标识(发送钮运行中变红色"停止"可点击停止+助手Tab标签加"●"+停止/完成事件块⏹/🎉,打通孤儿信号sigAdhocStop→StopAdhoc链路) - ui/CTaskListPanel.{h,cpp}, ui/CExecutionPanel.{h,cpp}, ui/CMainWindow.cpp, orch/CTaskOrchestrator.cpp
[2026-08-12] 日志区文本选择复制:Event/System/Error块的原QLabel(默认NoTextInteraction不可选中)加Qt::TextSelectableByMouse,与Assistant(QTextBrowser)/ToolUse(QPlainTextEdit)统一支持鼠标选中Ctrl+C复制 - ui/CMessageBlock.cpp
[2026-08-12] 提示词运行时主动提问(方案A MCP工具):助手agent可向用户提问确认/选择/澄清——CClaudeRunner.StartStage增a_bEnableAskUser尾参(ad-hoc传true),argv增--mcp-config<临时.json>+--allowedTools追加mcp__autocode__ask_user+--append-system-prompt;claude -p同进程自动闭环调ask_user→MCP server(AutoCode.exe --mcp-ask-server模式,stdio JSON-RPC newline分帧,暴露ask_user工具)经QLocalSocket(AutoCodeMcpAsk-<PID>)转发到主窗口弹CAskUserDialog→用户作答回tool_result→claude继续;stream-json现有解析自动捕获ask_user的tool_use/tool_result出❓卡片;取消/超时/管道不可用均有友好回灌;新增Qt6::Network依赖(QLocalSocket/QLocalServer属QtNetwork) - agent/CMcpAskServer.{h,cpp}(新), ui/CAskUserDialog.{h,cpp}(新), main.cpp, ui/CMainWindow.{h,cpp}, agent/CClaudeRunner.{h,cpp}, orch/CTaskOrchestrator.cpp, ui/CMessageBlock.cpp, CMakeLists.txt, AutoCode.vcxproj, AutoCode.vcxproj.filters
[2026-08-12] 7项交互问题修复:①助手日志/任务日志持久化(打开工程自动恢复对话上下文:.autodev/assistant.json存sid+撤销栈+助手对话块,.autodev/tasks/<id>/chat.json存任务对话块;sigAdhocBlocksChanged/sigAdhocStateChanged触发写,任务流500ms去抖MarkTaskDirty/FlushTaskChat合并高频输出;切工程前存旧+ClearAllBlocks,加载后LoadAssistantState恢复,选任务懒加载chat.json) ②历史列表空白处点击取消选择(viewport MouseButtonPress判itemAt==null→setCurrentItem(nullptr)强制触发currentItemChanged) ③提示词输入框多行自动向上扩展(OnAdhocTextChanged用QTimer::singleShot(0)延后测documentLayout高度自适应) ④发送后显示助手运行态(DoAdhocSend把SetAdhocRunning(true)移到emit sigAdhocSubmit之前,规避FailedToStart同步链sigFinished覆盖) ⑤助手块乱码修正(CMessageBlock.SetEvent的szColor/szBullet由fromLatin1改fromUtf8正确解码UTF-8字面量;user标记❯→「你」;done事件去重emoji) ⑥任务日志区恢复(OnTaskSelected懒加载chat.json→LoadTaskBlocksFromJson注入缓存后SetCurrentTask的RebuildView渲染) ⑦再次执行失效修复(完成态任务ResetForRevive重置阶段从头跑,失败/停止态仅清运行标志续当前阶段;OnReviveTask把UI选中/显示移到StartTask之前使阶段事件可见,菜单"继续执行"→"再次执行") - ui/CExecutionPanel.{h,cpp}, orch/CTaskOrchestrator.{h,cpp}, project/CProjectManager.{h,cpp}, ui/CMainWindow.{h,cpp}, core/CTaskInfo.{h,cpp}, ui/CTaskListPanel.cpp, ui/CMessageBlock.cpp
[2026-08-12] 2项交互修复:①当前/历史任务列表选择互斥(选中一边时程序性清另一边选中,新增m_bSyncing防重入避免两槽互相触发致日志区显示混乱) ②提示词输入框多行自适应高度修复(根因:textChanged同步点document未按viewport重排,documentLayout()->documentSize()拿不到含软换行的准确高度→改用document->setTextWidth(viewport宽)+size().height()强制重排;上限6行→10行;垂直滚动条策略AlwaysOff→AsNeeded使超10行时显示滚动条) - ui/CTaskListPanel.{h,cpp}, ui/CExecutionPanel.cpp
[2026-08-12] 修复干净 Win10 上 install_env.bat 无法运行：根因 cmd.exe 对「含多字节中文的批处理 + chcp 65001」有按字节算偏移的解析 bug（行被随机截断报 'evel!'/'_GIT!"=="0" set "NEED'/'安装]' 等「半截命令」），且文件曾为 LF 行尾。改回 GBK(ANSI)+CRLF、删除活跃 chcp 65001 命令（仅注释警示），scripts/ 与 Execute/ 同步；字节级校验 od -c 见 \r\n + GBK 高字节、tr 计 241 CR、活跃 chcp 计 0 - scripts/install_env.bat, Execute/install_env.bat
[2026-08-12] Execute/ 自包含（拷贝即可用）：补齐本地 MSVC 运行时 vcruntime140/vcruntime140_1/msvcp140(+msvcp140_1/2/atomic_wait/codecvt_ids/concrt140)，从 System32 同源 VCRedist 拷到 exe 同级（本地部署，DLL 搜索优先 exe 目录），不再依赖目标机预装 VCRedist；Qt6 由 windeployqt6 已打包、SQLite 已静态编入、ucrtbase 为 Win10 自带不打；冒烟启动正常 - Execute/
[2026-08-12] 4项交互修复:①提示词输入框多行高度自适应(根因:documentSizeChanged连在document()->documentLayout()上,该对象在new QPlainTextEdit构造瞬间可能为空,致connect整体跳过、信号从未连上,setFixedHeight仅初始设一次单行高后再不更新→回车多行高度不变;改用QPlainTextEdit::blockCountChanged信号驱动,回车增删行必定触发、值即时准确,不依赖布局时机;OnAdhocTextChanged按blockCount()*lineSpacing线性算高,上限10行超出显示滚动条;输入框高度变化时日志区随QVBoxLayout自动同步收缩) ②按钮状态即时刷新(CTaskOrchestrator进入Running/Paused即emit sigTaskChanged,点"继续"即时禁用防重复点击;状态机梳理:Pending→"启动"可用/Running→"运行中…"禁/Paused·Stopped→"继续"可用/Pause仅Running可用/Stop在Running·Paused·Stopped可用) ③"删除"按钮取消,当前任务右键增"移除"菜单项(确认文案"是否将当前任务移除到历史任务中,如有需求,历史任务可以再次运行";移除=运行中先StopTask→置Stopped→SaveToFile存DB+task.json→从m_aTasks摘除并delete活跃副本,DB记录保留供OnReviveTask重建再次执行;ReloadHistory用QSet过滤活跃任务id防当前表/历史表重复;SetHistoryTasks去掉内部Done/Failed过滤) ④费用文本前增CBusyIndicator自绘动画指示器(任务或助手任一运行→12段旋转弧转圈,空闲→按任务状态色画静态圆点;QPainter+QTimer无Q_OBJECT免工程同步) - ui/CExecutionPanel.{h,cpp}, orch/CTaskOrchestrator.cpp, ui/CTaskListPanel.{h,cpp}, ui/CMainWindow.{h,cpp}
[2026-08-12] 修复干净 Win10「环境安装」菜单不弹窗 + bat 中文不显示：①OnInstallEnv 由 QProcess::startDetached（无 UAC、GUI 进程 detached 启 cmd 窗口不可见）改用 ShellExecuteW "runas"——直接弹 UAC 授权，授权后开可见的已提权 cmd /k 跑脚本；用户拒授权返回<=32 提示手动右键以管理员运行；新增 windows.h+shellapi.h 头(NOMINMAX/WIN32_LEAN_AND_MEAN 守卫) ②install_env.bat 开头加 chcp 936 >nul 强制控制台 GBK 代码页，修目标机默认代码页非 936 时中文花屏（936 非 65001，不触发 cmd 多字节批处理截断 bug）；Release|x64 编译通过 + 冒烟启动正常 - ui/CMainWindow.cpp, scripts/install_env.bat, Execute/install_env.bat
[2026-08-12] install_env.bat 去硬依赖 winget：Node/Git 改「winget --source winget(避开 msstore 源协议 0x8a15000f)→装后 where 验证→失败自动转直链下载兜底」三段式；RefreshPath 由覆盖改追加初始进程 PATH(_ORIG_PATH) 防提权后 HKCU 变管理员账户丢掉原用户 PATH(含 winget 的 WindowsApps)致第二次 winget 报「不是内部命令」；InstallNodeDl/InstallGitDl 强制 [Net.ServicePointManager]::SecurityProtocol=Tls12 兼容旧 PowerShell 的 GitHub/nodejs TLS；GBK+CRLF 同步 scripts/ 与 Execute/ - scripts/install_env.bat, Execute/install_env.bat

[2026-08-12] 修复 CodeReview↔Develop 驳回死循环（任务 cb9cda31 同一问题反复驳回 3 次至 k_iMaxIter 熔断）：根因编排器从不把驳回理由回流给回炉的 Develop，回炉 prompt 与首次完全一致→原样重写→再驳回直至熔断 - orch/CTaskOrchestrator.{h,cpp}, prompts/08_develop.md, prompts/09_code_review.md, prompts/13_bug_fix.md
  - 驳回反馈闭环：BuildPrompt 增 {{ITER}} 注入；Develop/BugFix 直读 .autodev/tasks/<id>/cr.md，包成「## 上轮评审驳回项（必须逐条解决）」注入 {{REJECT_FEEDBACK}}（不走 aInputs，避免 EnsureStageInputs 首次进 Develop 时因 cr.md 不存在误跳 CodeReview）
  - 决策解析三层重写：ParseReviewDecision 由关键词全文本子串匹配改为①行锚定正则取最后一条「决策：xxx」②无决策行时末尾 1500 字查「驳回/reject」独立词③默认 Pass；消除「无不通过项」「未通过的检查项为零」被误判 Reject 的假驳回
  - 事件带阻塞摘要 + 熔断升级报告：新增 ExtractRejectDigest（cr.md 去决策行后正文尾部 300 字），reject/escalation 事件拼「阻塞摘要」；熔断时 WriteEscalationReport 写 escalation.md（任务/阶段/迭代数/驳回焦点/最近 5 条阶段历史/建议）替代原空泛日志
  - prompt 迭代感知收敛：08_develop.md/13_bug_fix.md 增 {{ITER}}+{{REJECT_FEEDBACK}} 段（iter≥1 优先逐条修上轮 critical/major、不重写已通过部分、最终逐条对照）；09_code_review.md 增 iter≥1 聚焦上轮阻塞是否解决+新 critical、已通过不再驳回、minor/nit 不作驳回依据
  - 未新增源文件/Q_OBJECT 类，CMake/vcxproj/.filters 无需同步；验证：vcxproj Release|x64 编译链接通过(0 错 0 警) → Execute/AutoCode.exe
[2026-08-12] 助手上下文窗口按模型动态化:原"X / 200K"分母200K写死、与实际模型无关——现CClaudeRunner解析stream-json result的model字段(新增m_sLastModel+LastModel getter,与LastContextFill对称,不改sigFinished签名),编排器新增sigAdhocModel信号在助手回复完成/压缩完成时emit模型名,面板OnAdhocModel收到后查ContextLimitForModel(glm→128K,claude→200K,未知回退200K)更新分母,UpdateContextButton统一出文案(filled/limit)+tooltip带真实模型名便于核对;SetAdhocContext改为记m_iLastContextFill调UpdateContextButton - agent/CClaudeRunner.{h,cpp}, orch/CTaskOrchestrator.{h,cpp}, ui/CExecutionPanel.{h,cpp}, ui/CMainWindow.cpp
[2026-08-12] 修复已完成/失败任务不进历史任务列表（tankwar 等终态任务在两表间凭空消失）：根因有二——①OnTaskFinished 仅刷新两表，终态任务仍留在 m_aTasks，而 ReloadHistory 按活跃集合(QSet<id>)把它从历史表过滤、当前表又只放非终态，遂两表都看不到；②OpenProjectDir 加载循环把所有 task.json（含 Done/Failed）都塞进 m_aTasks，上个会话已完成的任务（如 tankwar/cb9cda31）一打开工程就重现①。修复：OnTaskFinished 对 Done/Failed 走 OnRemoveTask 同款归档（摘 m_aTasks→清 m_pCur+执行面板→delete 活跃副本→SetTasks+ReloadHistory；DB+task.json 留底供 OnReviveTask 再次执行）；加载循环对 Done/Failed 只 Upsert 入库+delete 副本、不进活跃列表（再次执行经 OnReviveTask 由 DB 副本重建 task.json，不依赖此处载入）；Paused/Stopped 仍进活跃可续跑，不动。验证：vcxproj Release|x64 编译链接通过 → Execute/AutoCode.exe - ui/CMainWindow.cpp
[2026-08-13] guide.md 扫描时机由「打开工程」迁移至「启动任务」(全自动)：打开工程不再弹窗询问是否扫描(状态栏措辞改「缺失（启动任务时自动生成）」)；启动任务时(OnStartTask)若 HasGuide()==false 则作为初始任务自动调 OnGenerateGuide 扫描生成(任务日志区写 init_guide 事件可见)，生成期间任务保持 Paused，OnGuideFinished 收到完成回调后自动 StartTask 续跑登记在 m_pTaskPendingGuide(QPointer<CTaskInfo>，任务被销毁自动置空防悬垂)的任务；生成失败则任务保持暂停可重试；菜单「生成/刷新 Guide」手动触发的路径保持原弹完成框行为 - ui/CMainWindow.{h,cpp}
[2026-08-13] 修复 CodeReview 阶段 max-turns 误判整任务 Failed（任务 be3fd900 代码审查阶段命中轮次上限被判死 status=5）：根因三层——①编译门结论未传给 CodeReview（本工程还 compile_skip 无权威构建结论），agent 自行 grep run.log 统计 error LNK 等开发历史中间态、自我怀疑耗光轮次；②评审工作目录=工程根可翻 run.log/task.json，prompt 依赖 Bash(git diff) 但工具集无 Bash；③CodeReview 不在 IsResumeEligible 续跑白名单，max-turns 直接判死整任务
  - (A) 编译门结论持久化+注入：RunCompileGate/OnBuildFinished 落 .autodev/tasks/<id>/build_verdict.md(pass/fail/skip+时间+命令+退出码+失败尾部)；新增 WriteBuildVerdict/ReadBuildVerdict+m_sLastBuildCmd(解析时赋值,OnBuildFinished 复用)；BuildPrompt 对 CodeReview/Test 读 verdict 经 {{BUILD_VERDICT}} 软锚点注入「上游编译门结论」，消除 agent 翻日志验构建的动机
  - (B) Develop 落盘变更清单：08_develop.md 输出段要求落 {{TASK_DIR}}/changed_files.md(每行 +/M/- <相对工程根路径>)；09_code_review.md 输入改读 changed_files.md、删 Bash(git diff) 依赖(本阶段工具集无 Bash)
  - (C) 评审 max-turns 降级兜底(核心)：新增 IsLightReviewStage(ReviewReq/ReviewDesign/ReviewPlan/RootCauseVerify/CodeReview，排除 Test——Test 是真验证门保留续跑+Failed)+ReviewResumeHint(催立即落盘结论续跑提示)+k_iMaxReviewResume=1；Advance 轻量评审 max-turns 先续跑 1 次→仍不收敛则 ProceedToNext 降级(ParseReviewDecision 按落盘结论判定，无结论 Tier3 默认 Pass)，不判死；无环路(降级是终态，Pass→前进/Reject→受 k_iMaxIter 限→escalation)
  - (D) prompt/工具对齐：09 删 Bash 依赖、加禁止 grep run.log/task.json 推断构建态条款(这些是开发历史中间态叙述不代表当前产物)；10_test.md 改「先看编译门结论」(pass 跳过重编直接跑测试/fail·skip 才自编，编译失败按代码缺陷驳回退回开发)+加同款禁止条款
  - (E) Advance sWhy subtype 感知：原固定 .arg(k_iMaxResume) 显示“(3)”误导(本次 CodeReview ResumeCount=0 却显示 3)，改为按 ResumeCount>0/是否续跑显示真实原因
  - (F) CodeReview 轮次预算 30→40(MaxTurnsFor 增 case，审查多文件需更多预算，C 降级兜底防判死)
  - 相关发现(不在本次范围，建议后续单独处理)：--allowedTools 执行缺口(run.log 显示 CodeReview 工具集无 Bash 时仍跑了 Bash grep run.log，疑 --dangerously-skip-permissions 覆盖白名单，需查 CClaudeRunner.cpp)；ResolveBuildCommand 仅探测 .sln+msbuild 不识本工程 cmake preset 致 compile_skip，建议 .autodev/build.txt 配 cmake 命令让编译门真正生效
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj/.filters 无需同步；验证：cmake qt6-release Release|x64 编译链接通过(0 错) → Execute/AutoCode.exe - orch/CTaskOrchestrator.{h,cpp}, prompts/08_develop.md, prompts/09_code_review.md, prompts/10_test.md
[2026-08-13] 修复工具白名单在 --dangerously-skip-permissions 下完全失效（深挖任务 be3fd900 根因）：原用 --allowedTools 白名单限制各阶段工具，但 bypass 模式「Bypass all permission checks」绕过所有权限检查，白名单形同虚设——实测 CodeReview(ToolsDocs 无 Bash) 仍跑了 7 次 Bash(含 3 次 grep run.log 找构建结果)；实测 --disallowedTools Bash 后 agent 明确报告无 Bash 工具(deny 优先级 > bypass > allow，deny 在 bypass 下仍生效)。系统性影响所有阶段工具隔离
  - 反转设计：sTools 字段语义由「允许的工具」改为「禁用的工具」(deny 黑名单)；CClaudeRunner argv 由 --allowedTools 改为 --disallowedTools(空串时不加参数=Develop/Verify 全开)；ask_user MCP 工具不再追加白名单(deny 模式默认可用)
  - StageDef 四函数反转：ToolsDocs 禁 "Bash,Edit"(读+写报告，不改代码不跑命令)；ToolsTest 禁 "Edit"(可跑命令写测试，不改业务代码)；ToolsDev/ToolsVerify 禁 ""(全开)
  - ad-hoc 助手：StartAdhocStage readOnly 由 allow "Read,Grep,Glob" 改 deny "Bash,Edit,Write"(只读分析)；非 readOnly 沿用 ToolsDev()(现返回空串=全开)
  - 参数更名：CClaudeRunner StartStage a_sAllowedTools→a_sDisallowedTools(.h+.cpp+注释)；CTaskOrchestrator.h m_bAdhocReadOnly 注释更新
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj/.filters 无需同步；验证：cmake qt6-release Release|x64 编译链接通过(0 错) → Execute/AutoCode.exe - core/StageDef.{h,cpp}, agent/CClaudeRunner.{h,cpp}, orch/CTaskOrchestrator.{h,cpp}
[2026-08-13] 三项改进：①构建系统统一到 CMake（删根目录手维护 AutoCode.sln/.vcxproj/.filters，VS2022 改开 build/AutoCode.sln；编译门 ResolveBuildCommand 新增 CMakeLists.txt 探测分支——读 CMakePresets.json 首个 buildPreset 走 cmake --preset/--build --preset，无 presets 退回 cmake -S -B build --config Release，置于 .sln 分支前；CMakePresets.json buildPreset 补 configuration 字段）②guide.md 生成扫描顺序优先文本文件（.md/.txt/.h/.cpp），非文本不逐个打开 ③ad-hoc 助手首次新会话(m_sAdhocSid 空)自动注入 guide.md 全文到首问，让助手创建即认识项目（撤销栈存原始提问不污染）
  - 未新增源文件/Q_OBJECT 类；验证：cmake qt6-release Release|x64 编译链接通过(0 错) → Execute/AutoCode.exe - orch/CTaskOrchestrator.{h,cpp}, project/CProjectManager.cpp, CMakePresets.json（+删除 AutoCode.sln/.vcxproj/.filters）
[2026-08-13] 清理工程冗余文件（移入 bak/，可恢复）：①Qt 向导脚手架空文件 AutoCode.cpp/.h/.ui/.qrc（CMakeLists 不引用，已被 main.cpp+ui/ 取代）②孤儿 AutoCode.sln（指向已删的 AutoCode.vcxproj，VS2022 现改开 build/AutoCode.sln）+ AutoCode.vcxproj.user ③旧 VS 构建产物 AutoCode/x64/（→bak/AutoCode-build-legacy、x64-build-legacy）④IDE 缓存 .vs/（→bak/.vs-cache，VS 下次打开自动重建）⑤误创建的 nul 文件（→bak/nul.txt）。Execute - 副本/ 按用户要求保留不动。子目录(agent/core/db/doc/orch/project/prompts/scripts/tests/tools/ui)无空文件/异常扩展名/残留
[2026-08-13] 修复主窗口系统标题栏与自绘 CTitleBar 重复显示：①构造函数 setWindowFlags(Qt::Window|Qt::FramelessWindowHint) 去掉系统原生标题栏（此前漏设，系统标题栏与自绘标题栏上下叠放重复）②DwmExtendFrameIntoClientArea 保留 1px 玻璃边框补 Win10/11 系统阴影（frameless 窗口否则无投影贴桌面）③nativeEvent 最大化时跳过边缘缩放避免误触 ④CTitleBar 配色加深提升可读性（背景 #2b2b2b→#1f1f1f、文字 #e0e0e0→纯白加粗、按钮 40x28→46x32 字号 10→12、补 pressed 态） - ui/CMainWindow.cpp, ui/CTitleBar.cpp, CMakeLists.txt(+链接 dwmapi)
[2026-08-13] 修复标题栏/菜单栏对比度低 + 加 frameless 窗口描边：①根因 CTitleBar/centralWidget 为 QWidget 子类却缺 WA_StyledBackground，致 stylesheet 的 background 不绘制（深色底透明、露出父级浅色底，白字反而不清晰）——两者补 setAttribute(Qt::WA_StyledBackground) ②frameless 窗口加 1px 深色描边（centralWidget 设 #AppFrame 深色底 + layout contentsMargins(1,1,1,1) 外留白充当边框）③main.cpp 全局 stylesheet 显式设 QMenuBar 浅底深字（系统深色模式下原生菜单栏深底灰字不可读，与 QMenu 统一） - ui/CTitleBar.cpp, ui/CMainWindow.cpp, main.cpp
[2026-08-13] 修正窗口描边方案：上版用 centralWidget 深色底+1px外留白当边框，但子面板(CTaskListPanel/CExecutionPanel)默认透明会透出深色底把整个内容区染黑。改为 CMainWindow::paintEvent 自绘 1px 深色描边——撤销 centralWidget 的 WA_StyledBackground/stylesheet/margin(恢复 0,0,0,0)，内容区背景完全交回 QMainWindow 默认浅色 palette；paintEvent 先调基类画背景再仅 drawRect 画 1px #1f1f1f 边框线(adjusted(0,0,-1,-1) 防 1px 画到像素外，不填充)，界面保持浅色只多一圈深色外框 - ui/CMainWindow.{h,cpp}
[2026-08-13] 标题栏配色由深色(#1f1f1f 深底白字)改为浅色(白底 #1f1f1f 深字)与浅色主体统一：CTitleBar stylesheet background→#ffffff、QLabel/QToolButton color→深色(#1f1f1f/#333333)、hover→浅灰 #e5e5e5、关闭键保持红底白字(Windows 惯例)，补 border-bottom 1px #e0e0e0 分隔内容区 - ui/CTitleBar.cpp
[2026-08-13] 新增 FR-6 代码回退(备份/恢复/版本清单)单元测试 test_rollback：覆盖 AC-6.1(全量快照+序号递增+排除清单防递归)/AC-6.2(首次无summary、SetBackupSummary回填)/ListBackups(倒序解析)/AC-6.4+AC-N(RestoreSources恢复正确+排除目录保留+_restore_tmp中转清理+非法入参安全失败)，共17用例全过；CMakeLists.txt 登记新测试目标 - tests/test_rollback.cpp, CMakeLists.txt(+test_rollback)
[2026-08-13] 新增迭代熔断 escalation 模态弹窗提醒：原 escalation(开发-评审循环超 k_iMaxIter=3 次仍被驳回)在 OnStageEvent 只往执行面板追加一条 ⚠ + 状态栏一行，用户不盯日志就错过人工决策时机。现 OnStageEvent 拦截 a_sKind=="escalation" 调新方法 OnEscalation：读 .autodev/tasks/<id>/escalation.md 作正文(缺失退回事件文本,超 1200 字截断)，QMessageBox(Warning) 三按钮——「打开任务目录」(QDesktopServices 打开 TaskDir 供查 cr.md/escalation.md) / 「重置迭代并继续」(ResetStageIter+SetPauseRequested(false)+OnStartTask 从当前阶段续跑,再给完整 N 次) / 「忽略」(保持 Paused)；弹窗用 s_kiMsgPt 放大字体。注:ask_user 提问弹窗机制本就存在,任务 3ae951dd 的 agent 全程未调 ask_user(chat.json 里 ask_user 字样是 main.cpp 源码注释被读文件记录),卡住实为 escalation 熔断而非提问未弹 - ui/CMainWindow.{h,cpp}
[2026-08-13] 六项改进（用户反馈批次）：
  - ①工程菜单删「生成/刷新 Guide」项（OnGenerateGuide 函数保留，启动任务自动扫描仍调用，只是去掉手动菜单入口） - ui/CMainWindow.cpp
  - ②备份文件用 zstd 压缩：vendor third_party/zstd/(libzstd_static.lib 1.5.5 x64 + zstd.h/zstd_errors.h/zdict.h)，CProjectManager.cpp 新增 ZstdCompressFile/ZstdDecompressFile(ZSTD_compress level3 / ZSTD_getFrameContentSize+ZSTD_decompress，均守 INT_MAX 上界 + 校验 write 字节数防磁盘满残缺) + CompressTree(逐文件→<file>.zst) + RestoreTree(.zst 解压回原名、旧版未压缩文件直接拷回兼容)；BackupSources 由 MirrorTree 改 CompressTree、RestoreSources 步骤2 改 RestoreTree（_restore_tmp 回滚路径仍走未压缩 MirrorTree 不变）；CMakeLists 三目标(AutoCode/test_projectmanager/test_rollback) 加 ZSTD_INCLUDE_DIR+ZSTD_LIB；test_rollback 改判 real.cpp.zst - project/CProjectManager.cpp, CMakeLists.txt, tests/test_rollback.cpp, third_party/zstd/*
  - ③介入输入框与助手输入框统一高度：抽 static AdjustInputHeight(QPlainTextEdit*,maxLines)（按字体度量算单行高、blockCount 线性增长、封顶 10 行），介入栏去 setFixedHeight(40) 改接同款 blockCountChanged+AdjustInputHeight(k_iTaskMaxLines=10)，助手栏 OnAdhocTextChanged 改调同函数(k_iAdhocMaxLines=10)，两框初始高与增长行为完全一致 - ui/CExecutionPanel.{h,cpp}
  - ④左右主分隔条空隙过大：水平 QSplitter setHandleWidth(5) + setChildrenCollapsible(false)（消除「当前任务」与「阶段」间过宽拖动条，5px 够拖） - ui/CMainWindow.cpp
  - ⑤助手日志提示词导引：助手工具条 m_pBtnContext 与 m_pBtnUndo 间加 ◀ ▶ 两个 QPushButton(fixedWidth 32) + 「提问 N/M」QLabel；UpdatePromptNav 从 m_aAdhocBlocks 重建用户提问下标索引(Event && sEventKind=="user")、游标默认指最新、刷新文案与按钮 enabled；OnPrevPrompt/OnNextPrompt 调整游标后 m_pAdhocView->ensureWidgetVisible(m_aAdhocWidgets[iBlock]) 滚动定位；AppendAdhocBlock(仅提问块)/RebuildAdhocView 触发刷新 - ui/CExecutionPanel.{h,cpp}
  - ⑥助手上下文跨启动保存：存盘机制(SaveAssistantState/LoadAssistantState 落 assistant.json)本已完整，缺的是启动时不自动打开最近工程→LoadAssistantState 没机会跑；构造函数末尾读 RecentProjects().first，存在且目录在则 OpenProjectDir 打开它（触发既有的 LoadAssistantState 恢复 sid+stack+blocks，助手续上次上下文） - ui/CMainWindow.cpp
  - 未新增源文件/Q_OBJECT 类（third_party 为预编译静态库+头，非源文件）；CMake 三目标加 zstd 链接已同步；验证：cmake qt6-release Release|x64 编译链接通过(0 错) → Execute/AutoCode.exe；ctest 5 套全过(test_rollback 验证 zstd 压缩→解压往返)
[2026-08-14] 修复「当前任务」与「阶段」间空隙仍过大（#4 补刀）：上版只设了 QSplitter::setHandleWidth(5)，但两栏顶层布局 new QVBoxLayout(this) 未设 contentsMargins、走 Qt 默认 ~9px，实际可视间隙 = 左栏右边距9 + 分隔条5 + 右栏左边距9 ≈ 23px。现两栏分别清零朝向分隔条的那一侧边距（CTaskListPanel 清右、CExecutionPanel 清左，其余三边保留默认留白），间隙收窄到只剩分隔条 5px。注意：本次发现用户一直在跑旧进程(PID 59544)未重启，已 taskkill 后重建 - ui/CTaskListPanel.cpp, ui/CExecutionPanel.cpp
[2026-08-14] 修复 FR-3 助手日志提示词按钮等编译错误并补全布局持久化：补 CExecutionPanel.cpp 中 FormatTokens 函数头(参数 qint64，与 CTaskListPanel 同口径)、补 CExecutionPanel.h 中 CreatePromptButton() 声明；CMainWindow::closeEvent 补 SaveSplitterState 使左右主分隔条比例随窗口几何一并持久化(需求4) - ui/CExecutionPanel.cpp, ui/CExecutionPanel.h, ui/CMainWindow.cpp
[2026-08-14] 需求5对齐修正：阶段表底部改为对齐任务列表(Tab)底部(不含按钮行)——CTaskListPanel::resizeEvent 上报 sigTaskListBottom → CExecutionPanel::SyncStagesHeight 动态 setFixedHeight 同步，随字体/DPI 自适应，取代写死的 setMinimumHeight(220) - ui/CTaskListPanel.h, ui/CTaskListPanel.cpp, ui/CExecutionPanel.h, ui/CExecutionPanel.cpp, ui/CMainWindow.cpp
[2026-08-14] 上下文按钮改进度条 + 助手日志关横向滚动条（用户反馈三需求）：
  - 需求1+2 上下文按钮：m_pBtnContext 由 QPushButton 改为 QProgressBar，去掉 autocompact 阈值档位右键菜单(50/100/150/200/300k+自定义)与左键「设阈值」菜单（阈值不再在 UI 暴露，内部沿用 config.ini 默认值维持自动压缩，sigAdhocAutoCompactChanged 信号保留但不再 emit）；本体 setTextVisible(false) 只画进度，占比 = fill/limit，≤70% 绿色(#22c55e)、>70% 红色(#ef4444)，脏数据(fill>窗口)钳满档红并标注「（异常）」；当前值/最大值/百分比/模型名全走 tooltip；右键仅留「立即压缩上下文」。UpdateContextButton 重写为按浮点 dRatio 算占比+动态切换 chunk 颜色+出 tooltip。CExecutionPanel.h 成员类型+前向声明同步
  - 需求3 助手/任务日志区：CMessageView 构造增 setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff)（CMessageBlock 内 QTextBrowser/QLabel 早已开 wordWrap/WidgetWidth，横向滚动条源自 QScrollArea 本体，关掉即按视口自动换行）
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj 无需同步；验证：cmake qt6-release Release|x64 编译链接通过(0 错) → Execute/AutoCode.exe - ui/CExecutionPanel.h, ui/CExecutionPanel.cpp, ui/CMessageView.cpp
[2026-08-14] 用户反馈五项（四项已实现 + guide 同步 agent 待确认）：
  - 需求5 提示词按钮避让滚动条：助手日志右上角半透明按钮 X 偏移由 -6 改 -11（左移 5px，让出 10px 宽竖直滚动条），两处定位（currentChanged lambda + eventFilter resize 分支）同步 - ui/CExecutionPanel.cpp
  - 需求2 助手块自动换行修复：根因 setMarkdown 早于块获得真实宽度，slotAdjustBrowserHeight 的 setTextWidth 用 0/默认宽度 → 长行不折行、关掉横向滚动条后被裁出右沿。CMessageBlock 补 resizeEvent（块宽度变化→重排换行+重算高度，仅 Assistant 块）+ slotAdjustBrowserHeight 加 m_bAdjusting 重入守卫防 setFixedHeight→resize 回环；头补 QResizeEvent 前向声明 + m_bAdjusting 成员 - ui/CMessageBlock.h, ui/CMessageBlock.cpp
  - 需求1 上下文大小设置+持久化：m_pBtnContext 右键菜单增「设置上下文大小…」（QInputDialog getInt，千 token 范围 4~2000）→ SetUserContextLimit(qint64) 覆盖按模型探测（m_iUserContextLimit>0 生效，0=自动）；面板新信号 sigAdhocContextLimitChanged → MainWindow → CProjectManager::SaveAdhocContextLimit 落 config.ini [Adhoc] ContextLimit；启动 LoadAdhocContextLimit→SetUserContextLimit 回填；OnAdhocModel/SetUserContextLimit 均按用户上限优先。ContextLimit 用 GetPrivateProfileStringW+toLongLong（qint64，超 int 范围安全） - ui/CExecutionPanel.h, ui/CExecutionPanel.cpp, project/CProjectManager.{h,cpp}, ui/CMainWindow.cpp
  - 需求3 guide.md 投喂策略改为变更检测：原仅 m_sAdhocSid 空（首次）投喂，现改为「首次 OR guide.md 自上次投喂后哈希变化」才投喂（SHA256 内容哈希，首次/更新两种提示文案）；m_sLastGuideHash 随 assistant.json 持久化（SaveAssistantState/LoadAssistantState 增 guideHash 字段），变更时 emit sigAdhocStateChanged 触发存盘。确认：初始化投喂→后续打开工程仅恢复上下文不投喂→guide.md 改动后自动重投 - orch/CTaskOrchestrator.{h,cpp}, ui/CMainWindow.cpp
  - 需求4 guide.md 同步 agent（已实现，用户定方案=专用 agent 增量更新，触发=任务 Done 后 + 仅手动菜单）：CProjectManager 增 SyncGuideAsync/OnGuideSyncRunnerFinished/FindLatestChangedFiles + m_pGuideSyncRunner 独立 CClaudeRunner（与生成路径 m_pGuideRunner 物理隔离，避免 OnGuideFinished 自动续跑待启动任务的误触发）；按 changed_files.md（指定任务→该任务清单，手动→tasks/*/changed_files.md 取 mtime 最新）内联 prompt 就地 Edit 更新 guide.md（deny "Bash,Write"，放行 Read/Edit/MultiEdit；不新建 prompts/14 md）；触发点 OnTaskFinished Done 分支（delete 前捕获 sDoneId→SyncGuideAsync）+ 工程「同步 Guide」手动菜单（OnSyncGuide/OnGuideSyncFinished 仅刷新状态栏）；sigGuideSyncFinished 信号连 MainWindow。无清单/无 guide/仍在跑→发 sigGuideSyncFinished(false) 安全返回 - project/CProjectManager.{h,cpp}, ui/CMainWindow.{h,cpp}
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj 无需同步；验证：cmake qt6-release Release|x64 编译链接通过(0 错，中遇 LNK1104 瞬时占用重试即过) → Execute/AutoCode.exe - ui/CExecutionBlock 无 / ui/CMessageBlock.{h,cpp}, ui/CExecutionPanel.{h,cpp}, project/CProjectManager.{h,cpp}, orch/CTaskOrchestrator.{h,cpp}, ui/CMainWindow.cpp
[2026-08-14] 打开工程恢复上次助手会话上下文占用（进度条不再恒显0）：CExecutionPanel 加 LastContextFill() getter；assistant.json 增 ctxFill 字段，SaveAssistantState 存 LastContextFill 快照，LoadAssistantState 按 ctxFill 调 SetAdhocContext 还原（旧存档无此字段且 sid 非空时标 -1=未知「—」，避免误显0）；ctxFill 经 double 存取（token 数 << 2^53 精确无损），向后兼容。本次按用户要求仅改代码未编译 - ui/CMainWindow.cpp, ui/CExecutionPanel.h
[2026-08-14] 堵任务抢占口子 + 评审多视角并行（提升评审墙钟效率）：
  - 阶段一·堵抢占口子：原 StartTask/OnStartTask 无 Running 拦截，A 运行中启动 B 会覆盖 m_pCurTask 导致 A 回调串扰/孤儿。orchestrator 增 IsRunnerBusy()（查主 runner + 评审 runner 集合）；StartTask 入口 IsRunnerBusy() 且非当前任务→置 Paused + stage_event("busy") 早返回兜底；CMainWindow::OnStartTask 开头同条件弹框拒绝。安全网，独立可交付。
  - 阶段二·评审并行 fan-out/fan-in：5 个轻量评审阶段（ReviewReq/ReviewDesign/ReviewPlan/CodeReview/RootCauseVerify，IsLightReviewStage）默认开 N=3 视角并行。RunCurrentStage 分流：m_iReviewParallel>1 && IsLightReviewStage→StartReviewParallel，否则原单 runner 路径（完全向后兼容，viewId 空）。fan-out 建 N 个 CClaudeRunner（parent=this Qt 托管），各分 viewId(v1..vN)、各写独立 <out>_v<k>.md 防并发写冲突，Lambda 捕获 viewId 转发对话流信号（遵循 OnAdhoc* 纪律：不读 m_pCurTask、不调 Advance，并发零串扰）；prompt 复用现有评审模板（同构多投票）+ 注入「第 k/N 位独立评审员」+ 输出文件覆盖指引。fan-in：OnReviewViewFinished 缓存结果 + 累加各自 cost，全部完成 FanInReview 聚合（复用 Tier1 正则逐视角提取「决策：通过/驳回」，一票 Reject 即 Reject，无决策=弃权不否决）、合并正文写回 sOutput（保 cr.md 回灌闭环格式）+ 聚合决策行、RecordSummary/AppendHistory、FinishReviewStage（Reject→IncStageIter/RejectTarget/熔断升级，Pass→NextStage）。StopReviewRunners 纳入 StopTask/StopAll 防进程泄漏。
  - 信号 viewId 贯穿（轻量 UI）：sigAssistantText/sigToolUse/sigToolResult 末尾加 const QString& a_sViewId=QString() 默认参（单 runner 路径 emit 不带→走默认，向后兼容）；CExecutionPanel 三槽同参、SBlockData 加 sViewId、BlockToJson/FromJson 同步、OnToolResult 回填改按 sViewId 过滤修并发串配、OnAssistantText 非 viewId 块加「【视角 vX】」前缀标签。MainWindow 信号直连链路无需改（Qt 自动全量透传匹配参）。
  - 配置：CProjectManager 增 SaveReviewParallel/LoadReviewParallel（[Review] Parallel，默认 3，钳 1~8，GetPrivateProfileIntW/WritePrivateProfileStringW）；SetReviewParallel + MainWindow 初始化回填。review_parallel=1 一键回退完全串行、零回归。
  - 成本提示：默认 N=3 意味评审环节 token 成本 ×3（省墙钟不省钱），保留 =1 回退。
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj 无需同步；验证：cmake qt6-release Release|x64 编译链接通过（0 错 0 警）→ Execute/AutoCode.exe - orch/CTaskOrchestrator.h, orch/CTaskOrchestrator.cpp, project/CProjectManager.h, project/CProjectManager.cpp, ui/CExecutionPanel.h, ui/CExecutionPanel.cpp, ui/CMainWindow.cpp
[2026-08-14] 需求4 升级：guide.md 同步改为 record.md 水位线方案（替代 changed_files.md 触发，经四轮兼容性审查定稿）：
  - 水位线机制：新增 .autodev/guide_sync.json {lastSyncLines}，CProjectManager 增 GuideSyncFile/LoadGuideSyncLines/SaveGuideSyncLines；SyncGuideAsync 改为读 record.md 全文→split 非空行→取水位线之后的新增行作增量输入（行数水位线，无时间戳歧义），无新行则发 sigGuideSyncFinished(false,"已最新") 静默跳过不起 agent；StartStage 补 autocompactK=LoadAdhocAutoCompact()（兜底大上下文）；prompt 改为按 record 摘要增量更新 guide 正文，明确禁写 record.md、禁动指纹表段（用 marker 名）。a_sTaskId 参数保留兼容但不再依赖（record 全局流水）。
  - 水位线推进：OnGuideSyncRunnerFinished 成功时 AppendRecord 同步日志后重读 record 行数 SaveGuideSyncLines（含日志行，下次仅对新改动触发），失败不推进以便重试。
  - 助手自治写 record（用户定方案A）：SubmitAdhoc 非只读时给 prompt 追加强制规则——实际改源码必须向 record.md 追加一行（[时间] 助手 | 摘要），明确要求用 Edit 追加、禁 Write 覆盖（防丢历史）；sTools 非只读=ToolsDev 全开已放行 Write/Edit。
  - 助手触发同步：OnAdhocFinished Submitting 成功分支，!m_bAdhocReadOnly 时调 SyncGuideAsync(QString())（内部按水位线判定，无新改动静默）。触发点=任务Done + 助手非只读提交后（砍掉「打开工程触发」，消除与初始任务 BuildGuide/SyncGuideIncremental 的并发窗口）。
  - 协调1（防 read-modify-write 丢更新）：SyncGuideIncremental 入口加 m_pGuideSyncRunner->IsRunning() 守卫——WriteIncremental 是读全文→改指纹段→rename 整文件覆盖，与 guide-sync agent 的 Edit 并发会丢更新，跳过本轮下次任务启动补上（本函数尽力而为）。
  - 协调2（防全量重生成后多余同步）：OnGuideRunnerFinished 成功分支重置水位线=当前 record 行数，避免 BuildGuide 后首次触发把全部历史 record 喂进同步 agent。
  - 兼容性审查结论：guide.md 写入者仅 A(BuildGuide)/B(SyncGuideIncremental 指纹段)/C(SyncGuideAsync 正文段) 三者，段落隔离+时序错开+守卫后无硬冲突；任务 agent 运行时读 guide 作阶段输入，C 改期间最坏读旧版无害；guideHash(req3 投喂哈希) 与 guide_sync.json 水位线是两套独立机制，连锁(C改guide→hash变→助手重投喂)符合预期。
  - 未新增源文件/Q_OBJECT 类；验证：cmake qt6-release Release|x64 编译链接通过（0 错）→ Execute/AutoCode.exe - project/CProjectManager.{h,cpp}, orch/CTaskOrchestrator.cpp, ui/CMainWindow.cpp
[2026-08-14] UI 八项改进（任务/阶段右键菜单、tooltip、表头可调、最大化修复、日志滚底、上下文菜单）：
  - 需求1 历史任务右键菜单：「查看任务内容」移首位，其与「再次执行」间加分隔线 - ui/CTaskListPanel.cpp
  - 需求2 当前任务右键菜单：「查看任务内容」与「移除」间加分隔线 - ui/CTaskListPanel.cpp
  - 需求3 阶段列表右键「再次执行」：编排器新增 RunSingleStage(task,stage)+m_bSingleStageRerun 标志，Advance 记录产物后据此置 Paused 返回（不进 ProceedToNext/RunCompileGate/续跑），仅重跑该阶段不推进后续；阶段表 RefreshTask 填充时 setData(UserRole,int(eStage))，CExecutionPanel 增 m_pCurTask 指针+sigRerunStage 信号+OnStagesContextMenu 槽，CMainWindow 连接转 RunSingleStage - orch/CTaskOrchestrator.{h,cpp}, ui/CExecutionPanel.{h,cpp}, ui/CMainWindow.cpp
  - 需求4 历史任务表：表头改 Interactive 全列可拖分隔线调宽+末列 stretchLastSection 填充+默认列宽；名称列 setToolTip(0,Name) 兜底显示不全 - ui/CTaskListPanel.cpp
  - 需求5 当前任务列表：QListWidgetItem setToolTip(sText) 行文本显示不全兜底 - ui/CTaskListPanel.cpp
  - 需求6 最大化按钮修复：frameless 下 showMaximized/showNormal 偶不即时反映到 isMaximized()（窗口视觉最大化但 windowState 仍 NoState）致图标/状态错乱；sigMaximizeToggle 槽改用 setWindowState 直接置位+立即 SetMaximized 同步图标，不依赖异步 changeEvent/resizeEvent 兜底时序 - ui/CMainWindow.cpp
  - 需求7 日志滚底：CMessageView 新增 ScrollToBottomRobust（adjustSize+0/30/120ms 多帧，覆盖 Markdown 块异步高度收敛），RebuildView(切任务)/RebuildAdhocView(打开工程) 改调之；流式 AddBlock 保持单帧 ScrollToBottom 避免定时器堆积 - ui/CMessageView.{h,cpp}, ui/CExecutionPanel.cpp
  - 需求8 上下文右键菜单：「立即压缩上下文」与「设置上下文大小…」位置调换（指针比较判断不受影响）- ui/CExecutionPanel.cpp
  - 未新增源文件/Q_OBJECT 类，CMake(AUTOMOC)/vcxproj 无需同步；本次按用户要求仅改代码未编译 - ui/CTaskListPanel.cpp, ui/CExecutionPanel.{h,cpp}, ui/CMessageView.{h,cpp}, ui/CMainWindow.cpp, orch/CTaskOrchestrator.{h,cpp}
[2026-08-14] 修复最大化死循环 + 恢复边缘缩放（鼠标拖动调节窗口大小）：
  - 根因：frameless 窗口最大化过程中 resizeEvent 早于 windowState 更新，原 resizeEvent 里 SetMaximized(isMaximized()) 读到旧值(false)，把按钮刚切的 □ 图标回滚成 ❐，致 m_bMaximized 恒 false → 再点又判「需最大化」=死循环（图标不变+无法还原）；同一 isMaximized() 时序坑使 nativeEvent 的 if(isMaximized()) return false 在窗口态残留 Maximized 时永久禁用边缘缩放（拖不动边缘）。
  - CTitleBar 增 IsMaximized() getter；最大化按钮与边缘缩放统一改用它（自维护 m_bMaximized）作为唯一真相源，绕开 isMaximized() 异步时序。
  - 最大化按钮 lambda：翻转目标态 + 显式 SetMaximized 同步图标，不再依赖状态事件；resizeEvent 去掉 SetMaximized 干预（切断回滚路径）仅转发基类；showEvent/changeEvent 保留兜底（Aero Snap/Win+↑ 等系统路径，时序安全）。
  - 边缘缩放带 6→8px 更易抓取。
  - 涉及文件 - ui/CTitleBar.h, ui/CMainWindow.cpp；未新增 Q_OBJECT 类；本次仅改代码未编译
[2026-08-14] 最大化图标滞后修正（接上条）：
  - 上条已改「按钮 lambda 翻转 + resizeEvent 去同步」，但实测图标仍比窗口状态滞后一拍、要点 3 次才同步。定位到漏网的 changeEvent：showMaximized/showNormal 异步派发 changeEvent 的瞬间 isMaximized() 尚未更新（读旧值），把按钮刚切好的图标回滚成旧态——这是上一版残留的回滚源。
  - changeEvent 改用 QTimer::singleShot(0, this, ...) 推迟到本轮事件队列末尾再读 isMaximized()（此时已稳定），用真实状态同步图标；singleShot 的 this 作 context，窗口销毁回调不触发，无悬垂。按钮 lambda 即时翻转不变（与延迟校正目标态一致则 SetMaximized 早退，互不冲突）。
  - 涉及文件 - ui/CMainWindow.cpp（+QTimer include、changeEvent）；未编译
[2026-08-14] 最大化图标同步改用 Win32 IsZoomed（根治）：
  - 经多轮验证，frameless 窗口下 QWidget::isMaximized() 完全不可靠（同步读/事件队列末尾延迟读都拿不到正确值），图标始终与窗口状态错位、要点多次才同步。彻底放弃 isMaximized()。
  - 改用 Win32 IsZoomed(hWnd) 读窗口真实最大化态（查 WS_MAXIMIZE，与视觉一致）：主力放 nativeEvent 拦 WM_SIZE（最大化/还原必经的 Win32 确定事件，覆盖按钮/Aero Snap/Win+↑ 一切途径），changeEvent/showEvent 兜底也统一改 IsZoomed；删除上一版加的 QTimer singleShot 方案与 #include <QTimer>。
  - 按钮 lambda 仍用 m_bMaximized 翻转目标态（即时反馈），最终由 WM_SIZE 的 IsZoomed 校正（一致则 SetMaximized 早退），2 次点击完成最大化/还原、图标同帧同步。
  - 涉及文件 - ui/CMainWindow.cpp；未编译
[2026-08-14] 最大化按钮确定性修复（按用户「先改这一步」）+ 消除 IsMaximized 宏冲突：
  - 前 3 轮（isMaximized 翻转 / singleShot 延迟 / IsZoomed+WM_SIZE 同步）均未根治图标错位，按用户指示回归最简：图标状态只由最大化按钮掌管，移除 changeEvent/WM_SIZE 的异步图标同步（它们是异步回滚源）。
  - 按钮判方向改用 Win32 IsZoomed（窗口真实 WS_MAXIMIZE，与视觉一致），不依赖会被污染的 m_bMaximized；点击立即 showMaximized/showNormal + SetMaximized 同帧改图标，绝不被异步事件改回——保证「点击即最大化、图标立即变还原(□)」确定生效。changeEvent 改为纯转发基类；showEvent 保留 IsZoomed 首帧/恢复帧初始化图标。
  - 消除 warning C4003：CTitleBar::IsMaximized 与 <windowsx.h> 的 IsMaximized(hwnd) 宏（IsZoomed 别名）撞名，被预处理器展开。删除该 getter（已无引用），nativeEvent 边缘缩放禁用改用 IsZoomed 读真实态。
  - 验证：cmake --build build --config Release 通过（0 错 0 警）→ Execute/AutoCode.exe；未新增 Q_OBJECT 类。
  - 涉及文件 - ui/CTitleBar.h, ui/CMainWindow.cpp
[2026-08-14] 最大化按钮改用「图标态」判翻转方向：
  - 按钮判方向从 Win32 IsZoomed 改为图标态 m_bMaximized（CTitleBar 新增 IsMaxState() getter，名避 IsMaximized 宏）：图标是还原□（已最大化）→还原窗口+图标改最大化❐；图标是最大化❐（未最大化）→最大化+图标改还原□。图标与点击动作同源、永远自洽；因已移除异步回滚，m_bMaximized 只由按钮掌管，用它判方向安全。
  - 验证：cmake --build build --config Release 通过（0 错 0 警）→ Execute/AutoCode.exe。
  - 涉及文件 - ui/CTitleBar.h, ui/CMainWindow.cpp
[2026-08-14] 清理最大化排查遗留的冗余 override/注释：
  - changeEvent/resizeEvent 在图标同步逻辑全部移除后只剩空转发基类，删除实现 + .h 声明（最大化图标现仅由按钮 + showEvent 首帧掌管）；精简 showEvent/nativeEvent 过时注释（去掉「上一版/changeEvent 兜底/与按钮同源」等排查叙述）。
  - 验证：cmake --build build --config Release 通过（0 错 0 警）→ Execute/AutoCode.exe。
  - 涉及文件 - ui/CMainWindow.{h,cpp}
[2026-08-14] 修复助手日志 Claude 块不自动换行（任务流正常、助手流异常的根因）：
  - 根因（两处时序缺陷叠加）：任务流与助手流共用 CMessageBlock，差异仅在「块创建时所在 Tab 是否可见」。任务日志 Tab 是当前页 → 块创建即布局、宽度就绪、换行生效；助手流的 Claude 块常在助手 Tab 不可见时创建（打开工程恢复 assistant.json 批量重建、agent 运行中用户切走），QTabWidget 隐藏页不布局 → QTextBrowser viewport 宽度恒 0 → slotAdjustBrowserHeight 跳过 setTextWidth → document 按理想宽度（不换行）排版。此前的 resizeEvent 兜底断在两点：①块 resize 瞬间子 m_pBrowser 尚未布局到新宽度（Qt 布局滞后一拍），读到旧值 0 仍跳过；②切回助手 Tab 时若块尺寸不变（隐藏期间已被赋宽）仅可见性变 → 只发 showEvent 不发 resizeEvent，修复不触发。
  - 修复：resizeEvent 改 QTimer::singleShot(0,this,slot) 延迟一拍（待子控件布局稳定后按真实视口宽度重排）；新增 showEvent override 同样延迟重排（覆盖「仅可见性变」的 Tab 切换场景）。singleShot 以 this 为接收者，块销毁自动取消回调无悬垂；slotAdjustBrowserHeight 既有 viewport>0 守卫 + m_bAdjusting 防重入不变，重复触发幂等。
  - 验证：cmake --build build --config Release 通过（0 错）→ Execute/AutoCode.exe。
  - 涉及文件 - ui/CMessageBlock.h, ui/CMessageBlock.cpp
[2026-08-14] 新增图文使用说明（Execute/help/使用说明.html + images/ 截图4张），帮助菜单改为用系统默认浏览器打开该文件
[2026-08-15] 使用说明新增「人工介入」一节（四种通道+能力速查表+已知限制说明），并修正 FAQ 中途纠偏说法 - Execute/help/使用说明.html
[2026-08-15] guide.md 生成期并入任务首阶段：生成期间任务置 Running（继续按钮置灰、执行态指示器转圈），生成完成自动续跑，失败/暂停/停止请求分流处理；生成期间拒绝启动其他任务 - ui/CMainWindow.cpp
[2026-08-15] 修 UI 卡顿：任务对话流重建改分帧渐进+批量免逐块滚底(原 O(N²) 布局致选任务卡 ~10s)；启动/继续路径重活(guide 指纹同步+任务目录初始化+改码前全量备份)移后台线程,完成回主线程续跑 - ui/CMessageView.{h,cpp} ui/CExecutionPanel.{h,cpp} orch/CTaskOrchestrator.{h,cpp}
[2026-08-15] 修事件洪水卡死：CClaudeRunner 源头丢弃 system/thinking_tokens 临时计数事件(思考期 ~2条/秒/视角)；评审并行 sigInfo 转发与主路径同口径过滤 system 噪音行(只落 run.log 不上屏) - agent/CClaudeRunner.cpp orch/CTaskOrchestrator.cpp
[2026-08-15] 阶段联网检索(prompts 01/04/06/11 条件化指令+config.ini [Agent] WebSearch=0 全阶段禁用) / 驳回差异化引导(所有生产阶段回炉注入评审产物全文+按迭代递进的换思路策略) / 熔断弹窗升级决策入口(可附人工指导意见,随下一轮 prompt 最高优先级注入,一次性消费) / prompts 便携化(构建同步到 Execute,运行期只读 exe 同级) - orch/CTaskOrchestrator.{h,cpp} core/CTaskInfo.{h,cpp} core/StageDef.cpp project/CProjectManager.{h,cpp} ui/CMainWindow.cpp CMakeLists.txt prompts/{01,04,06,11}*.md
[2026-08-15] 助手上下文进度条运行中实时刷新：CClaudeRunner 每条 assistant 消息按 usage(input+cache 三项)更新占用并新增 sigContextFill 上抛，编排器直通 sigAdhocContext(原仅 result 事件终值刷新，运行中恒显 0) - agent/CClaudeRunner.{h,cpp} orch/CTaskOrchestrator.cpp
[2026-08-15] 修正助手上下文口径：result 的 usage 实为本次全部 API 调用 input 侧累计(实测 21 次调用累计 1,365,471 与 result 一致，远超单次上下文可越窗口)，不能再当上下文占用——改为以最后一条 assistant 消息的 usage 为准，仅未收到 assistant usage 时回退 result 值(修完成瞬间进度条从 ~68K 跳 1.3M 且 assistant.json 持久化错误值) - agent/CClaudeRunner.{h,cpp}
[2026-08-15] 助手状态与记忆跟着任务走：助手档(sid/撤销栈/guideHash/ctxFill/对话块)按选中任务分档存 .autodev/tasks/<id>/assistant.json(无任务回退工程级全局档,向后兼容)；新增 SwitchAssistantArchive 切换原语(停跑→旧档落盘→切m_pCur→载新档,所有改 m_pCur 路径接入,修切工程尾块串档)；助手视图改分帧渐进重建(大历史不卡UI)；新增每任务 memory.md 经验记忆并在 BuildPrompt/SubmitAdhoc 注入+Edit追加规则(不动claude全局记忆) - project/CProjectManager.{h,cpp} ui/CExecutionPanel.{h,cpp} ui/CMainWindow.{h,cpp} orch/CTaskOrchestrator.{h,cpp}
[2026-08-15] 新增「功能→设置」对话框集中编辑 config.ini(策略MaxIter/MaxResume/ReviewResume、Agent:claude命令/AskTimeoutSec/WebSearch、评审并行、助手AutoCompactPct压缩比例/ContextLimit、[Scan]/[Backup]排除目录)；AutoCompact 千token档位改比例制(窗口×比例换算,删旧[Adhoc]AutoCompact键与面板档位链)；策略常量/claude命令/提问超时/排除目录全部配置驱动现读(保存即生效) - ui/CSettingsDialog.{h,cpp} ui/CMainWindow.{h,cpp} ui/CExecutionPanel.{h,cpp} project/CProjectManager.{h,cpp} project/CGuideFingerprintScanner.{h,cpp} orch/CTaskOrchestrator.{h,cpp} agent/CClaudeRunner.{h,cpp} agent/CMcpAskServer.cpp CMakeLists.txt
[2026-08-15] 任务运行耗时：SetStatus 进/出 Running 自动打点跨段累计(RunElapsedMs)，task.json(runMs/runStart) + history.db(run_ms 列,含旧库 ALTER 迁移)持久化；底部状态区「运行中」后实时显示耗时(1s 刷新)，当前任务列表项/历史任务表新「耗时」列/查看任务对话框一并展示 - core/CTaskInfo.{h,cpp} db/CTaskHistoryDB.cpp ui/CExecutionPanel.{h,cpp} ui/CTaskListPanel.{h,cpp} ui/CNewTaskDialog.{h,cpp}
[2026-08-15] 历史任务回看阶段表按执行时状态标注(选中DB副本从task.json懒加载阶段历史;当前阶段区分进行中/失败/未完成) + 功能菜单设置与环境安装调换位置 - CMainWindow.cpp, CExecutionPanel.cpp

[2026-08-15] 编排提效三件套：流程档位+阶段勾选清单(简单/标准/严格,task.json 持久化 tier/flowStages,缺文档阶段按实际资料推进)、评审多数决(标准档 2/3 驳回才驳回,少数派意见随产物与后续 prompt 携带,严格档保持一票否决)、轻量评审瘦身禁穷尽核查+限 turns(标准 18/简单 12) - TaskTypes.h CTaskInfo StageDef CStageMachine CTaskOrchestrator CNewTaskDialog CMainWindow CExecutionPanel CMakeLists prompts/03 05 07
