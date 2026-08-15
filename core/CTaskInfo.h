#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>

#include "TaskTypes.h"

// 单个任务的数据模型。QObject 子类：通过 sigChanged 通知 UI 刷新 + 触发持久化。
//
// 持久化策略：阶段进度、cost、控制标志等关键状态变化均 emit sigChanged，
// 由 CMainWindow 连接该信号把任务序列化到 .autodev/tasks/<id>/task.json，
// 实现「终止后重启可从上次阶段继续」。LoadFromFile 直接赋值成员、不 emit，
// 避免加载过程触发回写。
class CTaskInfo : public QObject {
    Q_OBJECT

public:
    // 一次阶段执行的记录
    struct SStageRun {
        EStage  eStage    = EStage::Intake;
        int     iIter     = 0;        // 该阶段第几次执行
        bool    bOk       = false;    // 是否成功完成
        QString sSummary;             // 摘要（同时写入 record.md）
    };

    explicit CTaskInfo(QObject* a_pParent = nullptr);

    // —— 基础属性 ——
    QString Id() const { return m_sId; }
    void SetId(const QString& a_sId) { m_sId = a_sId; }

    QString Name() const { return m_sName; }
    void SetName(const QString& a_sName) { m_sName = a_sName; }

    QString UserRequest() const { return m_sUserRequest; }
    void SetUserRequest(const QString& a_sReq) { m_sUserRequest = a_sReq; }

    ETaskType Type() const { return m_eType; }
    void SetType(ETaskType a_eType) { if (m_eType != a_eType) { m_eType = a_eType; emit sigChanged(); } }

    // 流程档位（创建时选定的预设：决定评审聚合方式/轻量评审轮次/审查视角数；
    // 默认 Standard，旧 task.json 缺键回退 Standard）。档位随任务终身保留，再次执行沿用。
    ETier Tier() const { return m_eTier; }
    void SetTier(ETier a_eTier) { if (m_eTier != a_eTier) { m_eTier = a_eTier; emit sigChanged(); } }

    // 自定义阶段清单（EStage 值序列，顺序须符合该类型规范流；空 = 完整规范流，向后兼容）。
    // 新建任务对话框按档位预设勾选，用户可自由增删；编排器按此走流水线。
    QList<EStage> FlowStages() const { return m_aFlowStages; }
    void SetFlowStages(const QList<EStage>& a_aStages) {
        if (m_aFlowStages != a_aStages) { m_aFlowStages = a_aStages; emit sigChanged(); }
    }

    ETaskStatus Status() const { return m_eStatus; }
    void SetStatus(ETaskStatus a_eStatus);

    QString ProjectDir() const { return m_sProjectDir; }
    void SetProjectDir(const QString& a_sDir) { m_sProjectDir = a_sDir; }

    QDateTime CreatedAt() const { return m_dtCreatedAt; }
    void SetCreatedAt(const QDateTime& a_dt) { m_dtCreatedAt = a_dt; }

    // —— 阶段进度 ——
    EStage CurrentStage() const { return m_eCurStage; }
    void SetCurrentStage(EStage a_eStage) {
        if (m_eCurStage != a_eStage) { m_eCurStage = a_eStage; emit sigChanged(); }
    }

    int StageIter() const { return m_iStageIter; }
    void IncStageIter() { ++m_iStageIter; emit sigChanged(); }
    // 绝对值设置阶段迭代次数（用于从 SQLite 历史库恢复）
    void SetStageIter(int a_iIter) { if (m_iStageIter != a_iIter) { m_iStageIter = a_iIter; emit sigChanged(); } }
    void ResetStageIter() {
        if (m_iStageIter != 0) { m_iStageIter = 0; emit sigChanged(); }
    }

    // —— cost / token 累计 ——
    double TotalCost() const { return m_dCost; }
    qint64 TotalInTokens() const { return m_iInTok; }
    qint64 TotalOutTokens() const { return m_iOutTok; }
    void AddUsage(double a_dCost, qint64 a_iIn, qint64 a_iOut);
    // 绝对值设置累计用量（用于从 SQLite 历史库恢复，非累加）
    void SetUsage(double a_dCost, qint64 a_iIn, qint64 a_iOut);

    // —— 运行耗时（2026.08.15，task.json 与 history.db 均持久化）——
    // SetStatus 进/出 Running 自动打点累计（暂停/停止冻结、继续累加，跨段求和）；
    // 状态栏「运行中」后显示；列表/查看任务时一并展示。
    qint64 RunElapsedMs() const;
    // 绝对值回填（SQLite 历史库加载用；不发 sigChanged，避免加载触发回写）
    void SetRunElapsedMs(qint64 a_iMs) { m_iRunMs = a_iMs; }

    // —— 历史 ——
    const QList<SStageRun>& History() const { return m_aHistory; }
    void AppendHistory(const SStageRun& a_run);

    // —— 错误 / 控制标志 ——
    QString LastError() const { return m_sErr; }
    void SetLastError(const QString& a_s) { m_sErr = a_s; emit sigChanged(); }

    bool PauseRequested() const { return m_bPauseReq; }
    void SetPauseRequested(bool a_b) {
        if (m_bPauseReq != a_b) { m_bPauseReq = a_b; emit sigChanged(); }
    }

    bool StopRequested() const { return m_bStopReq; }
    void SetStopRequested(bool a_b) {
        if (m_bStopReq != a_b) { m_bStopReq = a_b; emit sigChanged(); }
    }

    // —— 熔断后人工决策（指导意见）——
    // 迭代熔断时用户在升级弹窗里填写的指导意见：随下一次生产阶段 prompt 注入
    // （「## 人工指导意见」块，最高优先级），注入一次即清空，不泄漏到后续阶段。
    QString ResumeGuidance() const { return m_sResumeGuidance; }
    void SetResumeGuidance(const QString& a_sGuidance) {
        if (m_sResumeGuidance != a_sGuidance) { m_sResumeGuidance = a_sGuidance; emit sigChanged(); }
    }

    // —— 会话续跑 / 编译修复计数 ——
    // 本阶段 claude 会话 id：命中 max-turns 或编译失败时，凭它用 --resume 续跑同一会话。
    QString LastSessionId() const { return m_sLastSessionId; }
    void SetLastSessionId(const QString& a_sSid) {
        if (m_sLastSessionId != a_sSid) { m_sLastSessionId = a_sSid; emit sigChanged(); }
    }
    // 本阶段续跑/修复次数：封顶编排器 k_iMaxResume；阶段推进或评审驳回回退时清零。
    int ResumeCount() const { return m_iResumeCount; }
    void IncResumeCount() { ++m_iResumeCount; emit sigChanged(); }
    void ResetResumeCount() {
        if (m_iResumeCount != 0) { m_iResumeCount = 0; emit sigChanged(); }
    }

    // —— 持久化：序列化到 / 从 JSON 文件恢复（路径由调用方决定）——
    bool SaveToFile(const QString& a_sFilePath) const;
    bool LoadFromFile(const QString& a_sFilePath);

    // 「再次执行」复位：已完成任务从头重跑——阶段回 Intake、清历史/计数/cost/会话/错误。
    // 失败/停止任务不从首阶段重置（仅清运行态标志），由调用方在 OnReviveTask 处理。
    void ResetForRevive();

signals:
    // 任何属性变化时发出：UI 据此刷新，CMainWindow 据此回写 task.json
    void sigChanged();

private:
    QString     m_sId;
    QString     m_sName;
    QString     m_sUserRequest;     // 用户在 UI 填写的原始需求（喂给 intake 阶段）
    ETaskType   m_eType      = ETaskType::Feature;
    ETier       m_eTier      = ETier::Standard;   // 流程档位（默认标准，评审多数决）
    QList<EStage> m_aFlowStages;                  // 自定义阶段清单（空=该类型完整规范流）
    ETaskStatus m_eStatus    = ETaskStatus::Pending;
    QString     m_sProjectDir;
    EStage      m_eCurStage  = EStage::Intake;
    int         m_iStageIter = 0;
    double      m_dCost      = 0.0;
    qint64      m_iInTok     = 0;
    qint64      m_iOutTok    = 0;
    QList<SStageRun> m_aHistory;
    QString     m_sErr;
    bool        m_bPauseReq  = false;
    bool        m_bStopReq   = false;
    QString     m_sLastSessionId;   // 本阶段 claude 会话 id（供 --resume 续跑/编译修复）
    int         m_iResumeCount = 0; // 本阶段续跑/修复次数（封顶 k_iMaxResume，推进时清零）
    QString     m_sResumeGuidance;  // 熔断后人工指导意见（注入下一次生产阶段 prompt 后清空）
    QDateTime   m_dtCreatedAt;
    qint64      m_iRunMs    = 0;     // 已结束运行段的累计耗时（ms；进/出 Running 由 SetStatus 结算）
    QDateTime   m_dtRunStart;       // 本运行段起点（无效=当前不在 Running 段）
};
