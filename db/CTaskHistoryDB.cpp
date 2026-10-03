#include "CTaskHistoryDB.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <windows.h>

#include "sqlite3.h"
#include "core/CTaskInfo.h"

namespace {
// 绑定 TEXT：从 QString 转 UTF-8 的临时 QByteArray，必须用 SQLITE_TRANSIENT 让 sqlite 拷贝（避免悬垂）。
void BindText(sqlite3_stmt* a_pStmt, int a_iIdx, const QString& a_sText) {
    const QByteArray aUtf8 = a_sText.toUtf8();
    sqlite3_bind_text(a_pStmt, a_iIdx, aUtf8.constData(), aUtf8.size(), SQLITE_TRANSIENT);
}
} // namespace

CTaskHistoryDB::CTaskHistoryDB() = default;

CTaskHistoryDB::~CTaskHistoryDB() {
    Close();
}

QString CTaskHistoryDB::LegacyDbPath() {
    // 取 exe 全路径 → 去文件名 → 拼 history.db（沿用 CProjectManager::ConfigFilePath 套路）。
    wchar_t szBuf[MAX_PATH] = {0};
    const DWORD dwLen = ::GetModuleFileNameW(nullptr, szBuf, MAX_PATH);
    if (dwLen == 0 || dwLen >= MAX_PATH) {
        return QString();
    }
    const QString sExe = QString::fromWCharArray(szBuf, static_cast<int>(dwLen));
    QDir stDir = QFileInfo(sExe).absoluteDir();
    return QDir::cleanPath(stDir.absolutePath() + QStringLiteral("/history.db"));
}

QString CTaskHistoryDB::ProjectDbPath(const QString& a_sProjectDir) {
    if (a_sProjectDir.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(QDir(a_sProjectDir).absolutePath()
                           + QStringLiteral("/.autodev/history.db"));
}

bool CTaskHistoryDB::Open(const QString& a_sDbPath) {
    // 2026.08.21 短连接：Open 只验证路径可用（打开→建表→随手关闭）并记下路径，
    //   不长期持有句柄——否则工程目录会因 history.db 被占用而无法改名/删除/移动。
    Close();
    // 2026.08.20 历史库按工程隔离：默认路径 = 旧版全局库（仅迁移/一次性工具使用），
    //   主程序在 OpenProjectDir 里传 <工程>/.autodev/history.db。
    const QString sPath = a_sDbPath.isEmpty() ? LegacyDbPath() : a_sDbPath;
    if (sPath.isEmpty()) {
        return false;
    }
    const QByteArray aPath = sPath.toUtf8();

    // sqlite3_open 自动建库（不存在则创建）；父目录需已存在（工程侧由 EnsureAutoDev 保证）。
    QDir().mkpath(QFileInfo(sPath).absolutePath());
    sqlite3* pDb = nullptr;
    if (sqlite3_open(aPath.constData(), &pDb) != SQLITE_OK) {
        // 打开失败：sqlite3_open 仍可能分配了句柄，需关闭以防泄露。
        if (pDb != nullptr) {
            sqlite3_close(pDb);
        }
        return false;
    }
    m_pDb = pDb;
    // 2026.08.21 多实例并发写保护：同一工程的 .autodev/history.db 可能被多个 AutoCode
    //   实例同时打开（Execute / Execute - 副本 双实例共用），无 busy 超时时 Upsert 撞锁
    //   立即返回 SQLITE_BUSY 静默失败——终态任务从当前表移除、历史表又没写入，两表凭空
    //   消失。设 5s busy 等待 + WAL（读写不互斥）双保险。
    sqlite3_busy_timeout(m_pDb, 5000);
    sqlite3_exec(m_pDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // 2026.08.23 自愈损坏库：终态任务（Done/Failed/归档）只存于 history.db——库一旦
    //   malformed（多实例并发/异常退出残留），LoadAll 静默返回空表，历史任务整体
    //   「凭空消失」。此处 Open 时 quick_check 体检：损坏即把坏库收档为
    //   history.db.corrupt-<时间戳> 并重建空库；随后 CMainWindow 打开工程的
    //   LoadAllTasks 循环会对全部终态任务 Upsert 回填（task.json 为权威源），自愈完成。
    if (!VerifyOrReset(pDb, sPath)) {
        Release();
        return false;
    }
    const bool bOk = EnsureSchema();
    Release();   // 验证完即关，不留文件锁
    if (bOk) {
        m_sDbPath = sPath;
    }
    return bOk;
}

bool CTaskHistoryDB::VerifyOrReset(sqlite3* a_pDb, const QString& a_sPath) {
    // 返回值 = 库可用（健康原样 / 已重建均可）；false = 重建失败（Open 中止）。
    // quick_check 只扫主 btree（比 integrity_check 快且够用）。
    bool bCorrupt = false;
    QString sVerdict;
    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(a_pDb, "PRAGMA quick_check;", -1, &pStmt, nullptr) != SQLITE_OK) {
        // prepare 阶段即报 SQLITE_CORRUPT（页头/树损坏）：判损坏（quick_check 是
        // pragma，健康库含全新空库 prepare 必成功，不存在误伤路径）。
        sqlite3_finalize(pStmt);
        bCorrupt = true;
    } else {
        if (sqlite3_step(pStmt) == SQLITE_ROW) {
            sVerdict = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 0)));
        }
        sqlite3_finalize(pStmt);
        if (sVerdict.compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
            bCorrupt = true;   // verdict 为错误描述清单
        }
    }
    if (!bCorrupt) {
        return true;
    }

    // 损坏：关句柄 → 收档坏库（含 -wal/-shm 残件）→ 原路径重建空库。
    Release();
    const QString sBak = a_sPath + QStringLiteral(".corrupt-")
                         + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QFile::rename(a_sPath, sBak);   // 收档失败（占用）则走覆盖重建，不中断打开流程
    QFile::remove(a_sPath + QStringLiteral("-wal"));
    QFile::remove(a_sPath + QStringLiteral("-shm"));
    const QByteArray aPath = a_sPath.toUtf8();
    sqlite3* pNew = nullptr;
    if (sqlite3_open(aPath.constData(), &pNew) != SQLITE_OK) {
        if (pNew != nullptr) {
            sqlite3_close(pNew);
        }
        return false;
    }
    m_pDb = pNew;
    sqlite3_busy_timeout(m_pDb, 5000);
    sqlite3_exec(m_pDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return true;
}

sqlite3* CTaskHistoryDB::Acquire() const {
    if (m_pDb != nullptr) {
        return m_pDb;   // 嵌套调用复用当前句柄（与 Release 成对）
    }
    if (m_sDbPath.isEmpty()) {
        return nullptr;
    }
    const QByteArray aPath = m_sDbPath.toUtf8();
    sqlite3* pDb = nullptr;
    if (sqlite3_open(aPath.constData(), &pDb) != SQLITE_OK) {
        if (pDb != nullptr) {
            sqlite3_close(pDb);
        }
        return nullptr;
    }
    m_pDb = pDb;
    sqlite3_busy_timeout(m_pDb, 5000);
    sqlite3_exec(m_pDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return m_pDb;
}

void CTaskHistoryDB::Release() const {
    if (m_pDb != nullptr) {
        // close_v2 在还有未 finalize 的 stmt 时会延迟真正释放；本类每次操作都 finalize，
        // 故即时释放——句柄归零后 -wal/-shm 收档，工程目录恢复可改名/删除/移动。
        sqlite3_close_v2(m_pDb);
        m_pDb = nullptr;
    }
}

bool CTaskHistoryDB::EnsureSchema() {
    // 建表（幂等）。离散列，不存 stage history 数组（历史列表用不到，仍在 task.json）。
    const char* szSql =
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id           TEXT PRIMARY KEY,"
        "  name         TEXT,"
        "  user_request TEXT,"
        "  type         INTEGER,"
        "  status       INTEGER,"
        "  project_dir  TEXT,"
        "  cur_stage    INTEGER,"
        "  stage_iter   INTEGER,"
        "  cost         REAL,"
        "  in_tok       INTEGER,"
        "  out_tok      INTEGER,"
        "  req_count    INTEGER,"
        "  err          TEXT,"
        "  created_at   TEXT,"
        "  updated_at   TEXT,"
        "  run_ms       INTEGER"
        ");";
    char* szErr = nullptr;
    if (sqlite3_exec(m_pDb, szSql, nullptr, nullptr, &szErr) != SQLITE_OK) {
        sqlite3_free(szErr);
        sqlite3_close(m_pDb);
        m_pDb = nullptr;
        return false;
    }
    // 2026.08.15 旧库迁移：补 run_ms / req_count 列（列已存在时 ALTER 报错，忽略即可）。
    sqlite3_exec(m_pDb, "ALTER TABLE tasks ADD COLUMN run_ms INTEGER DEFAULT 0",
                 nullptr, nullptr, &szErr);
    sqlite3_free(szErr);
    sqlite3_exec(m_pDb, "ALTER TABLE tasks ADD COLUMN req_count INTEGER DEFAULT 0",
                 nullptr, nullptr, &szErr);
    sqlite3_free(szErr);
    return true;
}

void CTaskHistoryDB::Close() {
    Release();
    m_sDbPath.clear();   // 短连接：Close 同时弃用路径（Open 后才可再操作）
}

bool CTaskHistoryDB::Upsert(const CTaskInfo* a_pTask) {
    sqlite3* pDb = Acquire();
    if (pDb == nullptr || a_pTask == nullptr || a_pTask->Id().isEmpty()) {
        return false;
    }

    const char* szSql =
        "INSERT OR REPLACE INTO tasks "
        "(id, name, user_request, type, status, project_dir, cur_stage, stage_iter, "
        " cost, in_tok, out_tok, req_count, err, created_at, updated_at, run_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        Release();
        return false;
    }

    // 16 列绑定（顺序与 VALUES 占位符一致：id..run_ms）。
    BindText(pStmt, 1,  a_pTask->Id());
    BindText(pStmt, 2,  a_pTask->Name());
    BindText(pStmt, 3,  a_pTask->UserRequest());
    sqlite3_bind_int (pStmt, 4,  static_cast<int>(a_pTask->Type()));
    sqlite3_bind_int (pStmt, 5,  static_cast<int>(a_pTask->Status()));
    BindText(pStmt, 6,  a_pTask->ProjectDir());
    sqlite3_bind_int (pStmt, 7,  static_cast<int>(a_pTask->CurrentStage()));
    sqlite3_bind_int (pStmt, 8,  a_pTask->StageIter());
    sqlite3_bind_double(pStmt, 9,  a_pTask->TotalCost());
    sqlite3_bind_int64(pStmt, 10, a_pTask->TotalInTokens());
    sqlite3_bind_int64(pStmt, 11, a_pTask->TotalOutTokens());
    sqlite3_bind_int(pStmt, 12, a_pTask->RequestCount());   // 2026.08.15 API 请求数
    BindText(pStmt, 13, a_pTask->LastError());
    BindText(pStmt, 14, a_pTask->CreatedAt().toString(Qt::ISODate));
    BindText(pStmt, 15, QDateTime::currentDateTime().toString(Qt::ISODate));
    sqlite3_bind_int64(pStmt, 16, a_pTask->RunElapsedMs());   // 2026.08.15 运行耗时（含运行中段至今）

    const int iRc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    Release();   // 短连接：用完即关（不锁工程文件/目录）
    return iRc == SQLITE_DONE;
}

QList<CTaskInfo*> CTaskHistoryDB::LoadAll() const {
    QList<CTaskInfo*> aResult;
    sqlite3* pDb = Acquire();
    if (pDb == nullptr) {
        return aResult;
    }

    const char* szSql =
        "SELECT id, name, user_request, type, status, project_dir, cur_stage, stage_iter, "
        "       cost, in_tok, out_tok, req_count, err, created_at, updated_at, run_ms "
        "FROM tasks ORDER BY created_at DESC";

    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        Release();
        return aResult;
    }

    int iStepRc = sqlite3_step(pStmt);
    while (iStepRc == SQLITE_ROW) {
        auto* pTask = new CTaskInfo(); // 无父对象，调用方接管生命周期
        pTask->SetId(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 0))));
        pTask->SetName(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 1))));
        pTask->SetUserRequest(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 2))));
        pTask->SetType(static_cast<ETaskType>(sqlite3_column_int(pStmt, 3)));
        pTask->SetStatus(static_cast<ETaskStatus>(sqlite3_column_int(pStmt, 4)));
        pTask->SetProjectDir(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 5))));
        pTask->SetCurrentStage(static_cast<EStage>(sqlite3_column_int(pStmt, 6)));
        pTask->SetStageIter(sqlite3_column_int(pStmt, 7));
        pTask->SetUsage(sqlite3_column_double(pStmt, 8),
                        sqlite3_column_int64(pStmt, 9),
                        sqlite3_column_int64(pStmt, 10));
        pTask->SetRequestCount(sqlite3_column_int(pStmt, 11));   // 2026.08.15 API 请求数
        pTask->SetLastError(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 12))));

        const char* szCreated = reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 13));
        if (szCreated != nullptr) {
            pTask->SetCreatedAt(QDateTime::fromString(QString::fromUtf8(szCreated), Qt::ISODate));
        }
        pTask->SetRunElapsedMs(sqlite3_column_int64(pStmt, 15));   // 2026.08.15 历史表耗时列
        aResult.append(pTask);
        iStepRc = sqlite3_step(pStmt);
    }
    // 读完≠成功：BUSY/损坏等错误同样退出循环，与「空表」无法区分（历史任务凭空消失
    //   事故同形态）——非 SQLITE_DONE 时告警留痕，便于排查。
    if (iStepRc != SQLITE_DONE) {
        qWarning() << "[CTaskHistoryDB] LoadAll 读取中断：rc=" << iStepRc
                   << "（已读" << aResult.size() << "行）";
    }

    sqlite3_finalize(pStmt);
    Release();   // 短连接：用完即关（不锁工程文件/目录）
    return aResult;
}

bool CTaskHistoryDB::Delete(const QString& a_sId) {
    sqlite3* pDb = Acquire();
    if (pDb == nullptr || a_sId.isEmpty()) {
        return false;
    }

    const char* szSql = "DELETE FROM tasks WHERE id = ?";
    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        Release();
        return false;
    }
    BindText(pStmt, 1, a_sId);
    const int iRc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    Release();   // 短连接：用完即关（不锁工程文件/目录）
    return iRc == SQLITE_DONE;
}

int CTaskHistoryDB::MigrateLegacyToProjects(QString* a_pReport) {
    const QString sLegacy = LegacyDbPath();
    if (sLegacy.isEmpty() || !QFile::exists(sLegacy)) {
        return 0;   // 无旧库（全新安装/已迁移）→ 幂等直接返回
    }

    CTaskHistoryDB stLegacy;
    if (!stLegacy.Open()) {
        return -1;
    }
    const QList<CTaskInfo*> aRows = stLegacy.LoadAll();
    if (aRows.isEmpty()) {
        // 空旧库：也收档，避免每次启动空跑。
        stLegacy.Close();
        QFile::remove(sLegacy + QStringLiteral(".migrated"));
        QFile::rename(sLegacy, sLegacy + QStringLiteral(".migrated"));
        return 0;
    }

    // 按 project_dir 分组（规范化路径去斜杠差异；空/已不存在目录的行不迁，留收档文件）。
    QHash<QString, QList<CTaskInfo*>> mapGroups;
    for (CTaskInfo* pTask : aRows) {
        if (pTask == nullptr) { continue; }
        const QString sDir = QDir::cleanPath(
            QDir::fromNativeSeparators(pTask->ProjectDir()));
        if (sDir.isEmpty() || sDir == QStringLiteral(".") || !QFileInfo::exists(sDir)) {
            continue;
        }
        mapGroups[sDir].append(pTask);
    }

    int iMigrated = 0;
    int iFailed = 0;   // 迁移失败行数（目标 Open 失败或 Upsert 失败）：非零则保留旧库供下次重试
    QStringList aProjects;
    for (auto it = mapGroups.constBegin(); it != mapGroups.constEnd(); ++it) {
        CTaskHistoryDB stTarget;
        if (!stTarget.Open(ProjectDbPath(it.key()))) {
            iFailed += it.value().size();   // 整组失败：全组行待重试
            continue;
        }
        for (CTaskInfo* pTask : it.value()) {
            if (stTarget.Upsert(pTask)) {
                ++iMigrated;
            } else {
                ++iFailed;
            }
        }
        stTarget.Close();
        if (iFailed == 0) {
            aProjects << it.key();
        }
    }
    qDeleteAll(aRows);

    // 全部可迁移行落位后才收档旧库；存在失败行时保留原路径旧库供下次启动重试
    //   （目标侧 Upsert 幂等）——旧实现无条件改名，失败行的历史任务会静默消失。
    stLegacy.Close();
    if (iFailed == 0) {
        QFile::remove(sLegacy + QStringLiteral(".migrated"));
        QFile::rename(sLegacy, sLegacy + QStringLiteral(".migrated"));
    }

    if (a_pReport != nullptr) {
        *a_pReport = QStringLiteral("%1 行 → %2 个工程（%3）；失败 %4 行（旧库保留待重试）")
                         .arg(iMigrated).arg(aProjects.size())
                         .arg(aProjects.join(QStringLiteral("; "))).arg(iFailed);
    }
    return iMigrated;
}
