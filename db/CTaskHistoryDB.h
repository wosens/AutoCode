#pragma once

#include <QString>
#include <QList>

class CTaskInfo;
struct sqlite3;

// 任务历史 SQLite 库（exe 同级 history.db），用 sqlite3 C API 直接操作。
//
// 定位：跨工程的「全局历史」聚合存储。CMainWindow 在以下时机写入：
//   - 打开工程：把 .autodev/tasks/<id>/task.json 逐条 Upsert（=导入）
//   - 任务每次变更（sigChanged→SaveTask）：Upsert（=镜像写，task.json 照常保留供阶段恢复）
// 「历史任务」页用 LoadAll() 取全局历史显示。
//
// 纯 C++ 类（非 QObject）。一次 Open 对应一个库连接，析构自动 Close。
class CTaskHistoryDB {
public:
    CTaskHistoryDB();
    ~CTaskHistoryDB();

    // 打开/创建 exe 同级 history.db 并建表；失败返回 false（不阻断程序）。
    bool Open();
    // 关闭库（析构会自动调用）。
    void Close();

    // INSERT OR REPLACE 一行（按 id 主键，幂等）。
    bool Upsert(const CTaskInfo* a_pTask);
    // 读全表，逐行构造 CTaskInfo（无父对象，调用方接管生命周期）。
    QList<CTaskInfo*> LoadAll() const;
    // 删除一行。
    bool Delete(const QString& a_sId);

private:
    // 取 <exe 目录>/history.db 全路径（GetModuleFileName 去文件名）。
    QString DbPath() const;

    sqlite3* m_pDb = nullptr;
};
