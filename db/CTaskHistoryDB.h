#pragma once
#include <QList>
#include <QString>

struct sqlite3;

class CTaskInfo;

// 任务历史库（SQLite）：按工程隔离——每个工程一份 <工程>/.autodev/history.db，
// 历史页只展示当前打开工程的任务（2026.08.20 由 exe 同级全局库改为工程内库）。
//
// 兼容：Open() 不带参 = 打开旧版 exe 同级全局库（仅迁移/一次性工具使用）。
class CTaskHistoryDB {
public:
    CTaskHistoryDB();
    ~CTaskHistoryDB();

    // 打开/创建指定路径的库并建表；空路径 = 旧版全局库 <exe 目录>/history.db。
    // 失败返回 false（不阻断程序）。
    bool Open(const QString& a_sDbPath = QString());
    // 关闭库（析构会自动调用）。
    void Close();

    // 当前工程的历史库路径：<工程根>/.autodev/history.db。
    static QString ProjectDbPath(const QString& a_sProjectDir);

    // 旧版全局库 → 工程内库一次性迁移（CMainWindow 启动时调用，幂等）：
    //   把全局库各行按 project_dir 分组写入对应工程的 .autodev/history.db，
    //   全部迁完后把全局库改名 history.db.migrated 收档（project_dir 为空或工程
    //   目录已不存在的行留在收档文件里，不丢数据）。返回迁移成功的行数（-1=失败）。
    static int MigrateLegacyToProjects(QString* a_pReport = nullptr);

    // INSERT OR REPLACE 一行（按 id 主键，幂等）。
    bool Upsert(const CTaskInfo* a_pTask);
    // 读全表，逐行构造 CTaskInfo（无父对象，调用方接管生命周期）。
    QList<CTaskInfo*> LoadAll() const;
    // 删除一行。
    bool Delete(const QString& a_sId);

private:
    // 取 <exe 目录>/history.db 全路径（GetModuleFileName 去文件名）——旧版全局库。
    static QString LegacyDbPath();

    // 建表/补列（幂等），Open/Acquire 使用。
    bool EnsureSchema();

    // 2026.08.23 损坏自愈：Open 时 PRAGMA quick_check 体检；verdict 非 ok 判损坏——
    //   收档坏库为 history.db.corrupt-<时间戳>（含 -wal/-shm 残件清理）并原路径
    //   重建空库（返回 true，句柄已换新；重建失败返回 false）。task.json 为权威源，
    //   打开工程时终态任务会被 Upsert 回填，历史记录自动恢复。
    bool VerifyOrReset(sqlite3* a_pDb, const QString& a_sPath);

    // 2026.08.21 短连接（不锁工程文件/目录）：打开工程后不再长期持有 sqlite 句柄
    //   （Windows 下被打开的文件会阻止所在目录改名/删除/移动），改为每次操作
    //   Acquire 打开 → 操作 → Release 关闭；多实例并发仍靠 5s busy 超时 + WAL。
    sqlite3* Acquire() const;
    void Release() const;

    QString m_sDbPath;                   // Open() 记下的库路径（空 = 未打开）
    mutable sqlite3* m_pDb = nullptr;    // 短连接的当前句柄（const 操作可换手）
};
