// CProjectUsage.cpp —— 实现见头文件口径说明（2026-11-05 工程总消耗聚合）。
#include "CProjectUsage.h"

#include "CTaskHistoryDB.h"
#include "core/CTaskInfo.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

SProjectUsage CProjectUsage::Aggregate(const QString& a_sProjectDir) {
    SProjectUsage st;
    if (a_sProjectDir.isEmpty()) {
        return st;
    }

    // ① 任务侧：history.db 全表（短连接，读完即散，不锁工程目录）。
    CTaskHistoryDB db;
    if (db.Open(CTaskHistoryDB::ProjectDbPath(a_sProjectDir))) {
        const QList<CTaskInfo*> apTasks = db.LoadAll();
        for (const CTaskInfo* pTask : apTasks) {
            st.m_dTaskCost += pTask->TotalCost();
            st.m_iTaskIn   += pTask->TotalInTokens();
            st.m_iTaskOut  += pTask->TotalOutTokens();
            st.m_iTaskReqs += pTask->RequestCount();
            ++st.m_nTasks;
            delete pTask;
        }
        db.Close();
    }

    // ② 助手侧：工程级档 + 每任务档（tasks/<id>/assistant.json）。
    const QString sAutoDev = QDir(a_sProjectDir).filePath(QStringLiteral(".autodev"));
    SumAssistantArchive(QDir(sAutoDev).filePath(QStringLiteral("assistant.json")), &st);
    const QDir dirTasks(QDir(sAutoDev).filePath(QStringLiteral("tasks")));
    const QStringList aTaskDirs = dirTasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sId : aTaskDirs) {
        SumAssistantArchive(
            QDir(dirTasks.filePath(sId)).filePath(QStringLiteral("assistant.json")), &st);
    }
    return st;
}

void CProjectUsage::SumAssistantArchive(const QString& a_sJsonPath, SProjectUsage* a_pOut) {
    if (a_pOut == nullptr) {
        return;
    }
    QFile f(a_sJsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return;   // 无档（未用过助手）＝ 0
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        return;   // 坏档按 0，不瘫统计
    }
    const QJsonArray aAssts = doc.object().value(QStringLiteral("assistants")).toArray();
    for (const auto& v : aAssts) {
        const QJsonObject oUsage = v.toObject().value(QStringLiteral("usage")).toObject();
        a_pOut->m_dAsstCost += oUsage.value(QStringLiteral("cost")).toDouble();
        a_pOut->m_iAsstIn   += static_cast<qint64>(
            oUsage.value(QStringLiteral("in")).toDouble());
        a_pOut->m_iAsstOut  += static_cast<qint64>(
            oUsage.value(QStringLiteral("out")).toDouble());
        a_pOut->m_iAsstReqs += oUsage.value(QStringLiteral("reqs")).toInt();
        ++a_pOut->m_nAssts;
    }
}
