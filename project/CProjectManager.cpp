#include "CProjectManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>

#include <zstd.h>       // 备份压缩（#2）：第三方静态库 libzstd_static，逐文件 zstd 压缩/解压
#include <Windows.h>
#include <climits>
#include <algorithm>

#include "agent/CClaudeRunner.h"
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
    return true;
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
    QFile stFile(AutoDevDir() + QStringLiteral("/guide.md"));
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
    QFile stFile(AssistantStateFile());
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
    QFile stFile(TaskChatFile(a_sTaskId));
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
    QFile stFile(TaskAssistantFile(a_sTaskId));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return QByteArray();   // 该任务还没用过助手：返回空，调用方判空走清空分支
    }
    return stFile.readAll();
}

QString CProjectManager::TaskMemoryFile(const QString& a_sTaskId) const {
    return TaskDir(a_sTaskId) + QStringLiteral("/memory.md");
}

QString CProjectManager::ReadTaskMemory(const QString& a_sTaskId) const {
    QFile stFile(TaskMemoryFile(a_sTaskId));
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

    // 删除旧 guide，避免 claude 读到过期内容
    QFile::remove(AutoDevDir() + QStringLiteral("/guide.md"));

    if (m_pGuideRunner == nullptr) {
        m_pGuideRunner = new CClaudeRunner(this);
        connect(m_pGuideRunner, &CClaudeRunner::sigInfo,
                this, &CProjectManager::sigGuideLog);
        connect(m_pGuideRunner, &CClaudeRunner::sigAssistantText,
                this, [this](const QString& a_sText) { emit sigGuideLog(a_sText); });
        connect(m_pGuideRunner, qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                    &CClaudeRunner::sigFinished),
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
        "5. 构建/运行方式\n\n"
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
            QFile stRec(AutoDevDir() + QStringLiteral("/record.md"));
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
    QFile stRec(AutoDevDir() + QStringLiteral("/record.md"));
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

    if (m_pGuideSyncRunner == nullptr) {
        m_pGuideSyncRunner = new CClaudeRunner(this);
        connect(m_pGuideSyncRunner, &CClaudeRunner::sigInfo, this, &CProjectManager::sigGuideLog);
        connect(m_pGuideSyncRunner, &CClaudeRunner::sigAssistantText,
                this, [this](const QString& a_sText) { emit sigGuideLog(a_sText); });
        connect(m_pGuideSyncRunner, qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                    &CClaudeRunner::sigFinished),
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
        "3. 若上述所有记录均不影响 guide.md，则不要做任何修改。\n\n"
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
        QFile stRec(AutoDevDir() + QStringLiteral("/record.md"));
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
    QFile stFile(GuideSyncFile());
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
    // 编排扫描器四步（Scan → ReadGuide → Parse → Diff → WriteIncremental），
    // 同步完成（扫描千级文件仅数百毫秒，不冻结 UI）。全程吞错，失败不阻断任务启动。
    if (m_sProjectDir.isEmpty()) {
        return;
    }
    // 协调1：guide-sync agent 正在 Edit guide.md 正文时，跳过本轮指纹回写——
    //   WriteIncremental 是「读全文→改指纹段→rename 整文件覆盖」，与 agent 的 Edit 并发会丢更新。
    //   本函数尽力而为、失败吞掉，跳过一轮下次任务启动补上，无损。
    if (m_pGuideSyncRunner != nullptr && m_pGuideSyncRunner->IsRunning()) {
        return;
    }
    try {
        const QList<SFileFingerprint> aCur = CGuideFingerprintScanner::ScanProject(m_sProjectDir);
        const QString sGuide = ReadGuide();
        if (sGuide.isEmpty()) {
            return;   // 无 guide → 交全量流程
        }
        const QList<SFileFingerprint> aPrev = CGuideFingerprintScanner::ParseFingerprintTable(sGuide);
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
        const QString sGuidePath = AutoDevDir() + QStringLiteral("/guide.md");
        const QString sTs = QDateTime::currentDateTime().toString(Qt::ISODate);
        // 回写失败忽略（尽力而为），不影响后续 InitTaskDir / RunCurrentStage。
        CGuideFingerprintScanner::WriteIncremental(sGuidePath, aCur, stChanges, sTs);
    } catch (...) {
        // 任何异常吞掉：增量同步属增强项，绝不让它中断任务启动。
        return;
    }
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
    for (int i = 0; i < m_aRecentProjects.size(); ++i) {
        if (NormalizePath(m_aRecentProjects.at(i)) == sCmp) {
            m_aRecentProjects.removeAt(i);
            break;
        }
    }
    m_aRecentProjects.prepend(sStored);

    SaveRecent();
}

QStringList CProjectManager::RecentProjects() const {
    return m_aRecentProjects;
}

void CProjectManager::ClearRecent() {
    m_aRecentProjects.clear();
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

void CProjectManager::SaveRecent() {
    // 落盘失败不抛、不退出（NFR-3.3）；内存列表已更新，本会话仍可用。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Recent");

    const int iCount = m_aRecentProjects.size();
    WritePrivateProfileStringW(wsSect.c_str(), L"Count",
                               QString::number(iCount).toStdWString().c_str(),
                               wsPath.c_str());

    for (int i = 0; i < iCount; ++i) {
        // ini 中 1 = 最近（MRU），索引 0 → key "1"。
        const QString sKey = QString::number(i + 1);
        const QString sVal = m_aRecentProjects.at(i);
        WritePrivateProfileStringW(wsSect.c_str(),
                                   sKey.toStdWString().c_str(),
                                   sVal.toStdWString().c_str(),
                                   wsPath.c_str());
    }
}

void CProjectManager::LoadRecent() {
    // config.ini 不存在 → GetPrivateProfileInt 返回默认值 0 → 列表空（NFR-2.4）。
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const std::wstring wsSect = std::wstring(L"Recent");

    const int iCount = GetPrivateProfileIntW(wsSect.c_str(), L"Count", 0, wsPath.c_str());
    if (iCount <= 0) {
        return;
    }

    // ini 中 1 = 最近（MRU）；逆序 prepend 重建，使 1 落到列表最前。
    for (int i = iCount; i >= 1; --i) {
        wchar_t szBuf[MAX_PATH] = {0};
        const DWORD dwCopied = GetPrivateProfileStringW(
            wsSect.c_str(),
            QString::number(i).toStdWString().c_str(),
            L"",
            szBuf, MAX_PATH, wsPath.c_str());
        if (dwCopied == 0) {
            continue;
        }
        const QString sVal = QString::fromWCharArray(szBuf, static_cast<int>(dwCopied));
        if (!sVal.isEmpty()) {
            m_aRecentProjects.prepend(sVal);
        }
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

bool CProjectManager::SaveAutoCompactPct(int a_iPct) {
    // 2026.08.15 比例制：落 [Adhoc] AutoCompactPct（替代已删除的 [Adhoc] AutoCompact 千 token 档位）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 75;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iPct = GetPrivateProfileIntW(L"Adhoc", L"AutoCompactPct", 75, wsPath.c_str());
    return (iPct >= 10 && iPct <= 95) ? iPct : 75;
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"MaxIter", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 3;
}

bool CProjectManager::SaveMaxResume(int a_iN) {
    // 落 [Policy] MaxResume（驳回后单阶段最大续跑次数）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"MaxResume", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 3;
}

bool CProjectManager::SaveReviewResume(int a_iN) {
    // 落 [Policy] ReviewResume（评审阶段驳回后最大续跑次数）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 1;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Policy", L"ReviewResume", 1, wsPath.c_str());
    return (iN >= 1 && iN <= 9) ? iN : 1;
}

bool CProjectManager::SaveClaudeCmd(const QString& a_sCmd) {
    // 落 [Agent] ClaudeCmd（claude 命令，可含路径）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 600;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iSec = GetPrivateProfileIntW(L"Agent", L"AskTimeoutSec", 600, wsPath.c_str());
    return (iSec >= 30 && iSec <= 86400) ? iSec : 600;
}

bool CProjectManager::SaveScanExcludeDirs(const QString& a_sDirs) {
    // 落 [Scan] ExcludeDirs（逗号分隔目录名清单）；空串=写空（回退内置默认）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return 3;
    }
    const std::wstring wsPath = sPath.toStdWString();
    const int iN = GetPrivateProfileIntW(L"Review", L"Parallel", 3, wsPath.c_str());
    return (iN >= 1 && iN <= 8) ? iN : 3;
}

bool CProjectManager::SaveWebSearch(bool a_bOn) {
    // 2026.08.15 落 [Agent] WebSearch（阶段联网检索开关）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return true;
    }
    const std::wstring wsPath = sPath.toStdWString();
    return GetPrivateProfileIntW(L"Agent", L"WebSearch", 1, wsPath.c_str()) != 0;
}

bool CProjectManager::SaveAdhocContextLimit(qint64 a_iLimit) {
    // 落 [Adhoc] ContextLimit（0 表示未设/回退自动探测）；失败不抛。
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
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
    const QStringList aDirs = stSrc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aDirs) {
        if (a_aExclude.contains(sSub, Qt::CaseInsensitive)) {
            continue;
        }
        if (!MirrorTree(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub), a_aExclude)) {
            return false;
        }
    }
    // 文件拷贝
    const QStringList aFiles = stSrc.entryList(QDir::Files);
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
    QFile stSrc(a_sSrc);
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
    QFile stSrc(a_sSrc);
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

// 递归压缩镜像：目录结构原样保留，每个文件压缩为 <file>.zst。排除清单仅顶层一级生效（同 MirrorTree）。
bool CompressTree(const QString& a_sSrc, const QString& a_sDst, const QStringList& a_aExclude) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    const QStringList aDirs = stSrc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aDirs) {
        if (a_aExclude.contains(sSub, Qt::CaseInsensitive)) {
            continue;
        }
        if (!CompressTree(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub), a_aExclude)) {
            return false;
        }
    }
    const QStringList aFiles = stSrc.entryList(QDir::Files);
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
bool RestoreTree(const QString& a_sSrc, const QString& a_sDst) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    const QStringList aDirs = stSrc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aDirs) {
        if (!RestoreTree(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub))) {
            return false;
        }
    }
    const QString sZst = QStringLiteral(".zst");
    const QStringList aFiles = stSrc.entryList(QDir::Files);
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
    const QString sPath = ConfigFilePath();
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
    const QString sPath = ConfigFilePath();
    if (sPath.isEmpty()) {
        return false;
    }
    const std::wstring wsPath = sPath.toStdWString();
    WritePrivateProfileStringW(L"Backup", L"ExcludeDirs",
        a_sDirs.toStdWString().c_str(), wsPath.c_str());
    return true;
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
    return { QStringLiteral(".autodev"), QStringLiteral("build"),
             QStringLiteral("Execute"), QStringLiteral(".git"),
             QStringLiteral(".vs"), QStringLiteral("x64") };
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
    QFile stRead(sMetaPath);
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
        QFile stFile(stBackups.filePath(sMeta));
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
    if (!RestoreTree(sVerDir, sRoot)) {
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
