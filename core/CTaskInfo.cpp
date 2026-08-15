#include "CTaskInfo.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

CTaskInfo::CTaskInfo(QObject* a_pParent)
    : QObject(a_pParent) {
    m_dtCreatedAt = QDateTime::currentDateTime();
}

void CTaskInfo::SetStatus(ETaskStatus a_eStatus) {
    if (m_eStatus != a_eStatus) {
        // 2026.08.15 运行耗时打点：进 Running 记段起点；离开 Running 结算本段累计
        //   （暂停/停止冻结、继续再累加，RunElapsedMs 跨段求和）。
        if (a_eStatus == ETaskStatus::Running) {
            m_dtRunStart = QDateTime::currentDateTime();
        } else if (m_eStatus == ETaskStatus::Running && m_dtRunStart.isValid()) {
            m_iRunMs += m_dtRunStart.msecsTo(QDateTime::currentDateTime());
            m_dtRunStart = QDateTime();
        }
        m_eStatus = a_eStatus;
        emit sigChanged();
    }
}

qint64 CTaskInfo::RunElapsedMs() const {
    // 运行中 = 已累计段 + 本段至今；非运行中 = 已累计段（冻结值）。
    if (m_eStatus == ETaskStatus::Running && m_dtRunStart.isValid()) {
        return m_iRunMs + m_dtRunStart.msecsTo(QDateTime::currentDateTime());
    }
    return m_iRunMs;
}

void CTaskInfo::AddUsage(double a_dCost, qint64 a_iIn, qint64 a_iOut) {
    m_dCost  += a_dCost;
    m_iInTok += a_iIn;
    m_iOutTok += a_iOut;
    emit sigChanged();
}

void CTaskInfo::SetUsage(double a_dCost, qint64 a_iIn, qint64 a_iOut) {
    m_dCost  = a_dCost;
    m_iInTok = a_iIn;
    m_iOutTok = a_iOut;
    emit sigChanged();
}

void CTaskInfo::AppendHistory(const SStageRun& a_run) {
    m_aHistory.append(a_run);
    emit sigChanged();
}

bool CTaskInfo::SaveToFile(const QString& a_sFilePath) const {
    QJsonObject oRoot;
    oRoot["id"]          = m_sId;
    oRoot["name"]        = m_sName;
    oRoot["userRequest"] = m_sUserRequest;
    oRoot["type"]        = static_cast<int>(m_eType);
    oRoot["tier"]        = static_cast<int>(m_eTier);   // 流程档位（简单/标准/严格）
    // 自定义阶段清单（空数组不落盘 → 视为完整规范流，旧版读取兼容）
    if (!m_aFlowStages.isEmpty()) {
        QJsonArray aFlow;
        for (EStage e : m_aFlowStages) { aFlow.append(static_cast<int>(e)); }
        oRoot["flowStages"] = aFlow;
    }
    oRoot["status"]      = static_cast<int>(m_eStatus);
    oRoot["projectDir"]  = m_sProjectDir;
    oRoot["curStage"]    = static_cast<int>(m_eCurStage);
    oRoot["stageIter"]   = m_iStageIter;
    oRoot["cost"]        = m_dCost;
    oRoot["inTok"]       = m_iInTok;    // Qt6 QJsonValue 原生支持 qint64
    oRoot["outTok"]      = m_iOutTok;
    oRoot["err"]         = m_sErr;
    oRoot["pauseReq"]    = m_bPauseReq;
    oRoot["stopReq"]     = m_bStopReq;
    oRoot["lastSessionId"] = m_sLastSessionId;
    oRoot["resumeCount"]   = m_iResumeCount;
    oRoot["resumeGuidance"] = m_sResumeGuidance;
    oRoot["runMs"]       = m_iRunMs;   // 2026.08.15 运行耗时（已结算段累计；运行段另存 runStart）
    oRoot["runStart"]    = m_dtRunStart.isValid() ? m_dtRunStart.toString(Qt::ISODate)
                                                  : QString();
    oRoot["createdAt"]   = m_dtCreatedAt.toString(Qt::ISODate);
    oRoot["updatedAt"]   = QDateTime::currentDateTime().toString(Qt::ISODate);

    QJsonArray aHist;
    for (const SStageRun& r : m_aHistory) {
        QJsonObject oRun;
        oRun["stage"]   = static_cast<int>(r.eStage);
        oRun["iter"]    = r.iIter;
        oRun["ok"]      = r.bOk;
        oRun["summary"] = r.sSummary;
        aHist.append(oRun);
    }
    oRoot["history"] = aHist;

    const QJsonDocument stDoc(oRoot);
    QFile stFile(a_sFilePath);
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stFile.write(stDoc.toJson(QJsonDocument::Indented));
    return true;
}

void CTaskInfo::ResetForRevive() {
    // 已完成任务的「再次执行」：回到首阶段 Intake，清空历史/累计/会话，等同新建一次。
    // 失败/停止任务不进此分支（OnReviveTask 仅清运行态标志，从当前阶段续跑）。
    // 档位（m_eTier）与阶段清单（m_aFlowStages）随任务终身保留，再次执行沿用创建时配置。
    m_eCurStage      = EStage::Intake;
    m_aHistory.clear();
    m_iStageIter     = 0;
    m_dCost          = 0.0;
    m_iInTok         = 0;
    m_iOutTok        = 0;
    m_sLastSessionId.clear();
    m_iResumeCount   = 0;
    m_sResumeGuidance.clear();
    m_iRunMs         = 0;   // 2026.08.15 耗时随累计一并清零（失败/停止续跑不进此分支，耗时保留）
    m_dtRunStart     = QDateTime();
    m_sErr.clear();
    m_bPauseReq      = false;
    m_bStopReq       = false;
    emit sigChanged();
}

bool CTaskInfo::LoadFromFile(const QString& a_sFilePath) {
    QFile stFile(a_sFilePath);
    if (!stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray aData = stFile.readAll();
    QJsonParseError stErr;
    const QJsonDocument stDoc = QJsonDocument::fromJson(aData, &stErr);
    if (stErr.error != QJsonParseError::NoError || !stDoc.isObject()) {
        return false;
    }
    const QJsonObject o = stDoc.object();

    // 直接成员赋值，不 emit sigChanged（避免加载过程触发回写）
    m_sId          = o.value("id").toString();
    m_sName        = o.value("name").toString();
    m_sUserRequest = o.value("userRequest").toString();
    m_eType        = static_cast<ETaskType>(
        o.value("type").toInt(static_cast<int>(ETaskType::Feature)));
    m_eTier        = static_cast<ETier>(
        o.value("tier").toInt(static_cast<int>(ETier::Standard)));   // 缺键→标准（向后兼容）
    m_aFlowStages.clear();
    const QJsonArray aFlow = o.value("flowStages").toArray();
    for (const QJsonValue& v : aFlow) {
        m_aFlowStages.append(static_cast<EStage>(v.toInt()));
    }
    m_eStatus      = static_cast<ETaskStatus>(
        o.value("status").toInt(static_cast<int>(ETaskStatus::Pending)));
    m_sProjectDir  = o.value("projectDir").toString();
    m_eCurStage    = static_cast<EStage>(
        o.value("curStage").toInt(static_cast<int>(EStage::Intake)));
    m_iStageIter   = o.value("stageIter").toInt();
    m_dCost        = o.value("cost").toDouble();
    m_iInTok       = o.value("inTok").toInteger();
    m_iOutTok      = o.value("outTok").toInteger();
    m_sErr         = o.value("err").toString();
    m_bPauseReq    = o.value("pauseReq").toBool();
    m_bStopReq     = o.value("stopReq").toBool();
    m_sLastSessionId = o.value("lastSessionId").toString();
    m_iResumeCount   = o.value("resumeCount").toInt();
    m_sResumeGuidance = o.value("resumeGuidance").toString();
    // 2026.08.15 运行耗时：结算段 + 运行段起点（运行中存盘后重启可续算；无效起点=不在运行段）。
    m_iRunMs     = o.value("runMs").toInteger();
    m_dtRunStart = QDateTime::fromString(o.value("runStart").toString(), Qt::ISODate);
    m_dtCreatedAt  = QDateTime::fromString(o.value("createdAt").toString(), Qt::ISODate);

    m_aHistory.clear();
    const QJsonArray aHist = o.value("history").toArray();
    for (const QJsonValue& v : aHist) {
        const QJsonObject oRun = v.toObject();
        SStageRun r;
        r.eStage   = static_cast<EStage>(
            oRun.value("stage").toInt(static_cast<int>(EStage::Intake)));
        r.iIter    = oRun.value("iter").toInt();
        r.bOk      = oRun.value("ok").toBool();
        r.sSummary = oRun.value("summary").toString();
        m_aHistory.append(r);
    }
    return true;
}
