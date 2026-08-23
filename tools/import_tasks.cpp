#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "core/CTaskInfo.h"
#include "db/CTaskHistoryDB.h"

// 一次性 / 批量导入工具：把 <工程目录>/.autodev/tasks/*/task.json 全量 upsert 进
// 该工程自己的历史库 <工程目录>/.autodev/history.db（2026.08.20 历史库按工程隔离，
// 不再写 exe 同级全局库）。无 task.json 的任务目录跳过（需先还原 task.json）。
//
// 用法: import_tasks <工程目录>
// 说明：开发期辅助工具，仅 CMake 目标，不纳入主程序 AutoCode.exe 与 vcxproj。
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    const QStringList aArgs = app.arguments();
    if (aArgs.size() < 2) {
        qInfo().noquote() << "用法: import_tasks <工程目录>";
        return 1;
    }
    const QString sProject  = QDir::cleanPath(aArgs.at(1));
    const QString sTasksDir = QDir::cleanPath(sProject + QStringLiteral("/.autodev/tasks"));
    const QDir stTasksDir(sTasksDir);
    if (!stTasksDir.exists()) {
        qInfo().noquote() << "找不到任务目录:" << sTasksDir;
        return 1;
    }

    CTaskHistoryDB db;
    const QString sDbPath = CTaskHistoryDB::ProjectDbPath(sProject);
    if (!db.Open(sDbPath)) {
        qInfo().noquote() << "打开历史库失败:" << sDbPath;
        return 1;
    }

    const QStringList aDirs = stTasksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    int iOk = 0, iSkip = 0;
    for (const QString& sId : aDirs) {
        const QString sFile = QDir::cleanPath(
            sTasksDir + QStringLiteral("/") + sId + QStringLiteral("/task.json"));
        if (!QFileInfo::exists(sFile)) {
            qInfo().noquote() << "  跳过" << sId << "(无 task.json)";
            ++iSkip;
            continue;
        }
        CTaskInfo task;
        if (!task.LoadFromFile(sFile)) {
            qInfo().noquote() << "  跳过" << sId << "(task.json 解析失败)";
            ++iSkip;
            continue;
        }
        if (db.Upsert(&task)) {
            qInfo().noquote() << "  导入" << task.Id() << "|" << task.Name();
            ++iOk;
        } else {
            qInfo().noquote() << "  跳过" << sId << "(Upsert 失败)";
            ++iSkip;
        }
    }

    qInfo().noquote() << "----";
    qInfo().noquote() << QStringLiteral("完成: 导入 %1 条, 跳过 %2 条。库: %3")
                         .arg(iOk).arg(iSkip)
                         .arg(QCoreApplication::applicationDirPath() + QStringLiteral("/history.db"));

    // 回读校验：打印当前库内全部历史
    const QList<CTaskInfo*> aAll = db.LoadAll();
    qInfo().noquote() << QStringLiteral("库内现有 %1 条历史:").arg(aAll.size());
    for (const CTaskInfo* pTask : aAll) {
        qInfo().noquote() << QStringLiteral("  %1 | %2 | cost=%3 | in=%4 out=%5")
            .arg(pTask->Id(), pTask->Name(),
                 QString::number(pTask->TotalCost(), 'f', 2),
                 QString::number(pTask->TotalInTokens()),
                 QString::number(pTask->TotalOutTokens()));
    }
    qDeleteAll(aAll);
    return 0;
}
