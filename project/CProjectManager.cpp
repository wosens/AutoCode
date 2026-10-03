#include "CProjectManager.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <QCryptographicHash>
#include <QThread>   // 2026.08.22 blob 临时名防并发碰撞（线程 id 掺入）
#include <thread>     // 2026.08.26 指纹同步后台线程（大工程扫描不阻塞 UI）
#include <QSet>
#include <QMap>   // 2026.09.21 TruncateBefore：seq→meta 内存快照
#include <vector>    // 2026.09.09 v2.1.143 任务级配置覆盖（MergeFillTaskConfig 节枚举缓冲）

#include <zstd.h>       // 备份压缩（#2）：第三方静态库 libzstd_static，逐文件 zstd 压缩/解压
#include <Windows.h>
#include <climits>
#include <algorithm>

#include "agent/CClaudeRunner.h"   // ContextWindowForModel 静态仍在此
#include "agent/CAgentRunnerFactory.h"
#include "core/CTaskInfo.h"
#include "core/StageDef.h"

CProjectManager::CProjectManager(QObject* a_pParent)
    : QObject(a_pParent) {
}

bool CProjectManager::Open(const QString& a_sDir) {
    QDir stDir(a_sDir);
    if (!stDir.exists()) {
        return false;
    }
    m_sProjectDir = QDir::cleanPath(stDir.absolutePath());
    // 切工程：旧 store 指针作废（含旧工程路径），惰性重建。
    {
        QMutexLocker stLock(&m_MutexStore);
        delete m_pBackupStore;
        m_pBackupStore = nullptr;
    }
    // 静默孤儿 GC（方案 §4.4：备份中断留下的无引用 blob）。
    if (QDir(UnifiedBackupsDir()).exists()) {
        BackupStore()->GcOrphanBlobs();
    }
    return true;
}

CProjectManager::~CProjectManager() {
    delete m_pBackupStore;
}

QString CProjectManager::AutoDevDir() const {
    return QDir::cleanPath(m_sProjectDir + QStringLiteral("/.autodev"));
}

QString CProjectManager::TaskDir(const QString& a_sTaskId) const {
    return QDir::cleanPath(AutoDevDir() + QStringLiteral("/tasks/") + a_sTaskId);
}

bool CProjectManager::EnsureAutoDev() {
    if (m_sProjectDir.isEmpty()) {
        return false;
    }
    QDir stRoot(m_sProjectDir);
    if (!stRoot.mkpath(QStringLiteral(".autodev/tasks"))) {
        return false;
    }
    return true;
}

bool CProjectManager::InitTaskDir(const QString& a_sTaskId) {
    return QDir().mkpath(TaskDir(a_sTaskId));
}

QList<CTaskInfo*> CProjectManager::LoadAllTasks() const {
    QList<CTaskInfo*> aTasks;
    const QDir stTasksDir(AutoDevDir() + QStringLiteral("/tasks"));
    if (!stTasksDir.exists()) {
        return aTasks;
    }
    // 按目录名排序，保证加载顺序稳定（便于 UI 列表一致）
    const QStringList aDirs =
        stTasksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& sId : aDirs) {
        const QString sFile = TaskDir(sId) + QStringLiteral("/task.json");
        if (!QFileInfo::exists(sFile)) {
            continue;  // 无 task.json 的目录（如纯产物残留）跳过
        }
        auto* pTask = new CTaskInfo();   // 无父对象，由 CMainWindow 接管生命周期
        if (pTask->LoadFromFile(sFile)) {
            aTasks.append(pTask);
        } else {
            delete pTask;  // 解析失败，丢弃避免脏数据
        }
    }
    return aTasks;
}

bool CProjectManager::HasGuide() const {
    return QFileInfo::exists(AutoDevDir() + QStringLiteral("/guide.md"));
}

QString CProjectManager::ReadGuide() const {
    CFrwDevice stFile(AutoDevDir() + QStringLiteral("/guide.md"));
    if (!stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream stIn(&stFile);
    return stIn.readAll();
}

bool CProjectManager::AppendRecord(const QString& a_sLine) {
    QDir().mkpath(AutoDevDir());
    QFile stFile(AutoDevDir() + QStringLiteral("/record.md"));
    if (!stFile.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    QTextStream stOut(&stFile);
    stOut << a_sLine << "\n";
    return true;
}

bool CProjectManager::AppendTaskLog(const QString& a_sTaskId, const QString& a_sLine) {
    // 每个任务的完整运行流水落 .autodev/tasks/<id>/run.log（UTF-8；首建写 BOM）。
    InitTaskDir(a_sTaskId);   // 确保任务私有目录存在
    const QString sPath = TaskDir(a_sTaskId) + QStringLiteral("/run.log");
    const bool bNewFile = !QFileInfo::exists(sPath);
    QFile stFile(sPath);
    // 不用 QIODevice::Text：自行控制换行与 BOM，避免平台换行差异
    if (!stFile.open(QIODevice::Append)) {
        return false;
    }
    if (bNewFile) {
        stFile.write("\xEF\xBB\xBF");   // UTF-8 BOM（仅首建一次）
    }
    stFile.write(a_sLine.toUtf8());
    stFile.write("\n");
    return true;
}

// —— 对话流持久化文件（#1 助手 / #6 任务）——
// 仅负责字节读写：JSON 的组装/解析由 CMainWindow 用 QJsonDocument 完成，本层不耦合结构。
QString CProjectManager::AssistantStateFile() const {
    return AutoDevDir() + QStringLiteral("/assistant.json");
}

bool CProjectManager::WriteAssistantState(const QByteArray& a_sData) const {
    if (m_sProjectDir.isEmpty()) {
        return false;
    }
    // const 方法内不能调非 const 的 EnsureAutoDev；直接建父目录（.autodev 通常已存在）。
    QDir().mkpath(AutoDevDir());
    QFile stFile(AssistantStateFile());
    // WriteOnly+Truncate 全量覆盖（每次落盘都是最新完整快照）；JSON 无需 BOM。
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stFile.write(a_sData);
    return true;
}

QByteArray CProjectManager::ReadAssistantState() const {
    CFrwDevice stFile(AssistantStateFile());
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QByteArray();   // 不存在/无权限：返回空，调用方判空跳过恢复
    }
    return stFile.readAll();
}

QString CProjectManager::TaskChatFile(const QString& a_sTaskId) const {
    return TaskDir(a_sTaskId) + QStringLiteral("/chat.json");
}

bool CProjectManager::WriteTaskChat(const QString& a_sTaskId, const QByteArray& a_sData) const {
    if (m_sProjectDir.isEmpty()) {
        return false;
    }
    QDir().mkpath(TaskDir(a_sTaskId));   // 确保任务私有目录存在（const 内直接建）
    QFile stFile(TaskChatFile(a_sTaskId));
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stFile.write(a_sData);
    return true;
}

QByteArray CProjectManager::ReadTaskChat(const QString& a_sTaskId) const {
    CFrwDevice stFile(TaskChatFile(a_sTaskId));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return stFile.readAll();
}

QString CProjectManager::TaskAssistantFile(const QString& a_sTaskId) const {
    return TaskDir(a_sTaskId) + QStringLiteral("/assistant.json");
}

bool CProjectManager::WriteTaskAssistantState(const QString& a_sTaskId, const QByteArray& a_sData) const {
    if (m_sProjectDir.isEmpty() || a_sTaskId.isEmpty()) {
        return false;
    }
    QDir().mkpath(TaskDir(a_sTaskId));   // 确保任务私有目录存在（const 内直接建）
    QFile stFile(TaskAssistantFile(a_sTaskId));
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stFile.write(a_sData);
    return true;
}

QByteArray CProjectManager::ReadTaskAssistantState(const QString& a_sTaskId) const {
    CFrwDevice stFile(TaskAssistantFile(a_sTaskId));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QByteArray();   // 该任务还没用过助手：返回空，调用方判空走清空分支
    }
    return stFile.readAll();
}

QString CProjectManager::TaskMemoryFile(const QString& a_sTaskId) const {
    return TaskDir(a_sTaskId) + QStringLiteral("/memory.md");
}

QString CProjectManager::ReadTaskMemory(const QString& a_sTaskId) const {
    CFrwDevice stFile(TaskMemoryFile(a_sTaskId));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QString();   // 不存在（任务还没沉淀过经验）→ 空，调用方跳过注入
    }
    return QString::fromUtf8(stFile.readAll());
}

void CProjectManager::BuildGuideAsync() {
    if (m_sProjectDir.isEmpty()) {
        emit sigGuideFinished(false, QStringLiteral("未打开工程"));
        return;
    }
    EnsureAutoDev();

    // 2026.08.20 空工程支持（从 0 创建）：工程除 .autodev 等排除目录外没有任何文件时，
    //   不再交给 Agent 扫描（无物可扫 → 臆造内容且不落盘 → 反复"生成失败"），改为本地
    //   确定性写一份最小 guide.md（新工程占位骨架）。后续 Develop 产出源码后，任务启动前
    //   的 SyncGuideIncremental 会自动补指纹表，菜单「生成 guide」也可全量重生成替换。
    if (!CGuideFingerprintScanner::HasAnyScannableFile(m_sProjectDir)) {
        const QString sGuide = QStringLiteral(
            "# 工程指南（guide.md）\n\n"
            "> 本工程为**新建空工程**：当前尚无任何源码文件，首个任务将**从零开始**创建全部代码。\n"
            "> 本文件由 AutoCode 在工程为空时自动生成的占位骨架；工程出现源码后，"
            "可通过菜单「工程 → 生成 guide」全量重生成，或由任务启动前的指纹增量同步自动维护。\n\n"
            "## 1. 项目概述\n\n- 状态：空白工程，待首个任务定义产品形态。\n\n"
            "## 2. 技术栈与依赖\n\n- 待定：以首个任务的需求描述为准。\n\n"
            "## 3. 目录结构\n\n- 当前为空（仅 `.autodev/` 工作目录）。\n\n"
            "## 4. 编码规范声明\n\n"
            "编写/修改本工程代码时，自动遵循 %USERPROFILE%\\.claude\\CLAUDE.md 的全部规则"
            "（命名/格式/文件编码/修改记录/配置位置等）。\n"
            "新建文件使用 UTF-8（无 BOM）编码；修改已有文件时遵循其既有风格。\n\n"
            "## 5. 构建 / 运行方式\n\n- 待定：由首个任务的设计/计划阶段确定。\n");
        QFile stGuide(AutoDevDir() + QStringLiteral("/guide.md"));
        if (!stGuide.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            emit sigGuideFinished(false,
                QStringLiteral("空工程 guide.md 写入失败（无法创建文件）"));
            return;
        }
        {
            QTextStream stOut(&stGuide);
            stOut.setEncoding(QStringConverter::Utf8);
            stOut << sGuide;
        }
        stGuide.close();
        AppendRecord(QStringLiteral("[%1] 空工程：本地生成 guide.md 占位骨架（无源码可扫描）")
                         .arg(QDateTime::currentDateTime().toString(
                             QStringLiteral("yyyy-MM-dd HH:mm"))));
        SaveGuideSyncLines(1);   // 上面的 AppendRecord 即全部 record 行，水位线对齐
        emit sigGuideLog(QStringLiteral(
            ">>> 工程为空，已本地生成 guide.md 占位骨架（从零开发模式）"));
        emit sigGuideFinished(true, QStringLiteral("guide.md 生成完成（空工程占位）"));
        return;
    }

    // 删除旧 guide，避免 claude 读到过期内容
    QFile::remove(AutoDevDir() + QStringLiteral("/guide.md"));

        // 2026.08.18 后端漂移重建：非运行中才换（运行中由 OnSettings 拦截 + 双保险）。
        if (m_pGuideRunner != nullptr
            && m_pGuideRunner->BackendName() != CAgentRunnerFactory::BackendName()
            && !m_pGuideRunner->IsRunning()) {
            m_pGuideRunner->deleteLater();
            m_pGuideRunner = nullptr;
        }
    if (m_pGuideRunner == nullptr) {
        m_pGuideRunner = CAgentRunnerFactory::Create(this);
        connect(m_pGuideRunner, &CAgentRunnerBase::sigInfo,
                this, &CProjectManager::sigGuideLog);
        connect(m_pGuideRunner, &CAgentRunnerBase::sigAssistantText,
                this, [this](const QString& a_sText) { emit sigGuideLog(a_sText); });
        connect(m_pGuideRunner, qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                    &CAgentRunnerBase::sigFinished),
                this, &CProjectManager::OnGuideRunnerFinished);
    }

    const QString sPrompt = QStringLiteral(
        "你是项目结构分析助手。请扫描当前工程目录（用 Glob、Read 等工具），"
        "理解整体架构与各模块，生成 `.autodev/guide.md`。\n\n"
        "**扫描顺序（重要）**：优先探索 `.md`、`.txt`、`.h`、`.cpp` 等文本文件——"
        "它们承载项目文档与代码逻辑，是理解架构的关键；"
        "二进制、资源、生成产物等非文本文件跳过。先读已有的 README/readme/guide 等文档类文件，"
        "快速建立项目全貌，再深入源码细节。\n\n"
        "内容应包含：\n"
        "1. 项目概述（用途、目标）\n"
        "2. 技术栈与依赖\n"
        "3. 目录结构（关键目录说明）\n"
        "4. 各模块清单：每个模块的位置、功能、与其他模块的调用关系\n"
        "5. 构建/运行方式\n"
        "6. 编码规范声明（固定条目，必须有）：在文档靠前位置写明——"
        "「编写/修改本工程代码时，自动遵循 %USERPROFILE%\\.claude\\CLAUDE.md 的全部规则"
        "（命名/格式/文件编码/修改记录/配置位置等）」。\n\n"
        "约束：\n"
        "- 只创建 `.autodev/guide.md` 这一个文件，不要修改任何业务代码。\n"
        "- 内容要准确、基于真实扫描结果，不要臆造。\n"
        "- 若工程很大，聚焦主要源码目录，忽略 build/.git/node_modules 等。\n"
        "- 优先读取文本文件（.md/.txt/.h/.cpp 等），非文本文件不要逐个打开。\n");

    emit sigGuideLog(QStringLiteral(">>> 开始扫描工程并生成 guide.md ..."));
    m_pGuideRunner->StartStage(m_sProjectDir, sPrompt,
                               ToolsDocs(), 40);
}

void CProjectManager::OnGuideRunnerFinished(bool a_bOk, const QString& a_sResult,
                                            qint64 /*a_iIn*/, qint64 /*a_iOut*/,
                                            double /*a_dCost*/, const QString& /*a_sSid*/) {
    if (a_bOk && HasGuide()) {
        const QDateTime stNow = QDateTime::currentDateTime();
        AppendRecord(QStringLiteral("[%1] 自动生成 .autodev/guide.md")
                         .arg(stNow.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        // 协调2：全量重生成 guide.md 后，历史 record 不必再同步进 guide——水位线推进到当前
        //   record 行数（含本条生成日志），避免首次触发把全部历史 record 喂进同步 agent。
        {
            CFrwDevice stRec(AutoDevDir() + QStringLiteral("/record.md"));
            int iLines = 0;
            if (stRec.open(QIODevice::ReadOnly | QIODevice::Text)) {
                iLines = QTextStream(&stRec).readAll()
                             .split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
            }
            SaveGuideSyncLines(iLines);
        }
        emit sigGuideFinished(true, QStringLiteral("guide.md 生成完成"));
    } else {
        emit sigGuideFinished(false,
            QStringLiteral("guide.md 生成失败：%1").arg(a_sResult.left(200)));
    }
}

void CProjectManager::SyncGuideAsync(const QString& a_sTaskId) {
    // 需求4：按 record.md 新增流水用独立 agent 增量更新 guide.md（语义同步，非全量重扫）。
    //   水位线机制（guide_sync.json 的 lastSyncLines）：只把水位线之后的 record 新增行喂给 agent，
    //   避免重复同步历史；同步完成（或 BuildGuide 全量重生成）后推进水位线。
    //   独立 runner + 独立完成槽，与 BuildGuideAsync 生成路径隔离（OnGuideFinished 会自动续跑待启动任务，
    //   复用同一路径会误触发）。a_sTaskId 在 record 流水方案下不再依赖（保留兼容调用点）。
    Q_UNUSED(a_sTaskId)
    if (m_sProjectDir.isEmpty()) {
        emit sigGuideSyncFinished(false, QStringLiteral("未打开工程"));
        return;
    }
    if (!HasGuide()) {
        emit sigGuideSyncFinished(false, QStringLiteral("guide.md 不存在，请先生成"));
        return;
    }
    if (m_pGuideSyncRunner != nullptr && m_pGuideSyncRunner->IsRunning()) {
        emit sigGuideSyncFinished(false, QStringLiteral("上一次 guide 同步仍在进行"));
        return;
    }

    // 读 record.md 全文，取水位线之后的新增行作为本次增量输入（行数水位线，无时间戳歧义）。
    CFrwDevice stRec(AutoDevDir() + QStringLiteral("/record.md"));
    QString sRecord;
    if (stRec.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sRecord = QTextStream(&stRec).readAll();
    }
    const QStringList aAllLines = sRecord.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const int iLast = LoadGuideSyncLines();
    QStringList aNewLines;
    for (int i = iLast; i < aAllLines.size(); ++i) {
        aNewLines.append(aAllLines.at(i));
    }
    if (aNewLines.isEmpty()) {
        // 无新改动：不起 agent。任务 Done / 助手提交后高频触发，无新行时静默跳过。
        emit sigGuideSyncFinished(false, QStringLiteral("guide.md 已是最新（record.md 无新增改动）"));
        return;
    }

        // 2026.08.18 后端漂移重建：非运行中才换（运行中由 OnSettings 拦截 + 双保险）。
        if (m_pGuideSyncRunner != nullptr
            && m_pGuideSyncRunner->BackendName() != CAgentRunnerFactory::BackendName()
            && !m_pGuideSyncRunner->IsRunning()) {
            m_pGuideSyncRunner->deleteLater();
            m_pGuideSyncRunner = nullptr;
        }
    if (m_pGuideSyncRunner == nullptr) {
        m_pGuideSyncRunner = CAgentRunnerFactory::Create(this);
        connect(m_pGuideSyncRunner, &CAgentRunnerBase::sigInfo, this, &CProjectManager::sigGuideLog);
        connect(m_pGuideSyncRunner, &CAgentRunnerBase::sigAssistantText,
                this, [this](const QString& a_sText) { emit sigGuideLog(a_sText); });
        connect(m_pGuideSyncRunner, qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                    &CAgentRunnerBase::sigFinished),
                this, &CProjectManager::OnGuideSyncRunnerFinished);
    }

    // deny "Bash,Write"：禁跑命令、禁建新文件；放行 Read/Grep/Glob/Edit/MultiEdit（Edit 改 guide.md）。
    const QString sPrompt = QStringLiteral(
        "你是项目指南维护助手。工程自上次同步 `.autodev/guide.md` 后，产生了以下新的改动记录"
        "（来自 `.autodev/record.md`，每行一条，含时间/来源/摘要）：\n\n"
        "```\n%1\n```\n\n"
        "请据此增量更新 `.autodev/guide.md`，使其反映这些改动后的项目现状：\n"
        "1. 用 Read 读取当前 `.autodev/guide.md`。\n"
        "2. 逐条分析上述记录，判断是否影响 guide.md 内容（目录结构 / 各模块职责 / 调用关系等）：\n"
        "   - 新增文件或模块 → 补进相应段落（位置/功能/调用关系）。\n"
        "   - 已有文件职责变化 → 更新对应模块描述。\n"
        "   - 删除文件 → 从结构/清单中移除对应条目。\n"
        "   - 必要时用 Read/Grep 抽看相关源文件以准确描述，但不要把代码细节写进 guide.md。\n"
        "   - 若某条记录纯为日志/验证类、不影响 guide.md，则跳过该条。\n"
        "3. 若上述所有记录均不影响 guide.md，则不要做任何修改。\n"
        "4. 固定条目自检（与记录无关，每次执行）：若 guide.md 中尚无"
        "「自动遵循 %USERPROFILE%\\.claude\\CLAUDE.md 规则」的编码规范声明，"
        "在文档靠前位置补上这一条；已有则不动。\n\n"
        "约束（重要）：\n"
        "- 只允许编辑 `.autodev/guide.md` 这一个文件，用 Edit/MultiEdit 就地修改；"
        "不得新建或改动其它任何文件，尤其不要写 `.autodev/record.md`。\n"
        "- 保留 guide.md 的整体结构与既有正确内容，只更新受影响部分，不要推倒重写。\n"
        "- 内容基于真实文件，不得臆造。\n"
        "- guide.md 末尾的「文件指纹表」段（介于 `<!-- AUTOCODE_FINGERPRINTS_BEGIN -->` 与"
        " `<!-- AUTOCODE_FINGERPRINTS_END -->` 之间）由系统维护，禁止改动。\n"
    ).arg(aNewLines.join(QLatin1Char('\n')));

    emit sigGuideLog(QStringLiteral(">>> 按 record.md 新增记录同步 guide.md ..."));
    // autocompact 兜底：record 累积多 / 需 Read 核实多个文件时上下文可能逼近窗口；
    // 2026.08.15 比例制：按「窗口 × [Adhoc] AutoCompactPct」换算千 token 阈值。
    m_pGuideSyncRunner->StartStage(m_sProjectDir, sPrompt,
                                   QStringLiteral("Bash,Write"), 30,
                                   QString(), false,
                                   AutoCompactKFor(m_pGuideSyncRunner->LastModel()), false);
}

void CProjectManager::OnGuideSyncRunnerFinished(bool a_bOk, const QString& a_sResult,
                                                qint64 /*a_iIn*/, qint64 /*a_iOut*/,
                                                double /*a_dCost*/, const QString& /*a_sSid*/) {
    if (a_bOk) {
        AppendRecord(QStringLiteral("[%1] 按 record 新增记录同步 .autodev/guide.md")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        // 推进水位线到含本条同步日志的当前 record 行数，下次仅对新改动触发。
        CFrwDevice stRec(AutoDevDir() + QStringLiteral("/record.md"));
        int iLines = 0;
        if (stRec.open(QIODevice::ReadOnly | QIODevice::Text)) {
            iLines = QTextStream(&stRec).readAll()
                         .split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
        }
        SaveGuideSyncLines(iLines);
        emit sigGuideSyncFinished(true, QStringLiteral("guide.md 同步完成"));
    } else {
        // 失败不推进水位线：下次重试仍会处理这批未同步记录。
        emit sigGuideSyncFinished(false,
            QStringLiteral("guide.md 同步失败：%1").arg(a_sResult.left(200)));
    }
}

QString CProjectManager::FindLatestChangedFiles() const {
    // 扫 .autodev/tasks/*/changed_files.md，返回 mtime 最新的那份绝对路径；无则空。
    const QDir stTasksDir(AutoDevDir() + QStringLiteral("/tasks"));
    if (!stTasksDir.exists()) {
        return QString();
    }
    QString sNewest;
    QDateTime stNewestMt;
    const QStringList aSubDirs = stTasksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aSubDirs) {
        const QString sPath = stTasksDir.absoluteFilePath(sSub + QStringLiteral("/changed_files.md"));
        if (!QFile::exists(sPath)) {
            continue;
        }
        const QDateTime stMt = QFileInfo(sPath).lastModified();
        if (!stMt.isValid()) {
            continue;
        }
        if (sNewest.isEmpty() || stMt > stNewestMt) {
            stNewestMt = stMt;
            sNewest = sPath;
        }
    }
    return sNewest;
}

// ---- guide.md 增量同步水位线（.autodev/guide_sync.json）----

QString CProjectManager::GuideSyncFile() const {
    return AutoDevDir() + QStringLiteral("/guide_sync.json");
}

int CProjectManager::LoadGuideSyncLines() const {
    // 读取已同步到的 record.md 行数；无文件/缺键/非法值返回 0（视为从未同步）。
    CFrwDevice stFile(GuideSyncFile());
    if (!stFile.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QJsonValue v = QJsonDocument::fromJson(stFile.readAll()).object()
                             .value(QStringLiteral("lastSyncLines"));
    return v.isDouble() ? v.toInt() : 0;
}

void CProjectManager::SaveGuideSyncLines(int a_iLines) {
    // 落盘失败不抛（水位线最多落后一次，下次多同步一轮，无损正确性）。
    QDir().mkpath(AutoDevDir());
    QFile stFile(GuideSyncFile());
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    QJsonObject o;
    o[QStringLiteral("lastSyncLines")] = a_iLines;
    stFile.write(QJsonDocument(o).toJson());
}

// ---- guide 文件指纹增量同步（FR1）----

void CProjectManager::SyncGuideIncremental() {
    // 编排扫描器四步（Scan → ReadGuide → Parse → Diff → WriteIncremental）。
    // 2026.08.26 卡死修复：大工程（缺省排除只剩隐藏目录后，build/Execute 等全入表，
    //   十万级文件）ScanProject 枚举可达数秒～数十秒，在 UI 线程同步跑会冻结界面。
    //   改为 std::thread 后台执行：任务启动立即返回，同步尽力而为、失败不阻断。
    //   防重入：m_bFpSyncRunning 原子占用标志，上轮未完则跳过本轮（下轮任务启动补上，无损）。
    if (m_sProjectDir.isEmpty()) {
        return;
    }
    // 协调1：guide-sync agent 正在 Edit guide.md 正文时，跳过本轮指纹回写——
    //   WriteIncremental 是「读全文→改指纹段→rename 整文件覆盖」，与 agent 的 Edit 并发会丢更新。
    //   本函数尽力而为、失败吞掉，跳过一轮下次任务启动补上，无损。
    if (m_pGuideSyncRunner != nullptr && m_pGuideSyncRunner->IsRunning()) {
        return;
    }
    // 上轮后台同步未完 → 跳过（不排队，避免堆积）。
    bool bExpected = false;
    if (!m_bFpSyncRunning.compare_exchange_strong(bExpected, true)) {
        return;
    }
    // 按值捕获所需路径，避免后台线程访问 this 成员的并发风险（析构竞态由程序生命周期保证）。
    const QString sProjectDir = m_sProjectDir;
    const QString sGuidePath  = AutoDevDir() + QStringLiteral("/guide.md");
    std::thread([this, sProjectDir, sGuidePath]() {
        // RAII 占用释放：任何 return/异常路径都复位标志（否则上轮提前返回后同步永久失效）。
        struct SFlagGuard {
            std::atomic<bool>& m_rFlag;
            ~SFlagGuard() { m_rFlag = false; }
        } stGuard{m_bFpSyncRunning};
        try {
            const QList<SFileFingerprint> aCur =
                CGuideFingerprintScanner::ScanProject(sProjectDir);
            // 读 guide.md 全文（后台线程直接读文件，不走 ReadGuide 的成员访问）。
            QString sGuide;
            {
                CFrwDevice stGuide(sGuidePath);
                if (stGuide.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    sGuide = QTextStream(&stGuide).readAll();
                }
            }
            if (sGuide.isEmpty()) {
                return;   // 无 guide → 交全量流程
            }
            const QList<SFileFingerprint> aPrev =
                CGuideFingerprintScanner::ParseFingerprintTable(sGuide);
            // guide 非空但无指纹段（首次全量生成的 guide 不含指纹表）→ 交全量流程，不强加段。
            if (aPrev.isEmpty() && !sGuide.contains(CGuideFingerprintScanner::s_sBeginMarker)) {
                return;
            }
            const SGuideChangeSet stChanges = CGuideFingerprintScanner::Diff(aPrev, aCur);
            // FR1.4：三类均空 → 不动文件。
            if (stChanges.aAdded.isEmpty() && stChanges.aModified.isEmpty() &&
                stChanges.aDeleted.isEmpty()) {
                return;
            }
            const QString sTs = QDateTime::currentDateTime().toString(Qt::ISODate);
            // 回写失败忽略（尽力而为），不影响后续 InitTaskDir / RunCurrentStage。
            CGuideFingerprintScanner::WriteIncremental(sGuidePath, aCur, stChanges, sTs);
        } catch (...) {
            // 任何异常吞掉：增量同步属增强项，绝不让它中断任务启动。
        }
    }).detach();   // detach：不同步等待，任务启动路径立即返回（卡死修复核心）
}

QList<SFileFingerprint> CProjectManager::ReadFingerprintTable() const {
    return CGuideFingerprintScanner::ParseFingerprintTable(ReadGuide());
}

// ---- 最近打开工程历史（MRU）----

QString CProjectManager::NormalizePath(const QString& a_sDir) {
    // cleanPath 已统一分隔符为平台原生并解析 ".."/"."；这里再强制正斜杠并去尾部斜杠，
    // 保证跨写法的去重比较一致（AC-5）。盘符大小写也归一（仅比较用）。
    QString sNorm = QDir::cleanPath(a_sDir);
    sNorm.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (sNorm.size() > 1 && sNorm.endsWith(QLatin1Char('/'))) {
        sNorm.chop(1);
    }
    return sNorm.toLower();
}

void CProjectManager::AddRecent(const QString& a_sDir) {
    if (a_sDir.isEmpty()) {
        return;
    }
    // 统一以 cleanPath 形式存储（保留原始大小写），用 NormalizePath 做去重比较。
    const QString sStored = QDir::cleanPath(a_sDir);
    const QString sCmp = NormalizePath(sStored);

    // 去重：已存在则先移除旧条目（按规范化的统一形式比较）。
    // 2026.09.09 v2.1.139 改移除【全部】匹配：旧版 removeAt 后 break 只删首个——
    //   历史上 LoadRecent 叠加 bug（见 LoadRecent 注释）在列表里留下过同名重复项，
    //   只删首个会残留副本继续被落盘。
    // 2026.09.20 打开次数：命中时取旧计数（重复条目取最大——叠加 bug 的副本是同一批
    //   打开事件的重复落盘，取最大最接近真实），本次再 +1；新条目 = 1。
    int iOldOpens = 0;
    for (int i = 0; i < m_aRecentProjects.size(); ) {
        if (NormalizePath(m_aRecentProjects.at(i)) == sCmp) {
            if (i < m_aiRecentOpens.size()) {
                iOldOpens = qMax(iOldOpens, m_aiRecentOpens.at(i));
            } else {
                iOldOpens = qMax(iOldOpens, 1);
            }
            m_aRecentProjects.removeAt(i);
            if (i < m_aiRecentOpens.size()) {
                m_aiRecentOpens.removeAt(i);
            }
        } else {
            ++i;
        }
    }
    m_aRecentProjects.prepend(sStored);
    m_aiRecentOpens.prepend(iOldOpens > 0 ? iOldOpens + 1 : 1);
    // 计数表与路径表长度对齐（防御：异常态下截齐）。
    while (m_aiRecentOpens.size() > m_aRecentProjects.size()) {
        m_aiRecentOpens.removeLast();
    }
    while (m_aiRecentOpens.size() < m_aRecentProjects.size()) {
        m_aiRecentOpens.append(1);
    }

    SaveRecent();
}

QStringList CProjectManager::RecentProjects() const {
    return m_aRecentProjects;
}

void CProjectManager::RemoveRecent(const QString& a_sDir) {
    // 2026.08.20 与 AddRecent 同口径：按规范化路径比较，命中即删；不存在 no-op。
    //   2026.09.09 v2.1.139 同 AddRecent 改删全部匹配（防叠加 bug 残留重复项）。
    //   2026.09.20 计数随条目同删（计数表与路径表同序对齐）。
    if (a_sDir.isEmpty()) {
        return;
    }
    const QString sCmp = NormalizePath(QDir::cleanPath(a_sDir));
    for (int i = 0; i < m_aRecentProjects.size(); ) {
        if (NormalizePath(m_aRecentProjects.at(i)) == sCmp) {
            m_aRecentProjects.removeAt(i);
            if (i < m_aiRecentOpens.size()) {
                m_aiRecentOpens.removeAt(i);
            }
        } else {
            ++i;
        }
    }
    SaveRecent();
}

void CProjectManager::ClearRecent() {
    m_aRecentProjects.clear();
    m_aiRecentOpens.clear();   // 2026.09.20 计数随历史同清
}

// —— 多实例工程锁（2026.09.07 v2.1.123；说明见 CProjectManager.h 同名节）——
QString CProjectManager::ProjectLockPath(const QString& a_sProjectDir) {
    if (a_sProjectDir.trimmed().isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(a_sProjectDir) + QStringLiteral("/.autodev/instance.lock");
}

bool CProjectManager::ReadProjectLock(const QString& a_sProjectDir, SProjectLock* a_pOut) {
    const QString sLock = ProjectLockPath(a_sProjectDir);
    if (sLock.isEmpty()) {
        return false;
    }
    CFrwDevice fLock(sLock);
    if (!fLock.open(QIODevice::ReadOnly)) {
        return false;   // 无锁/不可读（无 .autodev 时同此）
    }
    const QJsonObject o = QJsonDocument::fromJson(fLock.readAll()).object();
    if (a_pOut != nullptr) {
        a_pOut->iPid     = o.value(QStringLiteral("pid")).toVariant().toLongLong();
        a_pOut->sExe     = o.value(QStringLiteral("exe")).toString();
        a_pOut->sStarted = o.value(QStringLiteral("started")).toString();
    }
    return o.value(QStringLiteral("pid")).toVariant().toLongLong() > 0;
}

bool CProjectManager::IsLockHolderAlive(const SProjectLock& a_stLock) {
    // 自 pid / 无 pid：对「他实例在席」判定而言不算占用。
    if (a_stLock.iPid <= 0
            || a_stLock.iPid == static_cast<qint64>(::GetCurrentProcessId())) {
        return false;
    }
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(a_stLock.iPid));
    if (hProc == nullptr) {
        // 打开失败：拒绝访问 = 进程存在但无权限（他人/提权）→ 保守当活；
        // 参数错等 = PID 已不存在 → 陈旧锁。
        return (::GetLastError() == ERROR_ACCESS_DENIED);
    }
    DWORD dwExit = 0;
    const BOOL bExitOk = ::GetExitCodeProcess(hProc, &dwExit);
    WCHAR wszImage[MAX_PATH] = {0};
    DWORD dwImageLen = MAX_PATH;
    QString sImage;
    if (::QueryFullProcessImageNameW(hProc, 0, wszImage, &dwImageLen) != FALSE) {
        sImage = QString::fromWCharArray(wszImage, static_cast<int>(dwImageLen));
    }
    ::CloseHandle(hProc);
    if (!bExitOk || dwExit != STILL_ACTIVE) {
        return false;   // 已退出（崩溃/强杀残留锁）
    }
    // PID 复用复核：锁主必须是 AutoCode 系进程（AutoCode.exe / AutoCode1.exe …），
    //   否则视为 PID 被无关进程回收的陈旧锁。
    if (!sImage.isEmpty() && !QFileInfo(sImage).fileName().startsWith(
            QStringLiteral("AutoCode"), Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}

void CProjectManager::WriteProjectLock(const QString& a_sProjectDir) {
    const QString sLock = ProjectLockPath(a_sProjectDir);
    if (sLock.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(sLock).absolutePath());   // .autodev 兜底（常规路径已建）
    QJsonObject o;
    o[QStringLiteral("pid")]     = static_cast<double>(::GetCurrentProcessId());
    o[QStringLiteral("exe")]     = QCoreApplication::applicationFilePath();
    o[QStringLiteral("started")] = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QFile fLock(sLock);
    if (fLock.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fLock.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        fLock.close();
    }
}

void CProjectManager::ReleaseProjectLock(const QString& a_sProjectDir) {
    SProjectLock st;
    if (!ReadProjectLock(a_sProjectDir, &st)) {
        return;
    }
    if (st.iPid != static_cast<qint64>(::GetCurrentProcessId())) {
        return;   // 非本进程的锁（他实例在席/陈旧）不动，由后继打开者接管
    }
    QFile::remove(ProjectLockPath(a_sProjectDir));
}

bool CProjectManager::IsProjectHeldByOther(const QString& a_sProjectDir, QString* a_pDesc) {
    SProjectLock st;
    if (!ReadProjectLock(a_sProjectDir, &st) || !IsLockHolderAlive(st)) {
        return false;
    }
    if (a_pDesc != nullptr) {
        *a_pDesc = QStringLiteral("PID %1 · %2 · 启动于 %3")
                       .arg(st.iPid)
                       .arg(st.sExe.isEmpty() ? QStringLiteral("AutoCode")
                                              : QFileInfo(st.sExe).fileName())
                       .arg(st.sStarted.isEmpty() ? QStringLiteral("?") : st.sStarted);
    }
    return true;
}

QString CProjectManager::LastBrowseDir() const {
    // 2026.08.20 [UI] LastProjectDir：目录选择对话框的初始定位记忆。
    //   无 config.ini / 缺键 / 目录已不存在 → 空串（调用方回退默认）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return QString();
    }
    wchar_t szBuf[MAX_PATH] = {0};
    const DWORD dwLen = ::GetPrivateProfileStringW(L"UI", L"LastProjectDir", L"",
                                                   szBuf, MAX_PATH, sPath.toStdWString().c_str());
    const QString sDir = QString::fromWCharArray(szBuf, static_cast<int>(dwLen)).trimmed();
    if (sDir.isEmpty() || !QDir(sDir).exists()) {
        return QString();
    }
    return sDir;
}

void CProjectManager::SetLastBrowseDir(const QString& a_sDir) {
    // 记住本次选择的目录（落盘失败不抛，本会话不记忆亦可接受）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty() || a_sDir.trimmed().isEmpty()) {
        return;
    }
    WritePrivateProfileStringW(L"UI", L"LastProjectDir",
                               a_sDir.trimmed().toStdWString().c_str(),
                               sPath.toStdWString().c_str());
}

QString CProjectManager::ConfigFilePath() const {
    // 取 exe 全路径 → 去文件名 → 拼 config.ini（NFR-2.1/2.2，禁用 GetCurrentDirectory）。
    wchar_t szBuf[MAX_PATH] = {0};
    const DWORD dwLen = ::GetModuleFileNameW(nullptr, szBuf, MAX_PATH);
    // 返回 0 表示失败；返回 == MAX_PATH 表示可能被截断（buffer 不够），按失败处理（R-1）。
    if (dwLen == 0 || dwLen >= MAX_PATH) {
        return QString();
    }
    QString sExe = QString::fromWCharArray(szBuf, static_cast<int>(dwLen));
    QDir stDir = QFileInfo(sExe).absoluteDir();
    return QDir::cleanPath(stDir.absolutePath() + QStringLiteral("/config.ini"));
}

// —— 任务级配置覆盖（2026.09.09 v2.1.143，说明见 CProjectManager.h 同名节）——
QString CProjectManager::SettingsCfgPath() const {
    return m_sTaskCfgFile.isEmpty() ? ConfigFilePath() : m_sTaskCfgFile;
}

void CProjectManager::MergeFillTaskConfig() {
    // 只补缺、从不覆盖任务已有键：按节枚举全局 config.ini 的键，任务档缺失则回填。
    //   节清单=设置页管理的运行参数节（[Agent]/[Review]/[Adhoc]/[Scan]/[Backup] 与四张
    //   模型表）；[Recent]/[UI]/[Window]/[Layout] 等界面态不参与（恒全局）。
    if (m_sTaskCfgFile.isEmpty() || m_sProjectDir.isEmpty()) {
        return;
    }
    const QString sGlobal = ConfigFilePath();
    if (sGlobal.isEmpty()) {
        return;
    }
    static const wchar_t* const k_aSections[] = {
        L"Agent", L"Policy", L"Review", L"Adhoc", L"Scan", L"Backup",
    };   // 四张模型表不参与键级回填（Count 列表节会被冻结）——读侧走 TableCfgPath
        // 整表活回退、写侧走 SyncModelTableToGlobal 超集同步（v2.1.144）。
    const std::wstring wsGlobal = sGlobal.toStdWString();
    const std::wstring wsTask = m_sTaskCfgFile.toStdWString();
    std::vector<wchar_t> aBuf(32768);
    for (const wchar_t* pSect : k_aSections) {
        const DWORD dwN = GetPrivateProfileSectionW(pSect, aBuf.data(),
                                                    static_cast<DWORD>(aBuf.size()),
                                                    wsGlobal.c_str());
        if (dwN == 0) {
            continue;
        }
        // 缓冲格式 "k=v\0k=v\0...\0\0"；逐键查任务档，缺才回填。
        const QString sSect = QString::fromWCharArray(pSect);
        size_t iPos = 0;
        while (iPos < dwN) {
            const QString sPair = QString::fromWCharArray(aBuf.data() + iPos);
            iPos += sPair.size() + 1;
            const int iEq = sPair.indexOf(QLatin1Char('='));
            if (iEq <= 0) {
                continue;
            }
            const QString sKey = sPair.left(iEq);
            const QString sVal = sPair.mid(iEq + 1);
            wchar_t szProbe[8] = {0};
            GetPrivateProfileStringW(pSect, sKey.toStdWString().c_str(), L"\x01",
                                     szProbe, 8, wsTask.c_str());
            if (szProbe[0] == L'\x01') {   // 任务档缺键 → 回填全局值
                WritePrivateProfileStringW(pSect, sKey.toStdWString().c_str(),
                                           sVal.toStdWString().c_str(), wsTask.c_str());
            }
        }
    }
}

void CProjectManager::SetTaskConfigOverlay(const QString& a_sTaskId) {
    m_sTaskCfgFile.clear();
    if (m_sProjectDir.isEmpty() || a_sTaskId.trimmed().isEmpty()) {
        return;
    }
    const QString sId = a_sTaskId.trimmed();
    QDir().mkpath(TaskDir(sId));
    m_sTaskCfgFile = QDir::cleanPath(TaskDir(sId) + QStringLiteral("/config.ini"));
    MergeFillTaskConfig();
}

QString CProjectManager::TableCfgPath(const wchar_t* a_pSect) const {
    // 2026.09.09 v2.1.144 模型表列表级读路径：覆盖激活且任务档已有本表（Count 键
    //   存在）→ 任务表；否则实时回退全局表（任务未显式保存过模型配置 = 跟随缺省
    //   超集；键级合并回填不适用于 Count/M<i> 列表节——全局后续新增条目无法穿过
    //   已冻结的 Count，故改为整表活回退）。
    if (!m_sTaskCfgFile.isEmpty()) {
        const int iCnt = GetPrivateProfileIntW(a_pSect, L"Count", -1,
                                               m_sTaskCfgFile.toStdWString().c_str());
        if (iCnt >= 0) {
            return m_sTaskCfgFile;
        }
    }
    return ConfigFilePath();
}

QList<SDshModelEntry> CProjectManager::LoadModelsFrom(const QString& a_sPath,
                                                      const wchar_t* a_pSect,
                                                      int* a_pActive) const {
    // 2026.09.09 v2.1.144 从 LoadPiModels 泛化（四张模型表同构，仅节名不同）。
    QList<SDshModelEntry> aModels;
    if (a_pActive) {
        *a_pActive = -1;
    }
    if (a_sPath.isEmpty()) {
        return aModels;
    }
    const std::wstring wsPath = a_sPath.toStdWString();
    const int iCount = static_cast<int>(GetPrivateProfileIntW(a_pSect, L"Count", 0, wsPath.c_str()));
    if (iCount <= 0) {
        return aModels;
    }
    wchar_t szBuf[1024] = { 0 };
    for (int i = 0; i < iCount && i < 64; ++i) {
        const std::wstring wsPrefix = L"M" + std::to_wstring(i);
        auto fnReadStr = [&](const wchar_t* a_lpszSuffix) -> QString {
            const std::wstring wsKey = wsPrefix + a_lpszSuffix;
            const DWORD dwCopied = GetPrivateProfileStringW(a_pSect, wsKey.c_str(), L"",
                                                            szBuf, 1024, wsPath.c_str());
            return QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
        };
        SDshModelEntry stE;
        stE.sDisp    = fnReadStr(L"Disp");
        stE.sBaseURL = fnReadStr(L"URL");
        stE.sApiKey  = fnReadStr(L"Key");
        stE.sApi     = fnReadStr(L"Api");
        stE.sModelId = fnReadStr(L"Model");
        stE.iCtx     = fnReadStr(L"Ctx").toLongLong();
        if (stE.iCtx <= 0) {
            stE.iCtx = 200000;
        }
        stE.iMaxTok = fnReadStr(L"Max").toInt();
        if (stE.iMaxTok <= 0) {
            stE.iMaxTok = 32768;
        }
        if (stE.sApi.isEmpty()) {
            stE.sApi = QStringLiteral("anthropic-messages");
        }
        if (stE.sDisp.isEmpty()) {
            stE.sDisp = stE.sModelId;
        }
        if (stE.sModelId.isEmpty() || stE.sBaseURL.isEmpty()) {
            continue;   // 无模型 id / 请求地址的残缺条目不进表
        }
        aModels.append(stE);
    }
    if (a_pActive) {
        const int iActive = static_cast<int>(GetPrivateProfileIntW(a_pSect, L"Active", -1, wsPath.c_str()));
        *a_pActive = (iActive >= 0 && iActive < aModels.size()) ? iActive : -1;
        if (*a_pActive < 0 && aModels.size() == 1) {   // 仅一条 → 自动设为缺省
            *a_pActive = 0;
        }
    }
    return aModels;
}

bool CProjectManager::SaveModelsTo(const QString& a_sPath, const wchar_t* a_pSect,
                                   const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026.09.09 v2.1.144 从 SavePiModels 泛化（含缩短后的高位键清理）。
    if (a_sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = a_sPath.toStdWString();
    const int iOldCount = static_cast<int>(GetPrivateProfileIntW(a_pSect, L"Count", 0, wsPath.c_str()));
    const int iNewCount = a_aModels.size();
    WritePrivateProfileStringW(a_pSect, L"Count",
                               std::to_wstring(iNewCount).c_str(), wsPath.c_str());
    for (int i = 0; i < iNewCount; ++i) {
        const SDshModelEntry& stE = a_aModels.at(i);
        const std::wstring wsPrefix = L"M" + std::to_wstring(i);
        auto fnWrite = [&](const wchar_t* a_lpszSuffix, const QString& a_sValue) {
            const std::wstring wsKey = wsPrefix + a_lpszSuffix;
            WritePrivateProfileStringW(a_pSect, wsKey.c_str(),
                                       a_sValue.trimmed().toStdWString().c_str(), wsPath.c_str());
        };
        fnWrite(L"Disp",  stE.sDisp);
        fnWrite(L"URL",   stE.sBaseURL);
        fnWrite(L"Key",   stE.sApiKey);
        fnWrite(L"Api",   stE.sApi.isEmpty() ? QStringLiteral("anthropic-messages") : stE.sApi);
        fnWrite(L"Model", stE.sModelId);
        fnWrite(L"Ctx",   QString::number(stE.iCtx));
        fnWrite(L"Max",   QString::number(stE.iMaxTok));
    }
    for (int i = iNewCount; i < iOldCount && i < 64; ++i) {
        const std::wstring wsPrefix = L"M" + std::to_wstring(i);
        for (const wchar_t* lpszSuffix : {L"Disp", L"URL", L"Key", L"Api", L"Model", L"Ctx", L"Max"}) {
            const std::wstring wsKey = wsPrefix + lpszSuffix;
            WritePrivateProfileStringW(a_pSect, wsKey.c_str(), nullptr, wsPath.c_str());
        }
    }
    const int iActive = (a_iActive >= 0 && a_iActive < iNewCount) ? a_iActive : -1;
    WritePrivateProfileStringW(a_pSect, L"Active",
                               std::to_wstring(iActive).c_str(), wsPath.c_str());
    return true;
}

void CProjectManager::SyncModelTableToGlobal(const wchar_t* a_pSect,
                                             const QList<SDshModelEntry>& a_aModels,
                                             int a_iActive) {
    // 2026.09.09 v2.1.144 全局超集同步（用户需求：缺省配置始终保有最多的模型配置）：
    //   任务态保存的模型表同时并入全局——同身份（URL+Api+ModelId 忽略大小写）以任务
    //   表内容更新、新身份追加、从不删除全局条目；全局 Active 保持原值（仅全局表
    //   原本为空时采用任务表 Active，确保缺省配置自身可用）。未激活覆盖=本来就写
    //   全局，直接返回；空表不反向写（防任务清表误清空全局）。
    if (m_sTaskCfgFile.isEmpty() || a_aModels.isEmpty()) {
        return;
    }
    const QString sGlobal = ConfigFilePath();
    if (sGlobal.isEmpty()) {
        return;
    }
    auto fnIdent = [](const SDshModelEntry& a_stE) -> QString {
        return a_stE.sBaseURL.trimmed().toLower() + QLatin1Char('|')
             + a_stE.sApi.trimmed().toLower() + QLatin1Char('|')
             + a_stE.sModelId.trimmed().toLower();
    };
    int iGlobalActive = -1;
    const QList<SDshModelEntry> aGlobal = LoadModelsFrom(sGlobal, a_pSect, &iGlobalActive);
    QList<SDshModelEntry> aMerged = aGlobal;
    for (const SDshModelEntry& stE : a_aModels) {
        const QString sId = fnIdent(stE);
        bool bReplaced = false;
        for (int j = 0; j < aMerged.size(); ++j) {
            if (fnIdent(aMerged.at(j)) == sId) {
                aMerged[j] = stE;   // 同身份 → 以任务表内容更新
                bReplaced = true;
                break;
            }
        }
        if (!bReplaced) {
            aMerged.append(stE);   // 新身份 → 追加（全局只增不减）
        }
    }
    const int iMergedActive = aGlobal.isEmpty()
        ? ((a_iActive >= 0 && a_iActive < aMerged.size()) ? a_iActive : -1)
        : iGlobalActive;
    SaveModelsTo(sGlobal, a_pSect, aMerged, iMergedActive);
}

void CProjectManager::SaveRecent() {
    // 落盘失败不抛、不退出（NFR-3.3）；内存列表已更新，本会话仍可用。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Recent");

    // 2026.09.09 v2.1.140 改 WritePrivateProfileSectionW 整节替换：旧版逐键写只增不删，
    //   列表缩短后高位残留键永留文件（[Recent] 膨胀事故里 1.1MB 残体正是这么留下的，
    //   profile 每次读都要扫全段）；整节替换会把未出现在新缓冲里的键全部删除（MSDN），
    //   一次调用收敛文件——旧实例投毒后的高位键在本实例下一次保存时即被清干净。
    //   缓冲格式："k=v\0k=v\0...\0\0"（wstring 自带尾部终止符补出双 null）。
    // ini 中 1 = 最近（MRU），索引 0 → key "1"；打开次数 → 附加键 "C<i>"（2026.09.20，
    //   用于统计各工程使用/修改频率；整节替换会同步清掉高位残留的旧 C 键）。
    QString sBody = QStringLiteral("Count=%1").arg(m_aRecentProjects.size());
    for (int i = 0; i < m_aRecentProjects.size(); ++i) {
        sBody += QChar(u'\0');
        sBody += QStringLiteral("%1=%2").arg(i + 1).arg(m_aRecentProjects.at(i));
        sBody += QChar(u'\0');
        sBody += QStringLiteral("C%1=%2").arg(i + 1)
                     .arg(i < m_aiRecentOpens.size() ? m_aiRecentOpens.at(i) : 1);
    }
    const std::wstring wsBody = sBody.toStdWString();
    WritePrivateProfileSectionW(wsSect.c_str(), wsBody.c_str(), wsPath.c_str());
}

void CProjectManager::LoadRecent() {
    // config.ini 不存在 → GetPrivateProfileInt 返回默认值 0 → 列表空（NFR-2.4）。
    // 2026.09.09 v2.1.139 修「启动极慢 + 最近列表膨胀到两万条」勿回退：
    //   ① 先 clear()——旧版直接在既有列表上 prepend，而 v2.1.123 起 OnConfigFileChanged
    //     热同步会多次调本函数，多实例共享 config 时每次热重载都把全量列表再叠一遍
    //     （内存滚雪球，叠出重复项后被 SaveRecent 全量落盘——副本 config 实测膨胀到
    //     21744 条/1.1MB，启动仅 LoadRecent 就要两万余次 profile 读）；
    //   ② 载入即按 NormalizePath 去重（保首个＝MRU 最新），旧档里的历史重复项自此
    //     自愈，配合 AddRecent/RemoveRecent 的「删全部匹配」双保险。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Recent");

    const int iCountCfg = GetPrivateProfileIntW(wsSect.c_str(), L"Count", 0, wsPath.c_str());
    m_aRecentProjects.clear();
    m_aiRecentOpens.clear();   // 2026.09.20 计数随列表同清（热同步重入安全）

    QStringList aLoaded;
    QList<int> aLoadedOpens;
    QSet<QString> aSeen;
    auto fnReadKey = [&wsSect, &wsPath](int a_iIdx) -> QString {
        wchar_t szBuf[MAX_PATH] = {0};
        const DWORD dwCopied = GetPrivateProfileStringW(
            wsSect.c_str(),
            QString::number(a_iIdx).toStdWString().c_str(),
            L"",
            szBuf, MAX_PATH, wsPath.c_str());
        return dwCopied == 0 ? QString()
                              : QString::fromWCharArray(szBuf, static_cast<int>(dwCopied));
    };
    // 2026.09.20 打开次数键 "C<i>"（旧档无此键 → 1：能在列表里即被打开过至少一次）。
    auto fnReadOpens = [&wsSect, &wsPath](int a_iIdx) -> int {
        const int iVal = GetPrivateProfileIntW(
            wsSect.c_str(),
            QStringLiteral("C%1").arg(a_iIdx).toStdWString().c_str(),
            1, wsPath.c_str());
        return iVal > 0 ? iVal : 1;
    };
    // ini 中 1 = 最近（MRU）；顺序读入保序，重复路径（规范化后相同）只留首个
    //   （计数取重复项最大——叠加 bug 副本同源，取最大最接近真实）。
    if (iCountCfg > 0) {
        for (int i = 1; i <= iCountCfg; ++i) {
            const QString sVal = fnReadKey(i);
            if (sVal.isEmpty()) {
                continue;
            }
            const QString sKey = NormalizePath(sVal);
            const int iOpens = fnReadOpens(i);
            const int iDupIdx = aLoaded.isEmpty() ? -1 : aLoaded.indexOf(sVal);
            if (!aSeen.contains(sKey)) {
                aSeen.insert(sKey);
                aLoaded.append(sVal);
                aLoadedOpens.append(iOpens);
            } else if (iDupIdx >= 0 && iOpens > aLoadedOpens.at(iDupIdx)) {
                aLoadedOpens[iDupIdx] = iOpens;
            }
        }
    } else {
        // 2026.09.09 v2.1.142 Count 键丢失/损坏兑底：如节头同行粘连（换行丢失后
        //   「[Recent]Count=52」整行被解析器当节头、Count 永远读不到——今日数据修复
        //   脚本实害，用户表现为「最近打开列表凭空消失」）但编号键仍在时，顺序探测
        //   重建（首个缺号即止，上限 1000 防坏档拖死启动）；正常空表仅一次探测零开销。
        for (int i = 1; i <= 1000; ++i) {
            const QString sVal = fnReadKey(i);
            if (sVal.isEmpty()) {
                break;
            }
            const QString sKey = NormalizePath(sVal);
            const int iOpens = fnReadOpens(i);
            const int iDupIdx = aLoaded.isEmpty() ? -1 : aLoaded.indexOf(sVal);
            if (!aSeen.contains(sKey)) {
                aSeen.insert(sKey);
                aLoaded.append(sVal);
                aLoadedOpens.append(iOpens);
            } else if (iDupIdx >= 0 && iOpens > aLoadedOpens.at(iDupIdx)) {
                aLoadedOpens[iDupIdx] = iOpens;
            }
        }
    }
    m_aRecentProjects = aLoaded;
    m_aiRecentOpens = aLoadedOpens;
    // 计数表与路径表长度对齐（防御：历史档/异常态）。
    while (m_aiRecentOpens.size() > m_aRecentProjects.size()) {
        m_aiRecentOpens.removeLast();
    }
    while (m_aiRecentOpens.size() < m_aRecentProjects.size()) {
        m_aiRecentOpens.append(1);
    }
}

bool CProjectManager::SaveWindowGeometry(const QRect& a_stGeometry, bool a_bMaximized) {
    // 落盘失败不抛、不退出；内存几何仍有效（与 SaveRecent 一致）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Window");

    WritePrivateProfileStringW(wsSect.c_str(), L"X",
        QString::number(a_stGeometry.x()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"Y",
        QString::number(a_stGeometry.y()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"W",
        QString::number(a_stGeometry.width()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"H",
        QString::number(a_stGeometry.height()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"Maximized",
        QString::number(a_bMaximized ? 1 : 0).toStdWString().c_str(), wsPath.c_str());
    return true;
}

bool CProjectManager::LoadWindowGeometry(QRect& a_stGeometry, bool& a_bMaximized) {
    // config.ini 不存在 / 段缺失 → GetPrivateProfileIntW 返回哨兵 → 判定无记录。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Window");

    // 哨兵 = INT_MIN：合法屏幕坐标不会等于它，借此区分「缺键」与「合法 0」。
    const int iSentinel = INT_MIN;
    const int iX = GetPrivateProfileIntW(wsSect.c_str(), L"X",     iSentinel, wsPath.c_str());
    const int iY = GetPrivateProfileIntW(wsSect.c_str(), L"Y",     iSentinel, wsPath.c_str());
    const int iW = GetPrivateProfileIntW(wsSect.c_str(), L"W",     iSentinel, wsPath.c_str());
    const int iH = GetPrivateProfileIntW(wsSect.c_str(), L"H",     iSentinel, wsPath.c_str());
    if (iX == iSentinel || iY == iSentinel || iW == iSentinel || iH == iSentinel) {
        return false;   // 任一键缺失 → 视为无有效记录，调用方用默认几何
    }

    a_stGeometry.setRect(iX, iY, iW, iH);
    a_bMaximized = (GetPrivateProfileIntW(wsSect.c_str(), L"Maximized", 0, wsPath.c_str()) != 0);
    return true;
}

// 2026.08.27 设置对话框几何持久化（[SettingsWindow] X/Y/W/H；无最大化态）：
//   口径同主窗口 [Window]，对话类窗口不记 Maximized。
bool CProjectManager::SaveSettingsDialogGeometry(const QRect& a_stGeometry) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"SettingsWindow");

    WritePrivateProfileStringW(wsSect.c_str(), L"X",
        QString::number(a_stGeometry.x()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"Y",
        QString::number(a_stGeometry.y()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"W",
        QString::number(a_stGeometry.width()).toStdWString().c_str(), wsPath.c_str());
    WritePrivateProfileStringW(wsSect.c_str(), L"H",
        QString::number(a_stGeometry.height()).toStdWString().c_str(), wsPath.c_str());
    return true;
}

bool CProjectManager::LoadSettingsDialogGeometry(QRect& a_stGeometry) {
    // 哨兵 = INT_MIN：区分「缺键」与合法坐标 0（同 LoadWindowGeometry 口径）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"SettingsWindow");

    const int iSentinel = INT_MIN;
    const int iX = GetPrivateProfileIntW(wsSect.c_str(), L"X", iSentinel, wsPath.c_str());
    const int iY = GetPrivateProfileIntW(wsSect.c_str(), L"Y", iSentinel, wsPath.c_str());
    const int iW = GetPrivateProfileIntW(wsSect.c_str(), L"W", iSentinel, wsPath.c_str());
    const int iH = GetPrivateProfileIntW(wsSect.c_str(), L"H", iSentinel, wsPath.c_str());
    if (iX == iSentinel || iY == iSentinel || iW == iSentinel || iH == iSentinel) {
        return false;
    }
    a_stGeometry.setRect(iX, iY, iW, iH);
    return true;
}

bool CProjectManager::SaveSplitterState(const QByteArray& a_stState) {
    // FR-4：把 QSplitter::saveState() 字节数组以 Base64 文本落 [Layout] Splitter；
    // 失败不抛（与 SaveWindowGeometry 一致）。Base64 文本无换行/特殊字符，适合 INI。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const QString sB64 = QString::fromLatin1(a_stState.toBase64());
    WritePrivateProfileStringW(L"Layout", L"Splitter",
                               sB64.toStdWString().c_str(), wsPath.c_str());
    return true;
}

QByteArray CProjectManager::LoadSplitterState() {
    // FR-4：读 [Layout] Splitter（Base64 文本）→ 还原字节数组；缺键/空 → 返回空数组。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return QByteArray();
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[8192] = {0};
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Layout", L"Splitter", L"", szBuf, 8192, wsPath.c_str());
    if (dwCopied == 0) {
        return QByteArray();
    }
    const QString sB64 = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied));
    return QByteArray::fromBase64(sB64.toLatin1());
}

bool CProjectManager::SaveLayoutValue(const QString& a_sKey, const QString& a_sVal) {
    // 2026.08.29 [Layout] 通用键值写（profile API，与该节其余键同一写入者家族）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty() || a_sKey.isEmpty()) {
        return false;
    }
    WritePrivateProfileStringW(L"Layout", a_sKey.toStdWString().c_str(),
                               a_sVal.toStdWString().c_str(), sPath.toStdWString().c_str());
    return true;
}

QString CProjectManager::LoadLayoutValue(const QString& a_sKey) const {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty() || a_sKey.isEmpty()) {
        return QString();
    }
    wchar_t szBuf[8192] = {0};
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Layout", a_sKey.toStdWString().c_str(), L"", szBuf, 8192,
        sPath.toStdWString().c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dwCopied));
}

bool CProjectManager::SaveVSplitState(const QByteArray& a_stState) {
    // 2026.08.18：任务列表高度分隔条 saveState() 落 [Layout] VSplit；与 SaveSplitterState 同形。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const QString sB64 = QString::fromLatin1(a_stState.toBase64());
    WritePrivateProfileStringW(L"Layout", L"VSplit",
                               sB64.toStdWString().c_str(), wsPath.c_str());
    return true;
}

QByteArray CProjectManager::LoadVSplitState() {
    // 2026.08.18：读 [Layout] VSplit → 还原字节数组；缺键/空 → 空数组（调用方跳过 restoreState）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return QByteArray();
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[8192] = {0};
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Layout", L"VSplit", L"", szBuf, 8192, wsPath.c_str());
    if (dwCopied == 0) {
        return QByteArray();
    }
    const QString sB64 = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied));
    return QByteArray::fromBase64(sB64.toLatin1());
}

bool CProjectManager::SaveAutoCompactPct(int a_iPct) {
    // 2026.08.15 比例制：落 [Adhoc] AutoCompactPct（替代已删除的 [Adhoc] AutoCompact 千 token 档位）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Adhoc", L"AutoCompactPct",
        QString::number(a_iPct).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadAutoCompactPct() const {
    // 无 config.ini / 缺键 → 默认 75；非法值（<10 或 >95）兜底回 75。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 75;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iPct = GetPrivateProfileIntW(L"Adhoc", L"AutoCompactPct", 75, wsPath.c_str());
    return (iPct >= 10 && iPct <= 95) ? iPct : 75;
}

bool CProjectManager::IsGuideBusy() const {
    // guide 生成/同步 runner 任一执行中即占用（后端切换/漂移重建的安全窗口判定）。
    return (m_pGuideRunner != nullptr && m_pGuideRunner->IsRunning())
        || (m_pGuideSyncRunner != nullptr && m_pGuideSyncRunner->IsRunning());
}

int CProjectManager::AutoCompactKFor(const QString& a_sModel) const {
    // 2026.08.15 比例制换算：K（千 token）= 窗口 × 比例 ÷ 100 ÷ 1000。
    // 窗口优先用 [Adhoc] ContextLimit 用户覆盖，未设回退按模型探测；K 至少 1。
    const qint64 iUserLimit = LoadAdhocContextLimit();
    const qint64 iWindow = (iUserLimit > 0)
        ? iUserLimit : CClaudeRunner::ContextWindowForModel(a_sModel);
    return static_cast<int>(qMax<qint64>(1, iWindow * LoadAutoCompactPct() / 100 / 1000));
}

bool CProjectManager::SaveMaxIter(int a_iN) {
    // 落 [Policy] MaxIter（单阶段最大迭代次数）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Policy", L"MaxIter",
        QString::number(a_iN).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadMaxIter() {
    // 无 config.ini/缺键 → 默认 3；非法值（<1 或 >9）兜底回 3。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"MaxIter", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 3;
}

bool CProjectManager::LoadReviewAskOnEscalate() {
    // 2026.09.22 [Policy] ReviewAskOnEscalate：评审驳回迭代熔断升级时是否弹人工介入窗
    //   （放行/再迭代/保持暂停）。缺省开；置 0 退回旧版静默升级（无人值守跑批可关）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return true;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Policy", L"ReviewAskOnEscalate", 1, wsPath.c_str()) != 0;
}

bool CProjectManager::LoadReviewSdkOnStage() {
    // 2026-09-30 [Review] SdkOnStage：任务 CodeReview 阶段 ReviewSdk 静态预审开关。
    //   缺省开（用户需求：审查环节接入该模块）；置 0 回纯 agent 评审。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return true;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Review", L"SdkOnStage", 1, wsPath.c_str()) != 0;
}

int CProjectManager::LoadReviewSdkMaxFiles() {
    // [Review] SdkMaxFiles：单轮送审文件上限（缺省 12；合法 1..50 兑底回 12）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 12;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Review", L"SdkMaxFiles", 12, wsPath.c_str());
    return (iN >= 1 && iN <= 50) ? iN : 12;
}

int CProjectManager::LoadReviewSdkTimeoutSec() {
    // [Review] SdkTimeoutSec：单文件 LLM 调用超时秒（缺省 600；合法 60..3600）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 600;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Review", L"SdkTimeoutSec", 600, wsPath.c_str());
    return (iN >= 60 && iN <= 3600) ? iN : 600;
}

bool CProjectManager::SaveMaxResume(int a_iN) {
    // 落 [Policy] MaxResume（驳回后单阶段最大续跑次数）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Policy", L"MaxResume",
        QString::number(a_iN).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadMaxResume() {
    // 无 config.ini/缺键 → 默认 3；非法值（<1 或 >9）兜底回 3。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"MaxResume", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 3;
}

bool CProjectManager::SaveReviewResume(int a_iN) {
    // 落 [Policy] ReviewResume（评审阶段驳回后最大续跑次数）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Policy", L"ReviewResume",
        QString::number(a_iN).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadReviewResume() {
    // 无 config.ini/缺键 → 默认 1；非法值（<1 或 >9）兜底回 1。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 1;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"ReviewResume", 1, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 1;
}

bool CProjectManager::SaveAgentBackend(int a_iBackend) {
    // 2026.08.18 落 [Agent] Backend（1=claude，2=dsh，3=pi）；
    // 2026.08.21 修：扩展 4=codex——旧实现 1..3 钳位把 Codex 兜底改写回 1，
    //   表现为「设置页选 CodexHarness 确认后无效」（其余键正常落盘）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    // 任务 3213cdbf 第五后端 Hermes：扩为 1..5（越界仍兜底回 1，非 qBound 夹边界）。
    WritePrivateProfileStringW(L"Agent", L"Backend",
        QString::number((a_iBackend >= 1 && a_iBackend <= 5) ? a_iBackend : 1).toStdWString().c_str(),
        wsPath.c_str());
    return true;
}

int CProjectManager::LoadAgentBackend() {
    // 2026.08.18 无 config.ini/缺键 → 默认 1（claude）；仅认合法值，其余兜底 1。
    // 2026.08.21 修：同 SaveAgentBackend 扩展 4=codex（旧 1..3 钳位读不回 4）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 1;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iBackend = GetPrivateProfileIntW(L"Agent", L"Backend", 1, wsPath.c_str());
    // 任务 3213cdbf 同 SaveAgentBackend 扩为 1..5（5=hermes；越界兜底回 1）。
    return (iBackend >= 1 && iBackend <= 5) ? iBackend : 1;
}

// 2026.08.18 DSH 三路径 + provider/model 的存取（[Agent] 段）。路径空=未配置
//   （CDeepSeekRunner::StartStage 前置校验带具体缺失项报失败）；provider/model
//   空串回退默认 glm-custom / glm-5.2（与 CDeepSeekRunner 静态默认一致）。
static bool SaveDshStrKey(const QString& a_sPathCfg, const wchar_t* a_lpszKey,
                          const QString& a_sValue) {
    const QString sPath = a_sPathCfg;
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Agent", a_lpszKey,
        a_sValue.trimmed().toStdWString().c_str(), wsPath.c_str());
    return true;
}

static QString LoadDshStrKey(const QString& a_sPathCfg, const wchar_t* a_lpszKey,
                             const QString& a_sDefault) {
    const QString sPath = a_sPathCfg;
    if (sPath.isEmpty()) {
        return a_sDefault;
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[512] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Agent", a_lpszKey, a_sDefault.toStdWString().c_str(), szBuf, 512, wsPath.c_str());
    const QString sValue = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
    return sValue.isEmpty() ? a_sDefault : sValue;
}

bool CProjectManager::SaveDshRoot(const QString& a_sRoot) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshRoot", a_sRoot);
}

QString CProjectManager::LoadDshRoot() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshRoot", QString());
}

bool CProjectManager::SaveDshExtraWriteDirs(const QStringList& a_aDirs) {
    // 2026.09.27 额外可写目录（[Dsh] ExtraWriteDirs；';' 分隔原样落盘，运行期规范化）。
    return SaveDshStrKey(SettingsCfgPath(), L"ExtraWriteDirs", a_aDirs.join(QLatin1Char(';')));
}

QStringList CProjectManager::LoadDshExtraWriteDirs() {
    const QString sRaw = LoadDshStrKey(SettingsCfgPath(), L"ExtraWriteDirs", QString());
    QStringList aDirs;
    for (const QString& s : sRaw.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString sT = s.trimmed();
        if (!sT.isEmpty() && !aDirs.contains(sT)) {
            aDirs.append(sT);
        }
    }
    return aDirs;
}

bool CProjectManager::SaveDshNodePath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshNodePath", a_sPath);
}

QString CProjectManager::LoadDshNodePath() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshNodePath", QString());
}

bool CProjectManager::SaveDshBinJsPath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshBinJsPath", a_sPath);
}

QString CProjectManager::LoadDshBinJsPath() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshBinJsPath", QString());
}

bool CProjectManager::SaveDshCordisPath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshCordisPath", a_sPath);
}

QString CProjectManager::LoadDshCordisPath() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshCordisPath", QString());
}

bool CProjectManager::SaveDshProvider(const QString& a_sProvider) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshProvider", a_sProvider);
}

QString CProjectManager::LoadDshProvider() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshProvider", QStringLiteral("glm-custom"));
}

bool CProjectManager::SaveDshModel(const QString& a_sModel) {
    return SaveDshStrKey(SettingsCfgPath(), L"DshModel", a_sModel);
}

QString CProjectManager::LoadDshModel() {
    return LoadDshStrKey(SettingsCfgPath(), L"DshModel", QStringLiteral("glm-5.2"));
}

// 2026.08.18 DSH 计费单价（$/百万 token，[Agent] DshPrice*）：DSH 协议只报 token
//   不报金额，客户端按单价折算；默认对齐 claude 后端 Sonnet 档价目（同网关口径）。
//   INI 无 double 型，走字符串互转（'g' 短格式；解析失败回退默认）。
static bool SaveDshDblKey(const QString& a_sPathCfg, const wchar_t* a_lpszKey,
                          double a_dValue) {
    return SaveDshStrKey(a_sPathCfg, a_lpszKey, QString::number(a_dValue, 'g', 8));
}

static double LoadDshDblKey(const QString& a_sPathCfg, const wchar_t* a_lpszKey,
                            double a_dDefault) {
    bool bOk = false;
    const double dValue = LoadDshStrKey(a_sPathCfg, a_lpszKey,
                                        QString::number(a_dDefault, 'g', 8)).toDouble(&bOk);
    return bOk ? qMax(0.0, dValue) : a_dDefault;
}

bool CProjectManager::SaveDshPriceInPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"DshPriceInPerM", a_d);
}

double CProjectManager::LoadDshPriceInPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"DshPriceInPerM", 3.0);
}

bool CProjectManager::SaveDshPriceCacheReadPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"DshPriceCacheReadPerM", a_d);
}

double CProjectManager::LoadDshPriceCacheReadPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"DshPriceCacheReadPerM", 0.3);
}

bool CProjectManager::SaveDshPriceCacheWritePerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"DshPriceCacheWritePerM", a_d);
}

double CProjectManager::LoadDshPriceCacheWritePerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"DshPriceCacheWritePerM", 3.75);
}

bool CProjectManager::SaveDshPriceOutPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"DshPriceOutPerM", a_d);
}

// 2026.08.20 PIAgent 计费单价（$/百万 token，[Agent] PiPrice*PerM）：pi 协议只报
//   token 分量不报金额，客户端按单价折算；默认对齐 DSH/claude 同档价目。
bool CProjectManager::SavePiPriceInPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"PiPriceInPerM", a_d);
}
double CProjectManager::LoadPiPriceInPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"PiPriceInPerM", 3.0);
}
bool CProjectManager::SavePiPriceCacheReadPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"PiPriceCacheReadPerM", a_d);
}
double CProjectManager::LoadPiPriceCacheReadPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"PiPriceCacheReadPerM", 0.3);
}
bool CProjectManager::SavePiPriceCacheWritePerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"PiPriceCacheWritePerM", a_d);
}
double CProjectManager::LoadPiPriceCacheWritePerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"PiPriceCacheWritePerM", 3.75);
}
bool CProjectManager::SavePiPriceOutPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"PiPriceOutPerM", a_d);
}
double CProjectManager::LoadPiPriceOutPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"PiPriceOutPerM", 15.0);
}

double CProjectManager::LoadDshPriceOutPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"DshPriceOutPerM", 15.0);
}

// 2026.08.18 自定义大模型表（[DshModels] Count/Active/M<i>*）：DSH 后端跨机器
//   自配 LLM 的多条目管理（设置页维护，CMainWindow 启动/保存时注入 runner）。
//   密钥经 INI 明文存储（设置页 tooltip 已告知）；读缓冲放宽到 1024 防长密钥截断。
//   节与 [Agent] 不同，不复用 SaveDshStrKey/LoadDshStrKey（二者硬编码 L"Agent"）。
QList<SDshModelEntry> CProjectManager::LoadDshModels(int* a_pActive) {
    // 2026.09.09 v2.1.144 改经 LoadModelsFrom（任务级覆盖：任务档无本表时整表实时回退全局）。
    return LoadModelsFrom(TableCfgPath(L"DshModels"), L"DshModels", a_pActive);
}

bool CProjectManager::SaveDshModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026.09.09 v2.1.144 改经 SaveModelsTo + 任务态全局超集同步（缺省恒保有最多模型配置）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    if (!SaveModelsTo(sPath, L"DshModels", a_aModels, a_iActive)) {
        return false;
    }
    SyncModelTableToGlobal(L"DshModels", a_aModels, a_iActive);
    return true;
}

// 2026.08.19 PIAgent 第三后端键（[Agent] Pi*）：root/node/provider/model/apiKey/thinking
//   六项，存取口径同 Dsh*（SaveDshStrKey/LoadDshStrKey 走 [Agent] 节，直接复用）。
bool CProjectManager::SavePiRoot(const QString& a_sRoot) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiRoot", a_sRoot);
}

QString CProjectManager::LoadPiRoot() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiRoot", QString());
}

bool CProjectManager::SavePiNodePath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiNodePath", a_sPath);
}

QString CProjectManager::LoadPiNodePath() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiNodePath", QString());
}

bool CProjectManager::SavePiProvider(const QString& a_sProvider) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiProvider", a_sProvider);
}

QString CProjectManager::LoadPiProvider() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiProvider", QStringLiteral("anthropic"));
}

bool CProjectManager::SavePiModel(const QString& a_sModel) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiModel", a_sModel);
}

QString CProjectManager::LoadPiModel() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiModel", QString());
}

bool CProjectManager::SavePiApiKey(const QString& a_sKey) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiApiKey", a_sKey);
}

QString CProjectManager::LoadPiApiKey() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiApiKey", QString());
}

bool CProjectManager::SavePiThinking(const QString& a_sLevel) {
    return SaveDshStrKey(SettingsCfgPath(), L"PiThinking", a_sLevel);
}

QString CProjectManager::LoadPiThinking() {
    return LoadDshStrKey(SettingsCfgPath(), L"PiThinking", QString());
}

// 2026.08.20 PiAgent 自定义大模型表（[PiModels]；实现口径与 [DshModels] 完全一致，
//   仅节名不同——条目本体由 CPiAgentRunner 合成写 ~/.pi/agent/models.json 生效）。
QList<SDshModelEntry> CProjectManager::LoadPiModels(int* a_pActive) {
    // 2026.09.09 v2.1.144 改经 LoadModelsFrom（任务级覆盖：任务档无本表时整表实时回退全局）。
    return LoadModelsFrom(TableCfgPath(L"PiModels"), L"PiModels", a_pActive);
}

bool CProjectManager::SavePiModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026.09.09 v2.1.144 改经 SaveModelsTo + 任务态全局超集同步（缺省恒保有最多模型配置）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    if (!SaveModelsTo(sPath, L"PiModels", a_aModels, a_iActive)) {
        return false;
    }
    SyncModelTableToGlobal(L"PiModels", a_aModels, a_iActive);
    return true;
}

// 2026.08.21 Codex 自定义大模型表（[CodexModels]；口径同 [PiModels]，差异仅一点：
//   默认 API 格式 = chat（Codex wire_api，v0.88+ 仅 responses））。
QList<SDshModelEntry> CProjectManager::LoadCodexModels(int* a_pActive) {
    // 2026.09.09 v2.1.144 改经 LoadModelsFrom（任务级覆盖：任务档无本表时整表实时回退全局）。
    return LoadModelsFrom(TableCfgPath(L"CodexModels"), L"CodexModels", a_pActive);
}

bool CProjectManager::SaveCodexModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026.09.09 v2.1.144 改经 SaveModelsTo + 任务态全局超集同步（缺省恒保有最多模型配置）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    if (!SaveModelsTo(sPath, L"CodexModels", a_aModels, a_iActive)) {
        return false;
    }
    SyncModelTableToGlobal(L"CodexModels", a_aModels, a_iActive);
    return true;
}

// CodexHarness 第四后端（OpenAI Codex CLI 0.87.0，[Agent] Codex* 键；口径同 Pi*）。
bool CProjectManager::SaveCodexRoot(const QString& a_sRoot) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexRoot", a_sRoot);
}

QString CProjectManager::LoadCodexRoot() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexRoot", QString());
}

bool CProjectManager::SaveCodexNodePath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexNodePath", a_sPath);
}

QString CProjectManager::LoadCodexNodePath() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexNodePath", QString());
}

bool CProjectManager::SaveCodexProvider(const QString& a_sProvider) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexProvider", a_sProvider);
}

QString CProjectManager::LoadCodexProvider() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexProvider", QString());
}

bool CProjectManager::SaveCodexModel(const QString& a_sModel) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexModel", a_sModel);
}

QString CProjectManager::LoadCodexModel() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexModel", QString());
}

bool CProjectManager::SaveCodexApiKey(const QString& a_sKey) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexApiKey", a_sKey);
}

QString CProjectManager::LoadCodexApiKey() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexApiKey", QString());
}

bool CProjectManager::SaveCodexBaseUrl(const QString& a_sUrl) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexBaseUrl", a_sUrl);
}

QString CProjectManager::LoadCodexBaseUrl() {
    return LoadDshStrKey(SettingsCfgPath(), L"CodexBaseUrl", QString());
}

bool CProjectManager::SaveCodexWireApi(const QString& a_sApi) {
    return SaveDshStrKey(SettingsCfgPath(), L"CodexWireApi", a_sApi);
}

QString CProjectManager::LoadCodexWireApi() {
    // 默认 chat（v0.88+ 移除 wire_api="chat"，本后端锁定 0.87.0 按 chat 网关接入）。
    const QString sApi = LoadDshStrKey(SettingsCfgPath(), L"CodexWireApi",
                                       QStringLiteral("chat"));
    return sApi.isEmpty() ? QStringLiteral("chat") : sApi;
}

// ---------------------------------------------------------------------------
// Hermes 第五后端键（任务 3213cdbf，[Agent] Hermes* / [HermesModels]）：存取口径
//   与 Pi/Codex 同（SaveDshStrKey/LoadDshStrKey/SaveDshDblKey/LoadDshDblKey 走
//   [Agent] 节 + Win32 profile API，GBK 安全）；模型表口径同 [PiModels]。
// ---------------------------------------------------------------------------
bool CProjectManager::SaveHermesRoot(const QString& a_sRoot) {
    return SaveDshStrKey(SettingsCfgPath(), L"HermesRoot", a_sRoot);
}

QString CProjectManager::LoadHermesRoot() {
    return LoadDshStrKey(SettingsCfgPath(), L"HermesRoot", QString());
}

bool CProjectManager::SaveHermesNodePath(const QString& a_sPath) {
    return SaveDshStrKey(SettingsCfgPath(), L"HermesNodePath", a_sPath);
}

QString CProjectManager::LoadHermesNodePath() {
    return LoadDshStrKey(SettingsCfgPath(), L"HermesNodePath", QString());
}

bool CProjectManager::SaveHermesModel(const QString& a_sModel) {
    return SaveDshStrKey(SettingsCfgPath(), L"HermesModel", a_sModel);
}

QString CProjectManager::LoadHermesModel() {
    return LoadDshStrKey(SettingsCfgPath(), L"HermesModel", QString());
}

bool CProjectManager::SaveHermesApiKey(const QString& a_sKey) {
    return SaveDshStrKey(SettingsCfgPath(), L"HermesApiKey", a_sKey);
}

QString CProjectManager::LoadHermesApiKey() {
    return LoadDshStrKey(SettingsCfgPath(), L"HermesApiKey", QString());
}

bool CProjectManager::SaveHermesBaseUrl(const QString& a_sUrl) {
    return SaveDshStrKey(SettingsCfgPath(), L"HermesBaseUrl", a_sUrl);
}

QString CProjectManager::LoadHermesBaseUrl() {
    // 空回退内网网关基址（与既有四后端网关口径一致；设置页可改）。
    return LoadDshStrKey(SettingsCfgPath(), L"HermesBaseUrl",
                         QStringLiteral("http://192.168.118.128:9000"));
}

bool CProjectManager::SaveHermesPriceInPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"HermesPriceInPerM", a_d);
}

double CProjectManager::LoadHermesPriceInPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"HermesPriceInPerM", 3.0);
}

bool CProjectManager::SaveHermesPriceCacheReadPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"HermesPriceCacheReadPerM", a_d);
}

double CProjectManager::LoadHermesPriceCacheReadPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"HermesPriceCacheReadPerM", 0.3);
}

bool CProjectManager::SaveHermesPriceCacheWritePerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"HermesPriceCacheWritePerM", a_d);
}

double CProjectManager::LoadHermesPriceCacheWritePerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"HermesPriceCacheWritePerM", 3.75);
}

bool CProjectManager::SaveHermesPriceOutPerM(double a_d) {
    return SaveDshDblKey(SettingsCfgPath(), L"HermesPriceOutPerM", a_d);
}

double CProjectManager::LoadHermesPriceOutPerM() {
    return LoadDshDblKey(SettingsCfgPath(), L"HermesPriceOutPerM", 15.0);
}

// Hermes 自定义大模型表（[HermesModels]；口径同 [PiModels]，默认 API 格式
//   anthropic-messages；maxTokens 保存侧不在此钳位——设置页与 CHermesRunner
//   EnsureRuntimeConfig 双侧 qMin(131072) 收口，本层保持透传）。
QList<SDshModelEntry> CProjectManager::LoadHermesModels(int* a_pActive) {
    // 2026.09.09 v2.1.144 改经 LoadModelsFrom（任务级覆盖：任务档无本表时整表实时回退全局）。
    return LoadModelsFrom(TableCfgPath(L"HermesModels"), L"HermesModels", a_pActive);
}

bool CProjectManager::SaveHermesModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026.09.09 v2.1.144 改经 SaveModelsTo + 任务态全局超集同步（缺省恒保有最多模型配置）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    if (!SaveModelsTo(sPath, L"HermesModels", a_aModels, a_iActive)) {
        return false;
    }
    SyncModelTableToGlobal(L"HermesModels", a_aModels, a_iActive);
    return true;
}

bool CProjectManager::SaveClaudeCmd(const QString& a_sCmd) {
    // 落 [Agent] ClaudeCmd（claude 命令，可含路径）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Agent", L"ClaudeCmd",
        a_sCmd.trimmed().toStdWString().c_str(), wsPath.c_str());
    return true;
}

QString CProjectManager::LoadClaudeCmd() {
    // 无 config.ini / 缺键 / 空 → 默认 "claude"（PATH 上找）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return QStringLiteral("claude");
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[MAX_PATH] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Agent", L"ClaudeCmd", L"claude", szBuf, MAX_PATH, wsPath.c_str());
    const QString sCmd = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
    return sCmd.isEmpty() ? QStringLiteral("claude") : sCmd;
}

bool CProjectManager::SaveAskTimeoutSec(int a_iSec) {
    // 落 [Agent] AskTimeoutSec（ask_user 提问等待超时秒数）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Agent", L"AskTimeoutSec",
        QString::number(a_iSec).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadAskTimeoutSec() {
    // 无 config.ini/缺键 → 默认 600（10 分钟）；非法值（<30）兜底回 600。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 600;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iSec = GetPrivateProfileIntW(L"Agent", L"AskTimeoutSec", 600, wsPath.c_str());
    return (iSec >= 30 && iSec <= 86400) ? iSec : 600;
}

int CProjectManager::LoadStallMinutes() {
    // 2026.08.17 [Agent] StallMinutes：无 config.ini/缺键 → 默认 10 分钟；非法值（<1 或 >120）
    //   兜底回 10。阈值下限保护长工具执行（如分钟级构建）不误杀；上限防配置手误致任务假死半天。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 10;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iMin = GetPrivateProfileIntW(L"Agent", L"StallMinutes", 10, wsPath.c_str());
    return (iMin >= 1 && iMin <= 120) ? iMin : 10;
}

bool CProjectManager::SaveScanExcludeDirs(const QString& a_sDirs) {
    // 落 [Scan] ExcludeDirs（逗号分隔目录名清单）；空串=写空（回退内置默认）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Scan", L"ExcludeDirs",
        a_sDirs.toStdWString().c_str(), wsPath.c_str());
    return true;
}

QString CProjectManager::LoadScanExcludeDirs() {
    // 无 config.ini/缺键/空 → 返回空串（调用方用内置默认清单）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return QString();
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[2048] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Scan", L"ExcludeDirs", L"", szBuf, 2048, wsPath.c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
}

bool CProjectManager::SaveReviewParallel(int a_iN) {
    // 落 [Review] Parallel（评审多视角并行视角数）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Review", L"Parallel",
        QString::number(a_iN).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadReviewParallel() {
    // 无 config.ini/缺键 → 默认 3（全部评审默认并行）；非法值（<1 或 >8）兜底回 3。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Review", L"Parallel", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 8) ? iN : 3;
}

bool CProjectManager::SaveWebSearch(bool a_bOn) {
    // 2026.08.15 落 [Agent] WebSearch（阶段联网检索开关）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Agent", L"WebSearch",
        QString::number(a_bOn ? 1 : 0).toStdWString().c_str(), wsPath.c_str());
    return true;
}

bool CProjectManager::LoadWebSearch() {
    // 2026.08.15 无 config.ini/缺键 → 默认开（1）；0=全阶段禁用联网检索。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return true;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Agent", L"WebSearch", 1, wsPath.c_str()) != 0;
}

bool CProjectManager::SaveExpertAssistant(bool a_bOn) {
    // 落 [Adhoc] ExpertAssistant（1=启用专家助手闭环验证，0=禁用）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Adhoc", L"ExpertAssistant",
        QString::number(a_bOn ? 1 : 0).toStdWString().c_str(), wsPath.c_str());
    return true;
}

bool CProjectManager::LoadExpertAssistant() const {
    // 无 config.ini/缺键 → 默认关闭（0，2026.09.26 用户定稿：深度链耗 token，须显式启用）；
    //   1=手工启用。已有 config.ini 存了 1 的老用户不受影响（显式记录优先）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Adhoc", L"ExpertAssistant", 0, wsPath.c_str()) != 0;
}

int CProjectManager::LoadExpertRestarts() const {
    // 2026.09.26 专家助手深度链重启上限：无 config.ini/缺键 → 默认 1（重启一次）；
    //   韩位 0..5（0=不自动重启）。读即时生效，无需重启。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 1;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return qBound(0, static_cast<int>(GetPrivateProfileIntW(
        L"Adhoc", L"ExpertRestarts", 1, wsPath.c_str())), 5);
}

bool CProjectManager::SaveAdhocMaxTurns(int a_iN) {
    // 2026.09.23 落 [Adhoc] MaxTurns（助手单轮执行 --max-turns；默认 40，合法 10..200）。
    //   助手做大型多文件重构时 40 轮常打满（LayoutDesign 实测 12 问 11 次触顶），
    //   允许用户按工程工作量放大；每条提问仍受自动续跑 2 次兜底，不会无限跑。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Adhoc", L"MaxTurns",
        QString::number(qBound(10, a_iN, 200)).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadAdhocMaxTurns() const {
    // 无 config.ini/缺键 → 默认 40；非法值（<10 或 >200）兜底回 40。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 40;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Adhoc", L"MaxTurns", 40, wsPath.c_str());
    return (iN >= 10 && iN <= 200) ? iN : 40;
}

bool CProjectManager::SaveAdhocMaxTurnsAuto(bool a_bOn) {
    // 2026.09.23 v3.0.219 落 [Adhoc] MaxTurnsAuto（1=按提示词难度自动上调轮次，0=恒用基值）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Adhoc", L"MaxTurnsAuto",
        QString::number(a_bOn ? 1 : 0).toStdWString().c_str(), wsPath.c_str());
    return true;
}

bool CProjectManager::LoadAdhocMaxTurnsAuto() const {
    // 无 config.ini/缺键 → 默认开（难度评估是零成本纯启发式，缺省受益）；0=恒用基值。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return true;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Adhoc", L"MaxTurnsAuto", 1, wsPath.c_str()) != 0;
}

bool CProjectManager::SaveAdhocContextLimit(qint64 a_iLimit) {
    // 落 [Adhoc] ContextLimit（0 表示未设/回退自动探测）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Adhoc", L"ContextLimit",
        QString::number(a_iLimit).toStdWString().c_str(), wsPath.c_str());
    return true;
}

qint64 CProjectManager::LoadAdhocContextLimit() const {
    // 无 config.ini / 缺键 / 非法 → 返回 0（回退按模型自动探测）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return 0;
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[32] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Adhoc", L"ContextLimit", L"0", szBuf, 32, wsPath.c_str());
    const qint64 iLimit = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).toLongLong();
    return iLimit > 0 ? iLimit : 0;
}

// ===========================================================================
// FR-6 代码回退：源码备份 / 恢复 / 版本清单
// 纯本地 Qt 文件系统操作（NFR-4 自包含）；备份范围按 Q4 默认排除清单，排除 .autodev/
// 自身防递归（NFR-5）；恢复用临时目录中转保证原子性（NFR-5 高危，R3）。
// ===========================================================================

namespace {
// 把整数序号格式化为 4 位零填充字符串（0001、0002…，>9999 自然扩展）。
QString FormatSeq(int a_iSeq) {
    return QStringLiteral("%1").arg(a_iSeq, 4, 10, QLatin1Char('0'));
}

// 递归镜像拷贝：从 a_sSrc（工程根）拷到 a_sDst（备份目录或恢复临时目录），
// 跳过 a_aExclude 中的顶层目录名（按目录 basename 匹配，仅工程根下一级）。
// a_sSrcRoot 为判定基准（即 a_sSrc），用于计算相对路径与排除顶层目录。
bool MirrorTree(const QString& a_sSrc, const QString& a_sDst, const QStringList& a_aExclude) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    // 子目录：跳过排除清单（仅顶层一级生效——排除 .autodev 即整树跳过，防递归）。
    //   NoSymLinks：防 junction/symlink 环无限递归（CAS 扫描器同款防护）。
    const QStringList aDirs = stSrc.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString& sSub : aDirs) {
        if (a_aExclude.contains(sSub, Qt::CaseInsensitive)) {
            continue;
        }
        if (!MirrorTree(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub), a_aExclude)) {
            return false;
        }
    }
    // 文件拷贝
    const QStringList aFiles = stSrc.entryList(QDir::Files | QDir::NoSymLinks);
    for (const QString& sFile : aFiles) {
        const QString sSrcPath = stSrc.filePath(sFile);
        const QString sDstPath = QDir(a_sDst).filePath(sFile);
        // 目标已存在先删（恢复回滚 / 重备场景需覆盖）。
        if (QFileInfo::exists(sDstPath) && !QFile::remove(sDstPath)) {
            return false;
        }
        if (!QFile::copy(sSrcPath, sDstPath)) {
            return false;
        }
    }
    return true;
}

// 递归清空并删除目录（用于删 _restore_tmp / 回滚）。失败不中断（尽力清理）。
bool RemoveTree(const QString& a_sPath) {
    QDir stDir(a_sPath);
    if (!stDir.exists()) {
        return true;
    }
    return stDir.removeRecursively();
}

// —— zstd 逐文件压缩 / 解压（#2，备份体积优化）——
// 读源文件全部字节 → ZSTD 压缩/解压 → 写目标。任一 IO 或 zstd 错误返回 false。
bool ZstdCompressFile(const QString& a_sSrc, const QString& a_sDst) {
    CFrwDevice stSrc(a_sSrc);
    if (!stSrc.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray aSrc = stSrc.readAll();
    stSrc.close();
    // ZSTD_compressBound 给出压缩后上界；level 3 = zstd 默认档（速度/压缩率均衡，源码场景足够）。
    const size_t stBound = ZSTD_compressBound(static_cast<size_t>(aSrc.size()));
    if (ZSTD_isError(stBound) || stBound > static_cast<size_t>(INT_MAX)) {
        return false;   // 压缩上界超 int → QByteArray 容量溢出（源码场景不会触发，防御性）
    }
    QByteArray aDst(static_cast<int>(stBound), '\0');
    const size_t stOut = ZSTD_compress(aDst.data(), stBound,
                                        aSrc.constData(), static_cast<size_t>(aSrc.size()), 3);
    if (ZSTD_isError(stOut)) {
        return false;
    }
    aDst.resize(static_cast<int>(stOut));
    if (QFileInfo::exists(a_sDst) && !QFile::remove(a_sDst)) {
        return false;
    }
    QFile stOutFile(a_sDst);
    if (!stOutFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool bWrote = (stOutFile.write(aDst) == aDst.size());
    stOutFile.close();
    return bWrote;   // 磁盘满 / 写半截 → 视为失败（备份工具不容忍残缺文件）
}

bool ZstdDecompressFile(const QString& a_sSrc, const QString& a_sDst) {
    CFrwDevice stSrc(a_sSrc);
    if (!stSrc.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray aSrc = stSrc.readAll();
    stSrc.close();
    // 由压缩帧头取原始大小（本工具写入时大小已知，不会是 UNKNOWN）。
    const unsigned long long uSize = ZSTD_getFrameContentSize(
        aSrc.constData(), static_cast<size_t>(aSrc.size()));
    if (uSize == ZSTD_CONTENTSIZE_ERROR || uSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        uSize > static_cast<unsigned long long>(INT_MAX)) {
        return false;
    }
    QByteArray aDst(static_cast<int>(uSize), '\0');
    const size_t stOut = ZSTD_decompress(aDst.data(), static_cast<size_t>(uSize),
                                         aSrc.constData(), static_cast<size_t>(aSrc.size()));
    if (ZSTD_isError(stOut) || stOut != static_cast<size_t>(uSize)) {
        return false;
    }
    if (QFileInfo::exists(a_sDst) && !QFile::remove(a_sDst)) {
        return false;
    }
    QFile stOutFile(a_sDst);
    if (!stOutFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool bWrote = (stOutFile.write(aDst) == aDst.size());
    stOutFile.close();
    return bWrote;
}

// 2026.08.24 blob → 内存解压（「查看变更」用：不落临时文件，直接回给调用方 QByteArray）。
bool ZstdDecompressBytes(const QByteArray& a_sSrc, QByteArray& a_dstOut) {
    const unsigned long long uSize = ZSTD_getFrameContentSize(
        a_sSrc.constData(), static_cast<size_t>(a_sSrc.size()));
    if (uSize == ZSTD_CONTENTSIZE_ERROR || uSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        uSize > static_cast<unsigned long long>(INT_MAX)) {
        return false;
    }
    a_dstOut.resize(static_cast<int>(uSize));
    const size_t stOut = ZSTD_decompress(a_dstOut.data(), static_cast<size_t>(uSize),
                                         a_sSrc.constData(), static_cast<size_t>(a_sSrc.size()));
    if (ZSTD_isError(stOut) || stOut != static_cast<size_t>(uSize)) {
        a_dstOut.clear();
        return false;
    }
    return true;
}

// 递归压缩镜像：目录结构原样保留，每个文件压缩为 <file>.zst。排除清单仅顶层一级生效（同 MirrorTree）。
bool CompressTree(const QString& a_sSrc, const QString& a_sDst, const QStringList& a_aExclude) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    const QStringList aDirs = stSrc.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString& sSub : aDirs) {
        if (a_aExclude.contains(sSub, Qt::CaseInsensitive)) {
            continue;
        }
        if (!CompressTree(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub), a_aExclude)) {
            return false;
        }
    }
    const QStringList aFiles = stSrc.entryList(QDir::Files | QDir::NoSymLinks);
    for (const QString& sFile : aFiles) {
        const QString sSrcPath = stSrc.filePath(sFile);
        const QString sDstPath = QDir(a_sDst).filePath(sFile + QStringLiteral(".zst"));
        if (!ZstdCompressFile(sSrcPath, sDstPath)) {
            return false;
        }
    }
    return true;
}

// 从备份版本目录恢复源码树到工程根：.zst 文件解压回原名，其余（旧版未压缩）直接拷贝。
// 兼容新（压缩）与历史（未压缩）备份，无需额外格式标记。
bool RestoreTreeLegacy(const QString& a_sSrc, const QString& a_sDst) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    const QStringList aDirs = stSrc.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString& sSub : aDirs) {
        if (!RestoreTreeLegacy(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub))) {
            return false;
        }
    }
    const QString sZst = QStringLiteral(".zst");
    const QStringList aFiles = stSrc.entryList(QDir::Files | QDir::NoSymLinks);
    for (const QString& sFile : aFiles) {
        const QString sSrcPath = stSrc.filePath(sFile);
        const bool bCompressed = sFile.endsWith(sZst, Qt::CaseInsensitive);
        const QString sDstName = bCompressed ? sFile.left(sFile.size() - sZst.size()) : sFile;
        const QString sDstPath = QDir(a_sDst).filePath(sDstName);
        if (QFileInfo::exists(sDstPath) && !QFile::remove(sDstPath)) {
            return false;
        }
        if (bCompressed) {
            if (!ZstdDecompressFile(sSrcPath, sDstPath)) {
                return false;
            }
        } else if (!QFile::copy(sSrcPath, sDstPath)) {
            return false;   // 旧版（未压缩）备份文件直接拷回
        }
    }
    return true;
}
}   // namespace

QString CProjectManager::LoadBackupExcludeDirs() const {
    // 读 [Backup] ExcludeDirs 原文（逗号分隔）；无 config.ini/缺键 → 空串（回退内置默认）。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return QString();
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[1024] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Backup", L"ExcludeDirs", L"", szBuf, 1024, wsPath.c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
}

bool CProjectManager::SaveBackupExcludeDirs(const QString& a_sDirs) {
    // 落 [Backup] ExcludeDirs（空串=清空回退内置默认）；失败不抛。
    const QString sPath = SettingsCfgPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Backup", L"ExcludeDirs",
        a_sDirs.toStdWString().c_str(), wsPath.c_str());
    return true;
}

// —— 2026.09.23 资源管理器显示隐藏目录（[Explorer] HiddenDirs）——

QString CProjectManager::DefaultExplorerHiddenDirs() {
    // 内置默认：常见生成目录不进树（单点维护；面板构造/设置页预填同源）。
    return QStringLiteral("build,.git,.vs,node_modules");
}

bool CProjectManager::SaveExplorerHiddenDirs(const QString& a_sDirs) {
    // 落 [Explorer] HiddenDirs（保存的是全量清单——设置页预填生效清单后整写；
    //   空串=写空值→全可见。与缺键（回退内置默认）区分）。失败不抛。
    //   注意用全局 ConfigFilePath 而非 SettingsCfgPath——显示偏好属界面态，
    //   不参与任务级覆盖（同 Recent/UI/Layout 口径）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Explorer", L"HiddenDirs",
        a_sDirs.toStdWString().c_str(), wsPath.c_str());
    return true;
}

QString CProjectManager::LoadExplorerHiddenDirs() const {
    // 读 [Explorer] HiddenDirs：缺键 → 内置默认（GetPrivateProfileStringW 缺键返回
    //   lpDefault、键存在值为空返回空串——两种情形天然区分）；有值 → 显式清单原文
    //   （设置页预填它，用户删哪项哪项可见）。无 config.ini 同缺键。
    //   读全局 ConfigFilePath（界面态不随任务覆盖，见 Save 注）。
    const QString sPath = ConfigFilePath();
    const std::wstring wsDefault = DefaultExplorerHiddenDirs().toStdWString();
    if (sPath.isEmpty()) {
        return DefaultExplorerHiddenDirs();
    }
    const std::wstring wsPath = sPath.toStdWString();
    wchar_t szBuf[1024] = { 0 };
    const DWORD dwCopied = GetPrivateProfileStringW(
        L"Explorer", L"HiddenDirs", wsDefault.c_str(), szBuf, 1024, wsPath.c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dwCopied)).trimmed();
}

QStringList CProjectManager::BackupExcludeDirs() const {
    // FR-6（Q4 默认排除清单）：.autodev（含 backups 自身，防递归）、build、Execute、
    // .git、.vs、x64。2026.08.15 改读 config.ini [Backup] ExcludeDirs（逗号分隔，
    //   空项跳过）；无 config.ini/缺键/全空 → 回退内置默认。每次调用现读（设置保存即生效）。
    const QStringList aItems = LoadBackupExcludeDirs()
        .split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList aTrimmed;
    for (const QString& sItem : aItems) {
        const QString sTrim = sItem.trimmed();
        if (!sTrim.isEmpty()) {
            aTrimmed.append(sTrim);
        }
    }
    if (!aTrimmed.isEmpty()) {
        return aTrimmed;
    }
    // 2026.08.29 默认清单补依赖/缓存目录（修「窗口关闭后进程僵死」根因之一）：
    //   pnpm/npm 单仓的 node_modules 动辄数万~数十万代码类文件（js/ts/json 命中
    //   文件白名单），首轮 turn 快照读+SHA256 可达几十分钟——备份线程退出时被
    //   无超时 join，窗口已关进程僵死（实测 DeepSeekHarness 5.6 万文件 25 分钟
    //   未扫完、实例内存 2.9GB）。__pycache__/.pnpm-store 同理。
    return { QStringLiteral(".autodev"), QStringLiteral("build"),
             QStringLiteral("Execute"), QStringLiteral(".git"),
             QStringLiteral(".vs"), QStringLiteral("x64"),
             QStringLiteral("node_modules"), QStringLiteral(".pnpm-store"),
             QStringLiteral("__pycache__") };
}

QString CProjectManager::BackupsDir(const QString& a_sTaskId) const {
    return QDir::cleanPath(TaskDir(a_sTaskId) + QStringLiteral("/backups"));
}

int CProjectManager::BackupSources(const QString& a_sTaskId, const QString& a_sStage) {
    // 工程根未就绪 / 任务 id 空 → 直接失败（0）。
    if (m_sProjectDir.isEmpty() || a_sTaskId.isEmpty()) {
        return 0;
    }
    const QString sRoot = m_sProjectDir;
    if (!QDir(sRoot).exists()) {
        return 0;
    }
    const QString sBackups = BackupsDir(a_sTaskId);
    QDir().mkpath(sBackups);

    // 扫已有版本目录取 max+1（序号单调递增）。目录名形如 0001、0002。
    int iMaxSeq = 0;
    const QStringList aSubDirs = QDir(sBackups).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aSubDirs) {
        if (sSub == QStringLiteral("_restore_tmp")) {
            continue;   // 恢复临时目录不计入版本序号
        }
        bool bOk = false;
        const int iSeq = sSub.toInt(&bOk);
        if (bOk && iSeq > iMaxSeq) {
            iMaxSeq = iSeq;
        }
    }
    const int iSeq = iMaxSeq + 1;
    const QString sSeqStr = FormatSeq(iSeq);
    const QString sVerDir = QDir(sBackups).filePath(sSeqStr);

    // 全量镜像工程源码（排除清单），逐文件 zstd 压缩为 .zst（#2，减小备份体积）；失败回滚。
    if (!CompressTree(sRoot, sVerDir, BackupExcludeDirs())) {
        RemoveTree(sVerDir);   // 清理半成品，避免 ListBackups 列出残缺版本
        return 0;
    }

    // 写版本元数据 .meta.json：{seq,time,stage}；首次（无 summary）不写 summary 字段（FR-6.3）。
    QJsonObject oMeta;
    oMeta[QStringLiteral("seq")] = iSeq;
    oMeta[QStringLiteral("time")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    oMeta[QStringLiteral("stage")] = a_sStage;
    // summary 留空：阶段结束时由 SetBackupSummary 回填（首次版本无上一版可对比）。
    const QString sMetaPath = QDir(sBackups).filePath(sSeqStr + QStringLiteral(".meta.json"));
    QJsonDocument stDoc(oMeta);
    QFile stMetaFile(sMetaPath);
    if (stMetaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        stMetaFile.write("\xEF\xBB\xBF");   // UTF-8 BOM（与工程其它落盘文件一致）
        stMetaFile.write(stDoc.toJson());
        stMetaFile.close();
    }
    return iSeq;
}

bool CProjectManager::SetBackupSummary(const QString& a_sTaskId, int a_iSeq,
                                       const QString& a_sSummary) {
    // 回填某版本的 summary：读现有 meta → 补 summary 字段 → 覆盖写回。
    if (a_sTaskId.isEmpty() || a_iSeq <= 0) {
        return false;
    }
    const QString sMetaPath = QDir(BackupsDir(a_sTaskId))
        .filePath(FormatSeq(a_iSeq) + QStringLiteral(".meta.json"));
    CFrwDevice stRead(sMetaPath);
    if (!stRead.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray bufData = stRead.readAll();
    stRead.close();
    // 跳过可能存在的 UTF-8 BOM。
    if (bufData.startsWith("\xEF\xBB\xBF")) {
        bufData = bufData.mid(3);
    }
    QJsonParseError stErr;
    QJsonDocument stDoc = QJsonDocument::fromJson(bufData, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        return false;
    }
    QJsonObject oMeta = stDoc.object();
    oMeta[QStringLiteral("summary")] = a_sSummary;

    QFile stWrite(sMetaPath);
    if (!stWrite.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stWrite.write("\xEF\xBB\xBF");
    stWrite.write(QJsonDocument(oMeta).toJson());
    stWrite.close();
    return true;
}

QList<SBackupVersion> CProjectManager::ListBackups(const QString& a_sTaskId) const {
    QList<SBackupVersion> aVersions;
    if (a_sTaskId.isEmpty()) {
        return aVersions;
    }
    const QDir stBackups(BackupsDir(a_sTaskId));
    if (!stBackups.exists()) {
        return aVersions;
    }
    // 扫所有 NNNN.meta.json，解析为 SBackupVersion，按序号倒序（最新在前）。
    const QStringList aMetas = stBackups.entryList({QStringLiteral("*.meta.json")}, QDir::Files);
    for (const QString& sMeta : aMetas) {
        CFrwDevice stFile(stBackups.filePath(sMeta));
        if (!stFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        QByteArray bufData = stFile.readAll();
        stFile.close();
        if (bufData.startsWith("\xEF\xBB\xBF")) {
            bufData = bufData.mid(3);
        }
        QJsonParseError stErr;
        const QJsonDocument stDoc = QJsonDocument::fromJson(bufData, &stErr);
        if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
            continue;
        }
        const QJsonObject o = stDoc.object();
        SBackupVersion st;
        st.iSeq    = o.value(QStringLiteral("seq")).toInt();
        st.sTime   = o.value(QStringLiteral("time")).toString();
        st.sStage  = o.value(QStringLiteral("stage")).toString();
        st.sSummary = o.value(QStringLiteral("summary")).toString();   // 首次版本无此字段→空
        if (st.iSeq > 0) {
            aVersions.append(st);
        }
    }
    // 倒序：序号大的在前。
    std::sort(aVersions.begin(), aVersions.end(),
              [](const SBackupVersion& a, const SBackupVersion& b) { return a.iSeq > b.iSeq; });
    return aVersions;
}

bool CProjectManager::RestoreSources(const QString& a_sTaskId, int a_iSeq, QString& a_sErr) {
    // 原子恢复（R3/NFR-5 高危）：
    //   1) 把工程当前源码（排除清单）移到 backups/_restore_tmp/（中转）
    //   2) 从目标版本目录拷贝全部源码到工程根
    //   3) 全部成功 → 删 _restore_tmp；任一步失败 → 删工程根半恢复内容 + 把 _restore_tmp 拷回（回滚）
    a_sErr.clear();
    if (m_sProjectDir.isEmpty() || a_sTaskId.isEmpty() || a_iSeq <= 0) {
        a_sErr = QStringLiteral("参数无效（工程未打开 / 任务 id 空 / 序号非法）");
        return false;
    }
    const QString sRoot = m_sProjectDir;
    const QString sVerDir = QDir(BackupsDir(a_sTaskId)).filePath(FormatSeq(a_iSeq));
    if (!QDir(sVerDir).exists()) {
        a_sErr = QStringLiteral("目标备份版本不存在：%1").arg(a_iSeq);
        return false;
    }
    const QString sTmp = QDir(BackupsDir(a_sTaskId)).filePath(QStringLiteral("_restore_tmp"));
    // 清理可能的残留临时目录（上次恢复异常退出留下）。
    RemoveTree(sTmp);

    // 步骤 1：当前工程源码镜像到临时目录（之后清空工程源码区）。
    if (!MirrorTree(sRoot, sTmp, BackupExcludeDirs())) {
        RemoveTree(sTmp);
        a_sErr = QStringLiteral("备份当前源码到临时目录失败，已中止（工程未改动）");
        return false;
    }
    // 清空工程根下的当前源码（逐项删，跳过排除目录——.autodev 含 backups 与临时目录本身须保留）。
    const QStringList aExclude = BackupExcludeDirs();
    QDir stRoot(sRoot);
    for (const QString& sSub : stRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (aExclude.contains(sSub, Qt::CaseInsensitive)) {
            continue;   // .autodev / build / ... 保留
        }
        RemoveTree(stRoot.filePath(sSub));
    }
    for (const QString& sFile : stRoot.entryList(QDir::Files)) {
        QFile::remove(stRoot.filePath(sFile));
    }

    // 步骤 2：从目标备份版本恢复到工程根（zstd 解压 .zst；旧版未压缩文件直接拷回）。
    if (!RestoreTreeLegacy(sVerDir, sRoot)) {
        // 拷贝失败 → 回滚：先清空工程根半恢复内容，再把临时目录拷回。
        for (const QString& sSub : stRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (aExclude.contains(sSub, Qt::CaseInsensitive)) {
                continue;
            }
            RemoveTree(stRoot.filePath(sSub));
        }
        for (const QString& sFile : stRoot.entryList(QDir::Files)) {
            QFile::remove(stRoot.filePath(sFile));
        }
        MirrorTree(sTmp, sRoot, QStringList());   // 尽力回滚到恢复前状态
        RemoveTree(sTmp);
        a_sErr = QStringLiteral("从备份版本恢复源码失败，已回滚到恢复前状态");
        return false;
    }

    // 步骤 3：成功 → 清理临时目录。
    RemoveTree(sTmp);
    return true;
}

// ============================================================================
// ===== [PortableCore] CBackupStore —— 统一增量备份 CAS 存储核心（方案 v3.1 §4）=====
// ============================================================================

namespace {

// 读 JSON 文件（自动跳 UTF-8 BOM）；失败返回空文档。
QJsonDocument ReadJsonFile(const QString& a_sPath) {
    CFrwDevice stFile(a_sPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QJsonDocument();
    }
    QByteArray bufData = stFile.readAll();
    stFile.close();
    if (bufData.startsWith("\xEF\xBB\xBF")) {
        bufData = bufData.mid(3);
    }
    QJsonParseError stErr;
    return QJsonDocument::fromJson(bufData, &stErr);
}

// 写 JSON 文件（按 a_bBom 决定是否写 UTF-8 BOM）。
bool WriteJsonFile(const QString& a_sPath, const QJsonDocument& a_stDoc, bool a_bBom) {
    QFile stFile(a_sPath);
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    // 2026.09.28 写入校验：磁盘满/短写时残缺 JSON 会被调用方 rename 成正式档，
    //   「tmp+rename 原子提交」形同虚设（ZstdCompressFile 已有同款校验）。
    const QByteArray aBody = a_stDoc.toJson();
    const QByteArray aOut = a_bBom ? QByteArray("\xEF\xBB\xBF") + aBody : aBody;
    if (stFile.write(aOut) != aOut.size() || stFile.error() != QFile::NoError) {
        stFile.close();
        return false;
    }
    stFile.close();
    return true;
}

// 统一库内路径工具（文件内静态风格）。
QString BsStoreDir(const QString& a_sRoot) {
    return QDir(a_sRoot).filePath(QStringLiteral("store"));
}
QString BsMetaName(int a_iSeq) {
    return QString(QStringLiteral("%1.meta.json")).arg(a_iSeq, 4, 10, QLatin1Char('0'));
}
QString BsMetaPath(const QString& a_sRoot, int a_iSeq) {
    return QDir(a_sRoot).filePath(BsMetaName(a_iSeq));
}

// 2026.08.29 无差异快照「同源引用版」解析：meta 不带 files 而是 "base"=<seq>（指向
//   同 owner 的上一版本，链深 ≤1）；读侧经此函数解析出真实 manifest（链断/坏档 → 空）。
//   背景：每次提交提示词触发的 turn 快照在「工程无任何改动」时原设计直接跳过建版
//   （复用 lastSeq）——该条提示在回退列表里凭空消失（实测 $HOME/.claude 工程：
//   连续两条提示无记录点，用户以为修改丢失无法回退）。
QJsonObject BsResolveFiles(const QString& a_sRoot, int a_iSeq) {
    QJsonObject oMeta = ReadJsonFile(BsMetaPath(a_sRoot, a_iSeq)).object();
    for (int iHop = 0; iHop < 8 && oMeta.contains(QStringLiteral("base")); ++iHop) {
        const int iBase = oMeta.value(QStringLiteral("base")).toInt();
        if (iBase <= 0) {
            return {};
        }
        oMeta = ReadJsonFile(BsMetaPath(a_sRoot, iBase)).object();
    }
    return oMeta.value(QStringLiteral("files")).toObject();
}
QString BsTmpDir(const QString& a_sRoot) {
    return QDir(a_sRoot).filePath(QStringLiteral("_restore_tmp"));
}

}   // namespace

CBackupStore::CBackupStore(const QString& a_sBackupsRoot, const QString& a_sSourceRoot,
                           bool a_bBom)
    : m_sBackupsRoot(a_sBackupsRoot), m_sSourceRoot(a_sSourceRoot), m_bBom(a_bBom) {}

int CBackupStore::Backup(const QString& a_sKind, const QString& a_sOwner,
                         const QString& a_sStage, const QString& a_sPrompt, int a_iTarget) {
    if (m_sBackupsRoot.isEmpty() || m_sSourceRoot.isEmpty() || a_sKind.isEmpty()
        || a_sOwner.isEmpty() || !QDir(m_sSourceRoot).exists()) {
        return 0;
    }
    QDir().mkpath(BsStoreDir(m_sBackupsRoot));

    // 2026.08.26 哈希缓存（turn 快照提速）：path → {h,z,m}。stat 未变（size+mtime）
    //   的文件免读内容免 SHA256；全部未变时直接复用上次版本（不新建 meta，无差异快照无意义）。
    //   大工程（万级文件）首轮全量后，后续 turn 快照从数十秒降到秒级。
    const QString sCachePath = QDir(m_sBackupsRoot).filePath(QStringLiteral("hashcache.json"));
    QJsonObject oCache = ReadJsonFile(sCachePath).object();
    const qint64 iLastSeq = oCache.value(QStringLiteral("#lastSeq")).toVariant().toLongLong();
    bool bAnyChanged = false;

    // 1) 遍历源码根（谓词过滤）→ manifest；未变内容 blob 已存在即跳过（FR-1.2 去重）。
    //   2026.08.26（自 HarnessAgent 平移）：entryInfoList + NoSymLinks（防 pnpm 等的
    //   junction/symlink 死循环）+ Hidden（隐藏文件如 .gitignore 亦入库）；目录级
    //   剪枝改用 m_pfDirFilter（不套文件白名单，免误杀无扩展名目录整棵子树）。
    QStringList aFiles;
    std::function<void(const QString&, const QString&)> fnScan =
        [&](const QString& a_sAbsDir, const QString& a_sPrefix) {
            if (m_bAbort.load()) {
                return;   // 2026.08.29 析构/新快照请求中止：及时退出长扫描
            }
            const QDir stDir(a_sAbsDir);
            const QList<QFileInfo> aSubDirs = stDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
            for (const QFileInfo& stSub : aSubDirs) {
                const QString sRel = a_sPrefix.isEmpty()
                    ? stSub.fileName() : a_sPrefix + QLatin1Char('/') + stSub.fileName();
                if (m_pfDirFilter) {
                    if (!m_pfDirFilter(sRel))
                        continue;
                } else if (m_pfFilter && !m_pfFilter(sRel)) {
                    continue;
                }
                fnScan(stSub.absoluteFilePath(), sRel);
            }
            const QList<QFileInfo> aNames = stDir.entryInfoList(
                QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
            for (const QFileInfo& stFi : aNames) {
                const QString sRelFile = a_sPrefix.isEmpty()
                    ? stFi.fileName() : a_sPrefix + QLatin1Char('/') + stFi.fileName();
                // 2026.08.23 文件级白名单：只备份代码相关文件（中间文件/产物跳过，
                //   不入 manifest → 不产生 blob，历史版本里已存的旧产物 blob 由 GC 兜底回收）。
                if (m_pfFileFilter && !m_pfFileFilter(sRelFile)) {
                    continue;
                }
                aFiles.append(sRelFile);
            }
        };
    fnScan(m_sSourceRoot, QString());
    if (m_bAbort.load()) {
        return 0;   // 2026.08.29 扫描被请求中止（退出/新快照）：静默失败
    }

    QJsonObject oFiles;
    int iKept = 0;                     // 2026.08.26 实际入库（入 manifest/缓存）的文件数（无差异判定用）
    for (const QString& sRel : aFiles) {
        if (m_bAbort.load()) {
            return 0;   // 2026.08.29 哈希循环同样可中止（首轮全量读+SHA256 最耗时）
        }
        const QString sAbs = QDir(m_sSourceRoot).filePath(sRel);
        const QFileInfo stInfo(sAbs);
        // 2026.08.26（自 HarnessAgent 平移）：单文件大小上限，超限不入库（不报错）。
        if (m_iMaxFileBytes > 0 && stInfo.size() > m_iMaxFileBytes) {
            continue;
        }
        const qint64 iSizeMs = static_cast<qint64>(stInfo.size());
        const qint64 iMtimeMs = stInfo.lastModified().toMSecsSinceEpoch();
        QString sHash;
        // 2026.08.26 提速核心：stat 未变（size+mtime 同缓存）→ 免读内容免 SHA256。
        const QJsonObject oHit = oCache.value(sRel).toObject();
        if (!oHit.isEmpty()
            && oHit.value(QStringLiteral("z")).toVariant().toLongLong() == iSizeMs
            && oHit.value(QStringLiteral("m")).toVariant().toLongLong() == iMtimeMs) {
            sHash = oHit.value(QStringLiteral("h")).toString();
        } else {
            bAnyChanged = true;
            CFrwDevice stF(sAbs);
            if (!stF.open(QIODevice::ReadOnly)) {
                return 0;
            }
            const QByteArray bufData = stF.readAll();
            stF.close();
            sHash = QString::fromLatin1(
                QCryptographicHash::hash(bufData, QCryptographicHash::Sha256).toHex());
        }
        const QString sBlob = QDir(BsStoreDir(m_sBackupsRoot))
            .filePath(sHash + QStringLiteral(".zst"));
        // v3.1 blob 不可变落盘：先写唯一临时名再 rename（并发备份同一 hash 时，
        // 后到者 rename 失败但最终 blob 已存在 = 内容相同，视为成功；防 remove+重写交错损坏共享 blob）。
        if (!QFileInfo::exists(sBlob)) {
            const QString sBlobTmp = sBlob + QStringLiteral(".tmp%1")
                .arg(QDateTime::currentMSecsSinceEpoch()
                     ^ reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
            if (!ZstdCompressFile(sAbs, sBlobTmp)
                || (!QFile::rename(sBlobTmp, sBlob) && !QFileInfo::exists(sBlob))) {
                QFile::remove(sBlobTmp);
                return 0;   // 压缩/落盘失败整版作废（残缺 tmp 无害，GC 兜底前不影响读取）
            }
            QFile::remove(sBlobTmp);   // rename 失败但 blob 已被并发对手落成 → 清自己的 tmp
        }
        QJsonObject oEnt;
        oEnt[QStringLiteral("h")] = sHash;
        oEnt[QStringLiteral("z")] = iSizeMs;
        oEnt[QStringLiteral("m")] = iMtimeMs;
        oFiles[sRel] = oEnt;
        oCache[sRel] = oEnt;   // 2026.08.26 缓存项与 manifest 同构（h/z/m）
        ++iKept;
    }

    // 2026.08.26→2026.08.29 全部未变（文件集与各文件 stat 均与上次快照一致）：
    //   原设计直接跳过建版（复用 lastSeq）——该条提示在回退列表里凭空消失（实测
    //   $HOME/.claude 工程连续两条提示无记录点，用户以为修改丢失）。改为写「同源
    //   引用版」：meta 仅数百字节（"base" 指向同 owner 的上一实版，Read 侧
    //   BsResolveFiles 解析），列表/回退/对比全链路透明；**仅同 owner 引用**——
    //   跨 owner（如任务 stage 版）不引用（PurgeOwner 删基版会把引用版变孤儿），
    //   退回旧口径。写失败同旧口径（回写缓存返回 lastSeq）。
    if (!bAnyChanged && iLastSeq > 0 && oCache.size() == iKept + 1) {
        int iBaseSeq = 0;
        const QJsonObject oLast = ReadJsonFile(BsMetaPath(m_sBackupsRoot, static_cast<int>(iLastSeq))).object();
        if (!oLast.isEmpty()
            && oLast.value(QStringLiteral("owner")).toString() == a_sOwner) {
            iBaseSeq = oLast.contains(QStringLiteral("base"))
                ? oLast.value(QStringLiteral("base")).toInt()
                : static_cast<int>(iLastSeq);
        }
        if (iBaseSeq > 0) {
            for (int iAttempt = 0; iAttempt < 3; ++iAttempt) {
                int iMaxSeq = 0;
                const QStringList aMetas = QDir(m_sBackupsRoot)
                    .entryList({QStringLiteral("*.meta.json")}, QDir::Files);
                for (const QString& sMeta : aMetas) {
                    bool bOk = false;
                    const int iDot = sMeta.indexOf(QLatin1Char('.'));
                    const int iSeq = (iDot > 0 ? sMeta.left(iDot) : sMeta).toInt(&bOk);
                    if (bOk && iSeq > iMaxSeq) {
                        iMaxSeq = iSeq;
                    }
                }
                const int iSeq = iMaxSeq + 1;
                QJsonObject oAlias;
                oAlias[QStringLiteral("seq")]    = iSeq;
                oAlias[QStringLiteral("time")]   = QDateTime::currentDateTime().toString(Qt::ISODate);
                oAlias[QStringLiteral("kind")]   = a_sKind;
                oAlias[QStringLiteral("owner")]  = a_sOwner;
                if (!a_sStage.isEmpty())  oAlias[QStringLiteral("stage")]  = a_sStage;
                if (!a_sPrompt.isEmpty()) oAlias[QStringLiteral("prompt")] = a_sPrompt.left(2000);
                if (a_iTarget > 0)        oAlias[QStringLiteral("target")] = a_iTarget;
                oAlias[QStringLiteral("format")] = QStringLiteral("cas");
                oAlias[QStringLiteral("base")]   = iBaseSeq;
                const QString sFinal = BsMetaPath(m_sBackupsRoot, iSeq);
                const QString sTmpF  = sFinal + QStringLiteral(".tmp");
                if (WriteJsonFile(sTmpF, QJsonDocument(oAlias), m_bBom)
                    && !QFile::exists(sFinal)
                    && QFile::rename(sTmpF, sFinal)) {
                    oCache[QStringLiteral("#lastSeq")] = static_cast<qint64>(iSeq);
                    WriteJsonFile(sCachePath + QStringLiteral(".tmp"), QJsonDocument(oCache), m_bBom);
                    QFile::remove(sCachePath);
                    QFile::rename(sCachePath + QStringLiteral(".tmp"), sCachePath);
                    return iSeq;
                }
                QFile::remove(sTmpF);   // 序号被并发占用/写失败 → 重试或退旧口径
            }
        }
        WriteJsonFile(sCachePath + QStringLiteral(".tmp"), QJsonDocument(oCache), m_bBom);
        QFile::remove(sCachePath);
        QFile::rename(sCachePath + QStringLiteral(".tmp"), sCachePath);
        return static_cast<int>(iLastSeq);
    }

    // 2) meta 原子提交：扫 max+1 → 临时文件 + rename；rename 失败=序号被并发占用 → 重试 ≤3。
    for (int iAttempt = 0; iAttempt < 3; ++iAttempt) {
        int iMaxSeq = 0;
        const QStringList aMetas = QDir(m_sBackupsRoot)
            .entryList({QStringLiteral("*.meta.json")}, QDir::Files);
        for (const QString& sMeta : aMetas) {
            bool bOk = false;
            const int iDot = sMeta.indexOf(QLatin1Char('.'));
            const int iSeq = (iDot > 0 ? sMeta.left(iDot) : sMeta).toInt(&bOk);   // 整段数字前缀（>9999 不断档）
            if (bOk && iSeq > iMaxSeq) {
                iMaxSeq = iSeq;
            }
        }
        const int iSeq = iMaxSeq + 1;
        QJsonObject oMeta;
        oMeta[QStringLiteral("seq")]    = iSeq;
        oMeta[QStringLiteral("time")]   = QDateTime::currentDateTime().toString(Qt::ISODate);
        oMeta[QStringLiteral("kind")]   = a_sKind;
        oMeta[QStringLiteral("owner")]  = a_sOwner;
        if (!a_sStage.isEmpty())  oMeta[QStringLiteral("stage")]  = a_sStage;
        if (!a_sPrompt.isEmpty()) oMeta[QStringLiteral("prompt")] = a_sPrompt.left(2000);
        if (a_iTarget > 0)        oMeta[QStringLiteral("target")] = a_iTarget;
        oMeta[QStringLiteral("format")] = QStringLiteral("cas");
        oMeta[QStringLiteral("files")]  = oFiles;
        const QString sFinal = BsMetaPath(m_sBackupsRoot, iSeq);
        const QString sTmpF  = sFinal + QStringLiteral(".tmp");
        if (!WriteJsonFile(sTmpF, QJsonDocument(oMeta), m_bBom)) {
            return 0;
        }
        if (QFile::exists(sFinal)) {
            QFile::remove(sTmpF);   // 重试窗口内被抢：清临时文件再重扫
            continue;
        }
        if (QFile::rename(sTmpF, sFinal)) {
            // 2026.08.26 哈希缓存回写：记录本次版本号，裁剪掉已不在备份范围（被删/
            //   被排除）的旧条目。临时文件+rename 原子替换；并发备份下后写胜出（条目均为
            //   有效指纹，互相覆盖无损坏）。写失败不影响备份结果（下次冷启动重算）。
            oCache = oFiles;                     // 裁剪：仅保留本次在范围内的条目
            oCache[QStringLiteral("#lastSeq")] = static_cast<qint64>(iSeq);
            const QString sCacheTmp = sCachePath + QStringLiteral(".tmp");
            if (WriteJsonFile(sCacheTmp, QJsonDocument(oCache), m_bBom)) {
                QFile::remove(sCachePath);
                QFile::rename(sCacheTmp, sCachePath);
            }
            return iSeq;
        }
        QFile::remove(sTmpF);
    }
    return 0;
}

bool CBackupStore::Restore(int a_iSeq, QString& a_sErr) {
    a_sErr.clear();
    const QString sMetaPath = BsMetaPath(m_sBackupsRoot, a_iSeq);
    const QJsonDocument stDoc = ReadJsonFile(sMetaPath);
    if (!stDoc.isObject()) {
        a_sErr = QStringLiteral("版本 #%1 不存在或元数据损坏").arg(a_iSeq);
        return false;
    }
    const QJsonObject oMeta = stDoc.object();
    QJsonObject oFiles;
    if (oMeta.contains(QStringLiteral("files"))) {
        oFiles = oMeta.value(QStringLiteral("files")).toObject();
    } else if (oMeta.contains(QStringLiteral("base"))) {
        // 2026.08.29 同源引用版：files 取基版。
        oFiles = BsResolveFiles(m_sBackupsRoot, a_iSeq);
    }
    if (oFiles.isEmpty()) {
        a_sErr = QStringLiteral("版本 #%1 为旧格式或引用链损坏（无 manifest）").arg(a_iSeq);
        return false;
    }

    const QString sTmp = BsTmpDir(m_sBackupsRoot);
    RemoveTree(sTmp);   // 清理上次残留（上层残留场景已先行处理）
    if (!QDir().mkpath(sTmp)) {
        a_sErr = QStringLiteral("创建恢复临时目录失败");
        return false;
    }
    const QString sRoot = m_sSourceRoot;
    const QDir stRoot(sRoot);

    // ① 当前源码（谓词范围内顶层项）移入 tmp（rename，快；失败退 copy+remove，
    //    任一项无法安全移入 → 中止恢复（工程未动），砰低 mv3 安全承诺）。
    auto fnMoveToTmp = [&](const QString& a_sName) -> bool {
        const QString sSrc = stRoot.filePath(a_sName);
        const QString sDst = QDir(sTmp).filePath(a_sName);
        if (QDir().rename(sSrc, sDst)) {
            return true;
        }
        // rename 失败（句柄被编辑器/索引器/杀毒占用等）→ 拷贝+删除兜底。
        if (QFileInfo(sSrc).isDir()) {
            return MirrorTree(sSrc, sDst, QStringList()) && RemoveTree(sSrc);
        }
        return QFile::copy(sSrc, sDst) && QFile::remove(sSrc);
    };
    // 步骤①失败时的现场复原（把已移入 tmp 的项放回工程根；纯增量操作，不删任何现有内容）。
    auto fnUndoMove = [&]() {
        const QDir stTmpDir(sTmp);
        for (const QString& sName : stTmpDir.entryList(
                 QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
            const QString sSrc = stTmpDir.filePath(sName);
            if (QDir().rename(sSrc, stRoot.filePath(sName))) {
                continue;
            }
            // rename 回不去 → 拷回（保留 tmp 副本，HasRestoreTmp 残留路径可重试）。
            if (QFileInfo(sSrc).isDir()) {
                MirrorTree(sSrc, stRoot.filePath(sName), QStringList());
            } else {
                QFile::copy(sSrc, stRoot.filePath(sName));
            }
        }
        RemoveTree(sTmp);
    };
    for (const QString& sSub : stRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (m_pfFilter && !m_pfFilter(sSub)) {
            continue;
        }
        if (!fnMoveToTmp(sSub)) {
            fnUndoMove();
            a_sErr = QStringLiteral("无法安全移入临时目录（破占用）：%1（工程未改动）").arg(sSub);
            return false;
        }
    }
    for (const QString& sFile : stRoot.entryList(QDir::Files)) {
        // 2026.08.23 非代码文件（文件级谓词拒绝）不入恢复现场——留在原地不动
        //   （不进 tmp → 不被删除也不被恢复；与备份侧「不备份」对称）。
        if (m_pfFileFilter && !m_pfFileFilter(sFile)) {
            continue;
        }
        if (!fnMoveToTmp(sFile)) {
            fnUndoMove();
            a_sErr = QStringLiteral("无法安全移入临时目录（破占用）：%1（工程未改动）").arg(sFile);
            return false;
        }
    }
    // 回滚辅助：清空工程根（谓词范围内）+ tmp 拷回（尽力还原现场）。
    auto fnRollback = [&]() {
        for (const QString& sSub : stRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (m_pfFilter && !m_pfFilter(sSub)) {
                continue;
            }
            RemoveTree(stRoot.filePath(sSub));
        }
        for (const QString& sFile : stRoot.entryList(QDir::Files)) {
            // 2026.09.28 补文件级谓词（与步骤①同口径）：非代码根级文件（.sln/.vcxproj/
            //   图片等）从未入 tmp 也从不在备份 manifest 里，旧实现回滚时无差别删光，
            //   造成不可逆数据丢失；它们留在原地不动即可（与①的「不迁移」对称）。
            if (m_pfFileFilter && !m_pfFileFilter(sFile)) {
                continue;
            }
            QFile::remove(stRoot.filePath(sFile));
        }
        MirrorTree(sTmp, sRoot, QStringList());
        RemoveTree(sTmp);
    };

    // ② 逐条解压 + sha256 复核（防线 3：坏 blob 硬失败且工程已还原）。
    for (auto it = oFiles.begin(); it != oFiles.end(); ++it) {
        const QString sRel  = it.key();
        const QString sHash = it.value().toObject().value(QStringLiteral("h")).toString();
        const QString sBlob = QDir(BsStoreDir(m_sBackupsRoot))
            .filePath(sHash + QStringLiteral(".zst"));
        const QString sDst  = QDir(sRoot).filePath(sRel);
        QDir().mkpath(QFileInfo(sDst).absolutePath());
        if (QFileInfo::exists(sDst) && !QFile::remove(sDst)) {
            fnRollback();
            a_sErr = QStringLiteral("目标文件无法覆盖：%1（工程已还原）").arg(sRel);
            return false;
        }
        if (!ZstdDecompressFile(sBlob, sDst)) {
            fnRollback();
            a_sErr = QStringLiteral("store/%1.zst 损坏（版本 #%2 不可恢复，工程已还原）")
                .arg(sHash.left(12)).arg(a_iSeq);
            return false;
        }
        CFrwDevice stCheck(sDst);
        if (!stCheck.open(QIODevice::ReadOnly)) {
            fnRollback();
            a_sErr = QStringLiteral("恢复后校验读取失败：%1（工程已还原）").arg(sRel);
            return false;
        }
        const QByteArray bufCheck = stCheck.readAll();
        stCheck.close();
        const QString sHash2 = QString::fromLatin1(
            QCryptographicHash::hash(bufCheck, QCryptographicHash::Sha256).toHex());
        if (sHash2 != sHash) {
            QFile::remove(sDst);
            fnRollback();
            a_sErr = QStringLiteral("store/%1.zst 内容校验失败（版本 #%2 不可恢复，工程已还原）")
                .arg(sHash.left(12)).arg(a_iSeq);
            return false;
        }
    }
    // ③ 全部成功 → 非代码文件先从 tmp 拷回原地（备份侧未入 manifest，不能随 tmp 一起丢弃），
    //   再删 tmp。manifest 内路径跳过（历史全量版本可能含非代码文件，以恢复点内容为准）。
    {
        QSet<QString> aManifest;
        for (auto it = oFiles.begin(); it != oFiles.end(); ++it) {
            aManifest.insert(it.key());
        }
        QDirIterator itTmp(sTmp, QDir::Files,
                           QDirIterator::Subdirectories);
        while (itTmp.hasNext()) {
            const QString sAbs = itTmp.next();
            const QString sRelInTmp = QDir(sTmp).relativeFilePath(sAbs);
            if (aManifest.contains(sRelInTmp)
                || !m_pfFileFilter || m_pfFileFilter(sRelInTmp)) {
                continue;   // 已按 manifest 恢复 / 代码文件（不在 manifest = 恢复点不存在，正确删除）；
                            //   只拷回「白名单外」的非代码文件（备份侧没入 manifest，不能丢）
            }
            const QString sDst = QDir(sRoot).filePath(sRelInTmp);
            QDir().mkpath(QFileInfo(sDst).absolutePath());
            QFile::copy(sAbs, sDst);
        }
    }
    RemoveTree(sTmp);
    return true;
}

bool CBackupStore::RestoreFromTmp(QString& a_sErr) {
    a_sErr.clear();
    const QString sTmp = BsTmpDir(m_sBackupsRoot);
    if (!QDir(sTmp).exists()) {
        return true;
    }
    if (!MirrorTree(sTmp, m_sSourceRoot, QStringList())) {
        a_sErr = QStringLiteral("从恢复临时目录还原现场失败（临时目录保留，可重试）");
        return false;
    }
    RemoveTree(sTmp);
    return true;
}
QList<SBackupVersion> CBackupStore::List(const QString& a_sOwner) const {
    QList<SBackupVersion> aVersions;
    const QDir stRoot(m_sBackupsRoot);
    if (!stRoot.exists()) {
        return aVersions;
    }
    const QStringList aMetas = stRoot.entryList({QStringLiteral("*.meta.json")}, QDir::Files);
    for (const QString& sMeta : aMetas) {
        const QJsonDocument stDoc = ReadJsonFile(stRoot.filePath(sMeta));
        if (!stDoc.isObject()) {
            continue;
        }
        const QJsonObject o = stDoc.object();
        if (!a_sOwner.isEmpty() && o.value(QStringLiteral("owner")).toString() != a_sOwner) {
            continue;
        }
        SBackupVersion st;
        st.iSeq     = o.value(QStringLiteral("seq")).toInt();
        st.sTime    = o.value(QStringLiteral("time")).toString();
        st.sStage   = o.value(QStringLiteral("stage")).toString();
        st.sSummary = o.value(QStringLiteral("summary")).toString();
        st.sKind    = o.value(QStringLiteral("kind")).toString();
        st.sOwner   = o.value(QStringLiteral("owner")).toString();
        st.sPrompt  = o.value(QStringLiteral("prompt")).toString();
        st.iTarget  = o.value(QStringLiteral("target")).toInt();
        // 2026.08.29 同源引用版：无 files 有 base → 数量取基版（解析失败按 0/非旧格式）。
        if (o.contains(QStringLiteral("files"))) {
            st.iFileCount = o.value(QStringLiteral("files")).toObject().size();
            st.bLegacy = false;
        } else if (o.contains(QStringLiteral("base"))) {
            st.iFileCount = BsResolveFiles(m_sBackupsRoot,
                                           o.value(QStringLiteral("base")).toInt()).size();
            st.bLegacy = false;
        } else {
            st.iFileCount = 0;
            st.bLegacy = true;
        }
        if (st.iSeq > 0) {
            aVersions.append(st);
        }
    }
    std::sort(aVersions.begin(), aVersions.end(),
              [](const SBackupVersion& a, const SBackupVersion& b) { return a.sTime > b.sTime; });
    return aVersions;
}

bool CBackupStore::Diff(int a_iSeqOld, int a_iSeqNew, SDiffResult& a_stOut) const {
    a_stOut = SDiffResult();
    // 2026.08.29 同源引用版参与对比：经 base 解析（引用版 vs 基版 → 空差异，语义正确）。
    const QJsonObject oOld = BsResolveFiles(m_sBackupsRoot, a_iSeqOld);
    const QJsonObject oNew = BsResolveFiles(m_sBackupsRoot, a_iSeqNew);
    if (oOld.isEmpty() && oNew.isEmpty()) {
        return false;   // 版本不存在或旧格式 → 界面占位
    }
    for (auto it = oNew.begin(); it != oNew.end(); ++it) {
        const QJsonObject oN = it.value().toObject();
        const qint64 iZN = oN.value(QStringLiteral("z")).toVariant().toLongLong();
        if (!oOld.contains(it.key())) {
            ++a_stOut.iAdded;
            SDiffEntry st;
            st.sPath = it.key(); st.sChange = QStringLiteral("add");
            st.iSizeNew = iZN;
            a_stOut.aEntries.append(st);
        } else if (oOld.value(it.key()).toObject().value(QStringLiteral("h")).toString()
                   != oN.value(QStringLiteral("h")).toString()) {
            ++a_stOut.iChanged;
            SDiffEntry st;
            st.sPath = it.key(); st.sChange = QStringLiteral("mod");
            st.iSizeOld = oOld.value(it.key()).toObject()
                .value(QStringLiteral("z")).toVariant().toLongLong();
            st.iSizeNew = iZN;
            a_stOut.aEntries.append(st);
        }
    }
    for (auto it = oOld.begin(); it != oOld.end(); ++it) {
        if (!oNew.contains(it.key())) {
            ++a_stOut.iRemoved;
            SDiffEntry st;
            st.sPath = it.key(); st.sChange = QStringLiteral("del");
            st.iSizeOld = it.value().toObject()
                .value(QStringLiteral("z")).toVariant().toLongLong();
            a_stOut.aEntries.append(st);
        }
    }
    return true;
}

// 2026.08.24 回退窗口「查看变更」：读指定版本 manifest 中某文件的 blob 并解压到内存。
bool CBackupStore::ReadFile(int a_iSeq, const QString& a_sRelPath,
                            QByteArray& a_bufOut, QString& a_sErr) const {
    a_bufOut.clear();
    a_sErr.clear();
    const QJsonObject oMeta = ReadJsonFile(BsMetaPath(m_sBackupsRoot, a_iSeq)).object();
    if (oMeta.isEmpty()) {
        a_sErr = QStringLiteral("版本 #%1 不存在或元数据损坏").arg(a_iSeq);
        return false;
    }
    QJsonObject oFiles;
    if (oMeta.contains(QStringLiteral("files"))) {
        oFiles = oMeta.value(QStringLiteral("files")).toObject();
    } else if (oMeta.contains(QStringLiteral("base"))) {
        oFiles = BsResolveFiles(m_sBackupsRoot, a_iSeq);   // 2026.08.29 引用版解析
    }
    if (oFiles.isEmpty()) {
        a_sErr = QStringLiteral("版本 #%1 为旧格式或引用链损坏（无 manifest）").arg(a_iSeq);
        return false;
    }
    if (!oFiles.contains(a_sRelPath)) {
        a_sErr = QStringLiteral("文件「%1」不在版本 #%2 中").arg(a_sRelPath).arg(a_iSeq);
        return false;
    }
    const QString sHash = oFiles.value(a_sRelPath).toObject()
        .value(QStringLiteral("h")).toString();
    const QString sBlob = QDir(BsStoreDir(m_sBackupsRoot))
        .filePath(sHash + QStringLiteral(".zst"));
    CFrwDevice stBlob(sBlob);
    if (!stBlob.open(QIODevice::ReadOnly)) {
        a_sErr = QStringLiteral("blob 丢失或无法读取（%1）").arg(sHash.left(12));
        return false;
    }
    const QByteArray aZst = stBlob.readAll();
    stBlob.close();
    if (!ZstdDecompressBytes(aZst, a_bufOut)) {
        a_sErr = QStringLiteral("blob 解压失败（%1）").arg(sHash.left(12));
        return false;
    }
    return true;
}

int CBackupStore::PurgeOwner(const QString& a_sOwner) {
    if (a_sOwner.isEmpty()) {
        return 0;
    }
    const QDir stRoot(m_sBackupsRoot);
    const QStringList aMetas = stRoot.entryList({QStringLiteral("*.meta.json")}, QDir::Files);
    int iDeleted = 0;
    for (const QString& sMeta : aMetas) {
        const QJsonObject o = ReadJsonFile(stRoot.filePath(sMeta)).object();
        if (o.value(QStringLiteral("owner")).toString() == a_sOwner) {
            if (QFile::remove(stRoot.filePath(sMeta))) {
                ++iDeleted;
            }
        }
    }
    return iDeleted ? GcOrphanBlobs() : 0;
}

int CBackupStore::GcOrphanBlobs() {
    const QDir stRoot(m_sBackupsRoot);
    if (!stRoot.exists()) {
        return 0;
    }
    QSet<QString> setUsed;
    const QStringList aMetas = stRoot.entryList({QStringLiteral("*.meta.json")}, QDir::Files);
    for (const QString& sMeta : aMetas) {
        const QJsonObject oFiles = ReadJsonFile(stRoot.filePath(sMeta))
            .object().value(QStringLiteral("files")).toObject();
        for (auto it = oFiles.begin(); it != oFiles.end(); ++it) {
            setUsed.insert(it.value().toObject().value(QStringLiteral("h")).toString());
        }
    }
    int iRemoved = 0;
    const QDir stStore(BsStoreDir(m_sBackupsRoot));
    if (stStore.exists()) {
        // 2026.08.22 顺手清理并发备份留下的残缺 tmp（不匹配 *.zst，不会被误判为孤儿 blob）。
        const QStringList aTmps = stStore.entryList({QStringLiteral("*.tmp*")}, QDir::Files);
        for (const QString& sTmpF : aTmps) {
            QFile::remove(stStore.filePath(sTmpF));
        }
        const QStringList aBlobs = stStore.entryList({QStringLiteral("*.zst")}, QDir::Files);
        // 2026.09.28 在途保护（与 TruncateBefore 同口径）：并发备份是 blob 先落盘、meta 后
        //   提交，无新鲜度判定的 GC 会误删在途 blob 导致该版本 manifest 指向缺失内容。
        //   跳过 10 分钟内落盘的 .zst，留待下轮回收。
        const QDateTime tFreshCut = QDateTime::currentDateTime().addSecs(-600);
        for (const QString& sBlob : aBlobs) {
            const QFileInfo stBlobFi(stStore.filePath(sBlob));
            if (stBlobFi.lastModified() > tFreshCut) {
                continue;   // 新鲜 blob：可能在途，本轮不回收
            }
            if (!setUsed.contains(sBlob.left(sBlob.size() - 4))
                && QFile::remove(stStore.filePath(sBlob))) {
                ++iRemoved;
            }
        }
    }
    return iRemoved;
}

// 2026.09.21 基准截断（用户需求：回退历史太多时选指定记录为基准、删更早的全部记录）。
//   架构依据：每版 manifest 全量自包含，删旧版不伤存活版可恢复性；唯一例外是同源
//   引用版（v1.4.85，meta 无 files 仅 base 指针）——base 落在删除区间的先物化（基版
//   files 拷入该 meta、摘除 base 键），物化失败则整体拒绝不动盘（保持库一致）。
bool CBackupStore::TruncateBefore(int a_iBaseSeq, bool a_bDryRun,
                                  int& a_iDeletedVersions, int& a_iDeletedBlobs,
                                  qint64& a_iFreedBytes, QString& a_sErr) {
    a_iDeletedVersions = 0;
    a_iDeletedBlobs = 0;
    a_iFreedBytes = 0;
    a_sErr.clear();
    if (a_iBaseSeq <= 0) {
        a_sErr = QStringLiteral("基准版本号无效");
        return false;
    }
    const QDir stRoot(m_sBackupsRoot);
    if (!stRoot.exists()) {
        a_sErr = QStringLiteral("备份库不存在");
        return false;
    }
    if (!QFile::exists(BsMetaPath(m_sBackupsRoot, a_iBaseSeq))) {
        a_sErr = QStringLiteral("基准版本 #%1 不存在（可能已被清理）").arg(a_iBaseSeq);
        return false;
    }
    // 安全阀：待恢复现场（notice pending）时禁止截断——还原流程可能正要按老 seq 读内容。
    {
        int iDummySeq = 0;
        QString sDummyOwner;
        if (LoadPendingNotice(iDummySeq, sDummyOwner)) {
            a_sErr = QStringLiteral("有待恢复的回退现场（notice），请先完成或放弃还原再截断");
            return false;
        }
    }
    // 1) 全量扫描 meta → 内存快照（删除前解析引用链——磁盘版 BsResolveFiles 删后就断）。
    const QStringList aMetaNames = stRoot.entryList({QStringLiteral("*.meta.json")}, QDir::Files);
    QMap<int, QString> mapFiles;      // seq → 文件名
    QMap<int, QJsonObject> mapMetas;  // seq → meta 对象
    for (const QString& sName : aMetaNames) {
        const int iDot = sName.indexOf(QLatin1Char('.'));
        bool bOk = false;
        const int iSeq = (iDot > 0 ? sName.left(iDot) : sName).toInt(&bOk);
        if (!bOk || iSeq <= 0) {
            continue;
        }
        mapFiles.insert(iSeq, sName);
        mapMetas.insert(iSeq, ReadJsonFile(stRoot.filePath(sName)).object());
    }
    // 内存版 files 解析（同 BsResolveFiles 语义：base 链 ≤8 跳）。
    std::function<QJsonObject(int)> fnResolve = [&](int a_iSeq) -> QJsonObject {
        QJsonObject o = mapMetas.value(a_iSeq);
        for (int iHop = 0; iHop < 8 && o.contains(QStringLiteral("base")); ++iHop) {
            o = mapMetas.value(o.value(QStringLiteral("base")).toInt());
        }
        return o.value(QStringLiteral("files")).toObject();
    };
    // 2) 存活引用版物化判定：base 在删除区间（seq < 基准）→ 需物化为自包含实版。
    QList<int> aMaterialize;
    for (auto it = mapMetas.constBegin(); it != mapMetas.constEnd(); ++it) {
        const int iSeq = it.key();
        if (iSeq >= a_iBaseSeq && it.value().contains(QStringLiteral("base"))
            && !it.value().contains(QStringLiteral("files"))) {
            if (it.value().value(QStringLiteral("base")).toInt() < a_iBaseSeq) {
                aMaterialize.append(iSeq);
            }
        }
    }
    // 3) 删除区间统计（meta 字节数先量后删）。
    qint64 iMetaBytes = 0;
    int iDelCount = 0;
    for (auto it = mapMetas.constBegin(); it != mapMetas.constEnd(); ++it) {
        if (it.key() < a_iBaseSeq) {
            iMetaBytes += QFileInfo(stRoot.filePath(mapFiles.value(it.key()))).size();
            ++iDelCount;
        }
    }
    if (iDelCount == 0) {
        a_sErr = QStringLiteral("没有比 #%1 更早的记录").arg(a_iBaseSeq);
        return false;   // 无事可做（UI 侧本就不该出菜单）
    }
    // 4) 孤儿 blob 统计：存活区间（物化后）引用并集之外的 blob。新鲜 blob（10 分钟内
    //    写入）跳过——并发备份是 blob 先落盘、meta 后提交，GC 会误删在途内容。
    QSet<QString> setUsed;
    for (auto it = mapMetas.constBegin(); it != mapMetas.constEnd(); ++it) {
        if (it.key() >= a_iBaseSeq) {
            const QJsonObject oFiles = fnResolve(it.key());
            for (auto itF = oFiles.begin(); itF != oFiles.end(); ++itF) {
                setUsed.insert(itF.value().toObject().value(QStringLiteral("h")).toString());
            }
        }
    }
    qint64 iBlobBytes = 0;
    QList<QString> aRemoveBlobs;
    const QDir stStore(BsStoreDir(m_sBackupsRoot));
    if (stStore.exists()) {
        const QDateTime tFreshCut = QDateTime::currentDateTime().addSecs(-600);
        const QStringList aBlobs = stStore.entryList({QStringLiteral("*.zst")}, QDir::Files);
        for (const QString& sBlob : aBlobs) {
            if (setUsed.contains(sBlob.left(sBlob.size() - 4))) {
                continue;
            }
            const QFileInfo stFi(stStore.filePath(sBlob));
            if (stFi.lastModified() > tFreshCut) {
                continue;   // 在途保护：本轮不动，下次截断/GC 再收
            }
            iBlobBytes += stFi.size();
            aRemoveBlobs.append(sBlob);
        }
    }
    a_iDeletedVersions = iDelCount;
    a_iDeletedBlobs = aRemoveBlobs.size();
    a_iFreedBytes = iMetaBytes + iBlobBytes;
    if (a_bDryRun) {
        return true;   // 预估模式：不动盘（引用版物化留给正式执行）
    }
    // 5) 物化存活引用版（删除前置；任一失败 → 整体拒绝，库保持原状）。
    for (const int iSeq : aMaterialize) {
        const QJsonObject oResolved = fnResolve(iSeq);
        if (oResolved.isEmpty()) {
            a_sErr = QStringLiteral("引用版 #%1 的基版内容解析失败，已中止（此前已物化的引用版内容等价，不影响库一致性）")
                         .arg(iSeq);
            return false;
        }
        QJsonObject o = mapMetas.value(iSeq);
        o.remove(QStringLiteral("base"));
        o[QStringLiteral("files")] = oResolved;
        const QString sFinal = BsMetaPath(m_sBackupsRoot, iSeq);
        const QString sTmpF = sFinal + QStringLiteral(".tmp");
        if (!WriteJsonFile(sTmpF, QJsonDocument(o), m_bBom)
            || !QFile::remove(sFinal)          // 引用版 meta 已存在：先删后 rename（同 SetMetaField）
            || !QFile::rename(sTmpF, sFinal)) {
            QFile::remove(sTmpF);
            a_sErr = QStringLiteral("引用版 #%1 物化失败，已中止（此前已物化的引用版内容等价，不影响库一致性）")
                         .arg(iSeq);
            return false;
        }
    }
    // 6) 删除旧 meta + 孤儿 blob（统计实际删除数——完成提示「删了多少文件」用）。
    int iDeleted = 0;
    for (auto it = mapMetas.constBegin(); it != mapMetas.constEnd(); ++it) {
        if (it.key() < a_iBaseSeq
            && QFile::remove(stRoot.filePath(mapFiles.value(it.key())))) {
            ++iDeleted;
        }
    }
    int iBlobsRemoved = 0;
    for (const QString& sBlob : aRemoveBlobs) {
        if (QFile::remove(stStore.filePath(sBlob))) {
            ++iBlobsRemoved;
        }
    }
    a_iDeletedBlobs = iBlobsRemoved;
    if (iDeleted < iDelCount) {
        a_sErr = QStringLiteral("部分旧记录删除失败（%1/%2），可重试")
                     .arg(iDeleted).arg(iDelCount);
        return false;
    }
    return true;
}

bool CBackupStore::HasRestoreTmp() const {
    return QDir(BsTmpDir(m_sBackupsRoot)).exists();
}

// 2026.08.26（自 HarnessAgent 平移）：丢弃 _restore_tmp 残留现场（用户确认放弃还原时用）。
bool CBackupStore::DiscardRestoreTmp(QString& a_sErr) {
    a_sErr.clear();
    const QString sTmp = BsTmpDir(m_sBackupsRoot);
    if (!QDir(sTmp).exists()) {
        return true;
    }
    if (!RemoveTree(sTmp)) {
        a_sErr = QStringLiteral("删除恢复临时目录失败（目录保留，可重试）");
        return false;
    }
    return true;
}

bool CBackupStore::LoadPendingNotice(int& a_iTargetSeq, QString& a_sOwner) const {
    const QJsonObject o = ReadJsonFile(
        QDir(m_sBackupsRoot).filePath(QStringLiteral("notice.json"))).object();
    if (!o.value(QStringLiteral("pending")).toBool(false)) {
        return false;
    }
    a_iTargetSeq = o.value(QStringLiteral("target")).toInt();
    a_sOwner     = o.value(QStringLiteral("owner")).toString();
    return true;
}

void CBackupStore::SetPendingNotice(int a_iTargetSeq, const QString& a_sOwner) {
    QJsonObject o;
    o[QStringLiteral("pending")] = true;
    o[QStringLiteral("target")]  = a_iTargetSeq;
    o[QStringLiteral("time")]    = QDateTime::currentDateTime().toString(Qt::ISODate);
    o[QStringLiteral("owner")]   = a_sOwner;
    QDir().mkpath(m_sBackupsRoot);
    WriteJsonFile(QDir(m_sBackupsRoot).filePath(QStringLiteral("notice.json")),
                  QJsonDocument(o), m_bBom);
}

void CBackupStore::ClearPendingNotice() {
    QFile::remove(QDir(m_sBackupsRoot).filePath(QStringLiteral("notice.json")));
}

bool CBackupStore::SetMetaField(int a_iSeq, const QString& a_sKey, const QJsonValue& a_vValue) {
    const QString sMetaPath = BsMetaPath(m_sBackupsRoot, a_iSeq);
    const QJsonDocument stDoc = ReadJsonFile(sMetaPath);
    if (!stDoc.isObject()) {
        return false;
    }
    QJsonObject o = stDoc.object();
    o[a_sKey] = a_vValue;
    // 2026.08.22 原子重写（tmp+rename）：写一半崩溃不损坏既有 meta（与 Backup 提交同模式）。
    const QString sTmpF = sMetaPath + QStringLiteral(".tmp");
    if (!WriteJsonFile(sTmpF, QJsonDocument(o), m_bBom)) {
        return false;
    }
    if (QFile::exists(sMetaPath) && !QFile::remove(sMetaPath)) {
        QFile::remove(sTmpF);
        return false;
    }
    if (!QFile::rename(sTmpF, sMetaPath)) {
        QFile::remove(sTmpF);
        return false;
    }
    return true;
}

// ===== [/PortableCore] =====

// ============================================================================
// CProjectManager 统一备份库适配层（薄封装：定位路径 + 注入排除清单 + 合并 legacy）
// ============================================================================

QString CProjectManager::UnifiedBackupsDir() const {
    return QDir::cleanPath(AutoDevDir() + QStringLiteral("/backups"));
}

// 2026.08.23 代码相关文件白名单：增量备份只备份代码/配置/文档类文本文件，
//   中间文件与产物（*.obj/*.exe/*.zst/*.png 等）不入库。按扩展名（小写）判定；
//   无扩展名的常见构建/说明文件按文件名判定；其余无扩展名文件一律视为非代码跳过。
bool IsCodeRelatedFile(const QString& a_sRel) {
    static const QSet<QString> s_aCodeExts = {
        // C/C++ 家族
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"),
        QStringLiteral("cxx"), QStringLiteral("c++"), QStringLiteral("h"),
        QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"),
        QStringLiteral("h++"), QStringLiteral("inl"), QStringLiteral("ipp"),
        QStringLiteral("tpp"), QStringLiteral("qml"), QStringLiteral("ui"),
        QStringLiteral("qrc"), QStringLiteral("pro"), QStringLiteral("pri"),
        // 2026.09.28 VS 工程文件（qmake 的 pro/pri 在列而 sln/vcxproj 缺席——本程序管
        //   理的正是 VS/Qt 工程，缺了它们回退时工程文件与源码版本错配）。二进制缓存
        //   （suo/ncb/aps 等）不入列。
        QStringLiteral("sln"), QStringLiteral("vcxproj"),
        QStringLiteral("filters"), QStringLiteral("props"),
        QStringLiteral("vcproj"), QStringLiteral("vsproj"),
        // 脚本与其他语言
        QStringLiteral("cs"), QStringLiteral("java"), QStringLiteral("kt"),
        QStringLiteral("kts"), QStringLiteral("scala"), QStringLiteral("swift"),
        QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("go"),
        QStringLiteral("rs"), QStringLiteral("rb"), QStringLiteral("py"),
        QStringLiteral("pyw"), QStringLiteral("pyi"), QStringLiteral("php"),
        QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"),
        QStringLiteral("cjs"), QStringLiteral("ts"), QStringLiteral("tsx"),
        QStringLiteral("vue"), QStringLiteral("svelte"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("ps1"),
        QStringLiteral("lua"), QStringLiteral("pl"), QStringLiteral("pm"),
        QStringLiteral("r"), QStringLiteral("jl"), QStringLiteral("sql"),
        QStringLiteral("proto"), QStringLiteral("gradle"),
        // 构建/配置
        QStringLiteral("cmake"), QStringLiteral("mk"), QStringLiteral("make"),
        QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"),
        QStringLiteral("toml"), QStringLiteral("ini"), QStringLiteral("cfg"),
        QStringLiteral("conf"), QStringLiteral("properties"), QStringLiteral("xml"),
        QStringLiteral("xaml"), QStringLiteral("gradlew"),
        // 文档/文本
        QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("txt"),
        QStringLiteral("rst"), QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sass"),
        QStringLiteral("less"),
    };
    static const QSet<QString> s_aCodeNames = {
        QStringLiteral("Makefile"), QStringLiteral("makefile"),
        QStringLiteral("GNUmakefile"), QStringLiteral("NMakefile"),
        QStringLiteral("Dockerfile"), QStringLiteral("CMakeLists"),
        QStringLiteral("LICENSE"), QStringLiteral("NOTICE"),
        QStringLiteral(".gitignore"), QStringLiteral(".gitattributes"),
        QStringLiteral(".editorconfig"), QStringLiteral(".dockerignore"),
    };
    const QString sName = QFileInfo(a_sRel).fileName();
    const QString sExt  = QFileInfo(a_sRel).suffix().toLower();
    return s_aCodeExts.contains(sExt) || s_aCodeNames.contains(sName);
}

void CProjectManager::RequestBackupAbort() {
    if (m_pBackupStore != nullptr) {
        m_pBackupStore->RequestAbort();
    }
}

void CProjectManager::ResetBackupAbort() {
    if (m_pBackupStore != nullptr) {
        m_pBackupStore->ResetAbort();
    }
}

CBackupStore* CProjectManager::BackupStore() const {
    QMutexLocker stLock(&m_MutexStore);
    if (m_pBackupStore == nullptr) {
        m_pBackupStore = new CBackupStore(UnifiedBackupsDir(), m_sProjectDir, true);
        // 排除清单谓词：以相对路径首段（顶层一级目录）判定，与旧 MirrorTree 同口径；
        // BackupExcludeDirs 每次现读（含 .autodev 自身 → 库永不递归自备）。
        m_pBackupStore->m_pfFilter = [this](const QString& a_sRel) {
            const int iPos = a_sRel.indexOf(QLatin1Char('/'));
            const QString sTop = (iPos > 0) ? a_sRel.left(iPos) : a_sRel;
            return !BackupExcludeDirs().contains(sTop, Qt::CaseInsensitive);
        };
        // 2026.08.23 文件级白名单：只备份代码相关文件（目录过滤之后再按文件过滤，
        //   中间文件/产物不再入 blob 库；恢复时这些文件原地保留不被触碰）。
        m_pBackupStore->m_pfFileFilter = [](const QString& a_sRel) {
            return IsCodeRelatedFile(a_sRel);
        };
        // 2026.08.27（自 HarnessAgent 平移）：单文件大小上限 10MB，超限不入库不报错
        //（防误伤大体积数据文件拖慢快照/膨胀库）。
        m_pBackupStore->m_iMaxFileBytes = 10 * 1024 * 1024;
    }
    return m_pBackupStore;
}

int CProjectManager::BackupTree(const QString& a_sKind, const QString& a_sOwner,
                                const QString& a_sStage, const QString& a_sPrompt,
                                int a_iTarget) {
    if (m_sProjectDir.isEmpty() || a_sOwner.isEmpty()) {
        return 0;
    }
    return BackupStore()->Backup(a_sKind, a_sOwner, a_sStage, a_sPrompt, a_iTarget);
}

bool CProjectManager::RestoreTree(int a_iSeq, QString& a_sErr) {
    a_sErr.clear();
    if (m_sProjectDir.isEmpty() || a_iSeq <= 0) {
        a_sErr = QStringLiteral("参数无效（工程未打开 / 序号非法）");
        return false;
    }
    // 统一库有该 seq → CAS 恢复。
    if (QFileInfo::exists(BsMetaPath(UnifiedBackupsDir(), a_iSeq))) {
        return BackupStore()->Restore(a_iSeq, a_sErr);
    }
    // legacy 分派：扫 .autodev/tasks/*/backups 找该 seq → 转调旧 RestoreSources。
    const QDir stTasks(AutoDevDir() + QStringLiteral("/tasks"));
    if (stTasks.exists()) {
        const QStringList aTasks = stTasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sTask : aTasks) {
            if (QFileInfo::exists(QDir(BackupsDir(sTask)).filePath(BsMetaName(a_iSeq)))) {
                return RestoreSources(sTask, a_iSeq, a_sErr);
            }
        }
    }
    a_sErr = QStringLiteral("目标版本不存在：%1").arg(a_iSeq);
    return false;
}

bool CProjectManager::RestoreTree(const SBackupVersion& a_stVer, QString& a_sErr) {
    if (a_stVer.bLegacy) {
        return RestoreSources(a_stVer.sOwner, a_stVer.iSeq, a_sErr);
    }
    return RestoreTree(a_stVer.iSeq, a_sErr);
}

QList<SBackupVersion> CProjectManager::ListUnifiedBackups(const QString& a_sOwner) const {
    QList<SBackupVersion> aVersions = BackupStore()->List(a_sOwner);
    // legacy 合并：owner 为任务 id 时收其旧全量库（标 bLegacy / sKind=legacy / sOwner=任务 id）。
    if (!a_sOwner.isEmpty() && a_sOwner != QStringLiteral("global")
        && QDir(BackupsDir(a_sOwner)).exists()) {
        const QList<SBackupVersion> aLegacy = ListBackups(a_sOwner);
        for (SBackupVersion st : aLegacy) {
            st.sKind   = QStringLiteral("legacy");
            st.sOwner  = a_sOwner;
            st.bLegacy = true;
            aVersions.append(st);
        }
    }
    // 两库序号空间独立 → 按 time ISO 倒序统一排序。
    std::sort(aVersions.begin(), aVersions.end(),
              [](const SBackupVersion& a, const SBackupVersion& b) { return a.sTime > b.sTime; });
    return aVersions;
}

bool CProjectManager::DiffVersions(int a_iSeqOld, int a_iSeqNew, SDiffResult& a_stOut) const {
    return BackupStore()->Diff(a_iSeqOld, a_iSeqNew, a_stOut);
}

// 2026.08.24 回退窗口「查看变更」：转发统一库按版本读文件内容。
bool CProjectManager::ReadUnifiedFile(int a_iSeq, const QString& a_sRelPath,
                                      QByteArray& a_bufOut, QString& a_sErr) const {
    return BackupStore()->ReadFile(a_iSeq, a_sRelPath, a_bufOut, a_sErr);
}

int CProjectManager::PurgeOwner(const QString& a_sOwner) {
    return BackupStore()->PurgeOwner(a_sOwner);
}

int CProjectManager::GcOrphanBlobs() {
    return BackupStore()->GcOrphanBlobs();
}

// 2026.09.21 基准截断（转发 PortableCore）。
bool CProjectManager::TruncateUnifiedBefore(int a_iBaseSeq, bool a_bDryRun,
                                            int& a_iDeletedVersions,
                                            int& a_iDeletedBlobs,
                                            qint64& a_iFreedBytes, QString& a_sErr) {
    return BackupStore()->TruncateBefore(a_iBaseSeq, a_bDryRun,
                                         a_iDeletedVersions, a_iDeletedBlobs,
                                         a_iFreedBytes, a_sErr);
}

bool CProjectManager::HasRestoreTmp() const {
    return BackupStore()->HasRestoreTmp();
}

bool CProjectManager::RestoreFromTmp(QString& a_sErr) {
    return BackupStore()->RestoreFromTmp(a_sErr);
}

bool CProjectManager::LoadPendingNotice(int& a_iTargetSeq, QString& a_sOwner) const {
    return BackupStore()->LoadPendingNotice(a_iTargetSeq, a_sOwner);
}

void CProjectManager::SetPendingNotice(int a_iTargetSeq, const QString& a_sOwner) {
    BackupStore()->SetPendingNotice(a_iTargetSeq, a_sOwner);
}

void CProjectManager::ClearPendingNotice() {
    BackupStore()->ClearPendingNotice();
}

bool CProjectManager::SetUnifiedSummary(int a_iSeq, const QString& a_sSummary) {
    return BackupStore()->SetMetaField(a_iSeq, QStringLiteral("summary"), a_sSummary.left(300));
}

// —— 内置测试沙箱（2026-09-26，[Sandbox] 全局键：界面态/能力开关，不随任务覆盖）——
// 统一口径：Save 写 ConfigFilePath（全局）；Load 读同一路径，缺键/非法值回默认。
bool CProjectManager::SaveSandboxEnable(bool a_bOn) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Sandbox", L"Enable",
        a_bOn ? L"1" : L"0", wsPath.c_str());
    return true;
}

bool CProjectManager::LoadSandboxEnable() const {
    // 无 config.ini / 缺键 → 默认开（1）；仅认 0 为关。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return true;
    }
    return GetPrivateProfileIntW(L"Sandbox", L"Enable", 1,
                                 sPath.toStdWString().c_str()) != 0;
}

bool CProjectManager::SaveSandboxAutoGhost(bool a_bOn) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Sandbox", L"AutoGhost",
        a_bOn ? L"1" : L"0", wsPath.c_str());
    return true;
}

bool CProjectManager::LoadSandboxAutoGhost() const {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return true;
    }
    return GetPrivateProfileIntW(L"Sandbox", L"AutoGhost", 1,
                                 sPath.toStdWString().c_str()) != 0;
}

bool CProjectManager::SaveSandboxRecord(bool a_bOn) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Sandbox", L"Record",
        a_bOn ? L"1" : L"0", wsPath.c_str());
    return true;
}

bool CProjectManager::LoadSandboxRecord() const {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    return GetPrivateProfileIntW(L"Sandbox", L"Record", 0,
                                 sPath.toStdWString().c_str()) != 0;
}

bool CProjectManager::SaveSandboxFps(int a_iFps) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Sandbox", L"Fps",
        QString::number(qBound(1, a_iFps, 15)).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadSandboxFps() const {
    // 无 config.ini / 缺键 → 默认 5；非法值（<1 或 >15）兜底回 5。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 5;
    }
    const int iFps = GetPrivateProfileIntW(L"Sandbox", L"Fps", 5,
                                           sPath.toStdWString().c_str());
    return (iFps >= 1 && iFps <= 15) ? iFps : 5;
}

bool CProjectManager::SaveSandboxRetainDays(int a_iDays) {
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Sandbox", L"RetainDays",
        QString::number(qBound(1, a_iDays, 365)).toStdWString().c_str(), wsPath.c_str());
    return true;
}

int CProjectManager::LoadSandboxRetainDays() const {
    // 无 config.ini / 缺键 → 默认 7；非法值（<1 或 >365）兜底回 7。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 7;
    }
    const int iDays = GetPrivateProfileIntW(L"Sandbox", L"RetainDays", 7,
                                            sPath.toStdWString().c_str());
    return (iDays >= 1 && iDays <= 365) ? iDays : 7;
}
