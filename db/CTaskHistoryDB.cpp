#include "CTaskHistoryDB.h"

#include <QDir>
#include <QFileInfo>
#include <QDateTime>
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

QString CTaskHistoryDB::DbPath() const {
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

bool CTaskHistoryDB::Open() {
    if (m_pDb != nullptr) {
        return true; // 已打开
    }

    const QString sPath = DbPath();
    if (sPath.isEmpty()) {
        return false;
    }
    const QByteArray aPath = sPath.toUtf8();

    // sqlite3_open 自动建库（不存在则创建）。
    if (sqlite3_open(aPath.constData(), &m_pDb) != SQLITE_OK) {
        // 打开失败：sqlite3_open 仍可能分配了句柄，需关闭以防泄露。
        if (m_pDb != nullptr) {
            sqlite3_close(m_pDb);
            m_pDb = nullptr;
        }
        return false;
    }

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
    // 2026.08.15 旧库迁移：补 run_ms 列（列已存在时 ALTER 报错，忽略即可）。
    sqlite3_exec(m_pDb, "ALTER TABLE tasks ADD COLUMN run_ms INTEGER DEFAULT 0",
                 nullptr, nullptr, &szErr);
    sqlite3_free(szErr);
    return true;
}

void CTaskHistoryDB::Close() {
    if (m_pDb != nullptr) {
        // close_v2 在还有未 finalize 的 stmt 时会延迟真正释放；本类每次操作都 finalize，故即时释放。
        sqlite3_close_v2(m_pDb);
        m_pDb = nullptr;
    }
}

bool CTaskHistoryDB::Upsert(const CTaskInfo* a_pTask) {
    if (m_pDb == nullptr || a_pTask == nullptr || a_pTask->Id().isEmpty()) {
        return false;
    }

    const char* szSql =
        "INSERT OR REPLACE INTO tasks "
        "(id, name, user_request, type, status, project_dir, cur_stage, stage_iter, "
        " cost, in_tok, out_tok, err, created_at, updated_at, run_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(m_pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        return false;
    }

    // 14 列绑定（顺序与 VALUES 占位符一致）。
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
    BindText(pStmt, 12, a_pTask->LastError());
    BindText(pStmt, 13, a_pTask->CreatedAt().toString(Qt::ISODate));
    BindText(pStmt, 14, QDateTime::currentDateTime().toString(Qt::ISODate));
    sqlite3_bind_int64(pStmt, 15, a_pTask->RunElapsedMs());   // 2026.08.15 运行耗时（含运行中段至今）

    const int iRc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    return iRc == SQLITE_DONE;
}

QList<CTaskInfo*> CTaskHistoryDB::LoadAll() const {
    QList<CTaskInfo*> aResult;
    if (m_pDb == nullptr) {
        return aResult;
    }

    const char* szSql =
        "SELECT id, name, user_request, type, status, project_dir, cur_stage, stage_iter, "
        "       cost, in_tok, out_tok, err, created_at, updated_at, run_ms "
        "FROM tasks ORDER BY created_at DESC";

    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(m_pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        return aResult;
    }

    while (sqlite3_step(pStmt) == SQLITE_ROW) {
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
        pTask->SetLastError(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 11))));

        const char* szCreated = reinterpret_cast<const char*>(sqlite3_column_text(pStmt, 12));
        if (szCreated != nullptr) {
            pTask->SetCreatedAt(QDateTime::fromString(QString::fromUtf8(szCreated), Qt::ISODate));
        }
        pTask->SetRunElapsedMs(sqlite3_column_int64(pStmt, 14));   // 2026.08.15 历史表耗时列
        aResult.append(pTask);
    }

    sqlite3_finalize(pStmt);
    return aResult;
}

bool CTaskHistoryDB::Delete(const QString& a_sId) {
    if (m_pDb == nullptr || a_sId.isEmpty()) {
        return false;
    }

    const char* szSql = "DELETE FROM tasks WHERE id = ?";
    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(m_pDb, szSql, -1, &pStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    BindText(pStmt, 1, a_sId);
    const int iRc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    return iRc == SQLITE_DONE;
}
