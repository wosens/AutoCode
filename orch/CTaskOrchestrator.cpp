#include "CTaskOrchestrator.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <climits>

#include "CProjectManager.h"
#include "agent/CAgentRunnerFactory.h"
#include "core/CTaskInfo.h"
#include "core/CStageMachine.h"
#include "core/StageDef.h"

CTaskOrchestrator::CTaskOrchestrator(CProjectManager* a_pPm, QObject* a_pParent)
    : QObject(a_pParent), m_pPm(a_pPm) {
    // 2026.08.27 助手 guide 同步空闲节流：旧实现非只读轮结束立即 SyncGuideAsync——大工程下
    //   该调用要 Read 整份 guide+Edit（一次长请求），且与用户的下一轮提示词并发抢网关
    //   （网关限流/缓存竞争，助手每步极慢主因之二）。改为空闲 3 分钟才同步；期间又跑新轮
    //   则重置计时（活跃期不同步），agent/任务仍在跑也顺延，避免与前台调用并发。
    m_tGuideSyncIdle.setSingleShot(true);
    m_tGuideSyncIdle.setInterval(3 * 60 * 1000);
    m_tGuideSyncIdle.callOnTimeout([this]() {
        if (m_pPm == nullptr) {
            return;
        }
        if ((m_pAdhocRunner != nullptr && m_pAdhocRunner->IsRunning())
            || (m_pRunner != nullptr && m_pRunner->IsRunning())
            || (m_pAdhocRunner != nullptr
                && (m_pAdhocRunner->IsGraceActive() || m_pAdhocRunner->IsRetryPending()))) {
            m_tGuideSyncIdle.start();   // 仍在跑：顺延 3 分钟
            return;
        }
        m_pPm->SyncGuideAsync(QString());
    });
}

CTaskOrchestrator::~CTaskOrchestrator() {
    // 后台预备/备份线程只做纯文件 IO（不触 Qt 对象），join 等其自然结束即可；
    // 队列 continuation 以 this 为上下文，析构后不会执行（Qt 自动丢弃）。
    // 2026.08.29 修「窗口关闭后进程僵死」：备份线程首轮全量扫描大工程（含
    //   node_modules 的 pnpm 单仓实测 25 分钟未完）时无超时 join 卡死退出——
    //   先请求中止（扫描/哈希循环及时退出）再 join（秒级）。m_pPm 声明于本对象
    //   之前（CMainWindow 成员逆序析构），此处仍然有效。
    if (m_pPm != nullptr) {
        m_pPm->RequestBackupAbort();
    }
    if (m_thStartPrep.joinable()) {
        m_thStartPrep.join();
    }
    if (m_thBackup.joinable()) {
        m_thBackup.join();
    }
    if (m_thAdhocBackup.joinable()) {
        m_thAdhocBackup.join();
    }
}

void CTaskOrchestrator::AsyncAdhocBackup(const QString& a_sOwner, const QString& a_sPrompt) {
    // 2026.08.26 turn 快照后台化（大工程提交助手卡死修复）：BackupTree 全量扫描+逐文件哈希
    //   在 UI 线程可达数十秒；快照仅作回退保障，失败不阻断本轮提问，异步化无顺序依赖。
    //   线程内纯文件 IO（不触 Qt 控件），结果经 invokeMethod 回本对象线程发事件。
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return;
    }
    CProjectManager* const pPm = m_pPm;
    const QString sOwner = a_sOwner, sPrompt = a_sPrompt;
    if (m_thAdhocBackup.joinable()) {
        // 2026.08.29 上一轮快照未完：先请求中止再等（旧实现裸 join——大工程首轮
        //   扫描几十分钟时，下一次提交提示词会把 UI 卡到扫描结束）。
        m_pPm->RequestBackupAbort();
        m_thAdhocBackup.join();
    }
    m_pPm->ResetBackupAbort();   // 启动新线程前复位（主线程侧，无竞态）
    m_thAdhocBackup = std::thread([this, pPm, sOwner, sPrompt]() {
        const int iSeq = pPm->BackupTree(QStringLiteral("turn"), sOwner,
                                         QString(), sPrompt.left(2000), 0);
        QMetaObject::invokeMethod(this, [this, iSeq]() {
            if (iSeq <= 0) {
                emit sigAdhocEvent(QStringLiteral("backup_failed"),
                    QStringLiteral("本轮无回退快照（备份失败，详见状态提示）"));
            }
        });
    });
}

void CTaskOrchestrator::EnsureRunner() {
    // 2026.08.18 后端漂移重建：设置切换后端后旧 runner 仍在内存——非占用
    //   （运行/宽限/重试等待）时替换为新后端实例；占用中本次仍用旧后端
    //   （OnSettings 已拦运行中切换，此为双保险）。
    if (m_pRunner != nullptr) {
        if (m_pRunner->BackendName() == CAgentRunnerFactory::BackendName()) {
            return;
        }
        if (m_pRunner->IsRunning() || m_pRunner->IsGraceActive()
            || m_pRunner->IsRetryPending()) {
            return;
        }
        m_pRunner->deleteLater();
        m_pRunner = nullptr;   // 落到下方按新后端重建
    }
    m_pRunner = CAgentRunnerFactory::Create(this);
    connect(m_pRunner, &CAgentRunnerBase::sigInfo, this, &CTaskOrchestrator::OnRunnerInfo);
    connect(m_pRunner, &CAgentRunnerBase::sigAssistantText,
            this, &CTaskOrchestrator::OnRunnerAssistantText);
    connect(m_pRunner, &CAgentRunnerBase::sigToolUse,
            this, &CTaskOrchestrator::OnRunnerToolUse);
    connect(m_pRunner, &CAgentRunnerBase::sigToolResult,
            this, &CTaskOrchestrator::OnRunnerToolResult);
    // 2026.08.21 运行中真实计量（Codex 经本地代理解析网关 usage）：即时累计进
    //   任务行费用/输入/输出/请求列并刷新 UI；阶段收尾 sigFinished 携带 0 防双计。
    connect(m_pRunner, &CAgentRunnerBase::sigUsageDelta, this,
            [this](qint64 a_iIn, qint64 a_iOut) {
                if (m_pCurTask == nullptr) {
                    return;
                }
                m_pCurTask->AddUsage(0.0, a_iIn, a_iOut);
                m_pCurTask->AddRequests(1);
                m_iLiveIn += a_iIn;   // 2026.08.27 本轮实时计量（Claude Code 式直观反馈）
                m_iLiveOut += a_iOut;
                emit sigLiveUsage(m_iLiveIn, m_iLiveOut);
                emit sigTaskChanged(m_pCurTask);
            });
    // 2026.08.27 流式实时刷新（绝对值直通显示，不入账）：claude stream_event / pi message_update。
    connect(m_pRunner, &CAgentRunnerBase::sigLivePartial, this,
            &CTaskOrchestrator::sigLiveUsage);
    connect(m_pRunner,
            qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                &CAgentRunnerBase::sigFinished),
            this, &CTaskOrchestrator::OnRunnerFinished);
}

// FR-6 备份已并入 RunCurrentStage 的后台线程路径（备份事件/序号记录移至其 continuation）。

void CTaskOrchestrator::MaybeBackfillBackupSummary(CTaskInfo* a_pTask, EStage a_eStage,
                                                   const QString& a_sSummary) {
    // FR-6（Q6 默认=a 复用阶段摘要）：仅改码阶段、且本轮确实备份过（m_iCurBackupSeq>0）才回填。
    if (a_pTask == nullptr || m_pPm == nullptr || m_iCurBackupSeq <= 0) {
        m_iCurBackupSeq = 0;
        return;
    }
    const bool bMutation = (a_eStage == EStage::Develop ||
                            a_eStage == EStage::BugFix ||
                            a_eStage == EStage::CodeReview);
    if (!bMutation) {
        m_iCurBackupSeq = 0;
        return;
    }
    // FR-6.3：首次备份（无上一版可对比）不记录修改内容——本次序号即该任务最小
    // stage 版本序号时跳过回填（统一库口径：只比 kind=stage 的 CAS 版本，legacy 不计）。
    const QList<SBackupVersion> aVersions = m_pPm->ListUnifiedBackups(a_pTask->Id());
    int iMinSeq = INT_MAX;
    for (const SBackupVersion& st : aVersions) {
        if (!st.bLegacy && st.sKind == QStringLiteral("stage")
            && st.iSeq > 0 && st.iSeq < iMinSeq) {
            iMinSeq = st.iSeq;
        }
    }
    if (iMinSeq != INT_MAX && m_iCurBackupSeq == iMinSeq) {
        m_iCurBackupSeq = 0;   // 首次版本：不回填 summary
        return;
    }
    QString sSum = a_sSummary.trimmed();
    if (sSum.isEmpty()) {
        sSum = QStringLiteral("（%1 阶段完成）").arg(StageNameZh(a_eStage));
    }
    // 截断防 meta.json 过大（摘要取前 300 字符）。
    m_pPm->SetUnifiedSummary(m_iCurBackupSeq, sSum.left(300));
    m_iCurBackupSeq = 0;   // 回填完毕清零
}

void CTaskOrchestrator::StartTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    a_pTask->SetPauseRequested(false);
    a_pTask->SetStopRequested(false);

    if (a_pTask->Status() == ETaskStatus::Done ||
        a_pTask->Status() == ETaskStatus::Running) {
        return;
    }

    // 堵抢占口子：已有别的任务在跑（m_pRunner 忙）且本次启动的不是当前任务 →
    // 不覆盖 m_pCurTask（否则原任务的 OnRunnerFinished 回调会串扰到新任务、原任务变孤儿）。
    // 编排器侧兜底（防绕过 UI 的调用路径）；UI 侧 OnStartTask 有同等拦截 + 友好提示。
    if (IsRunnerBusy() && a_pTask != m_pCurTask) {
        a_pTask->SetStatus(ETaskStatus::Paused);
        StageEvent(a_pTask, QStringLiteral("busy"),
                   QStringLiteral("已有任务运行中，本任务暂置暂停，请先完成或停止当前任务"));
        emit sigTaskChanged(a_pTask);
        return;
    }

    // 工程必须有 guide.md 才能启动（缺失由 UI 触发生成）
    if (!m_pPm->HasGuide()) {
        StageEvent(a_pTask, QStringLiteral("need_guide"),
                           QStringLiteral("工程缺少 .autodev/guide.md，请先点击「生成 Guide」"));
        a_pTask->SetStatus(ETaskStatus::Paused);
        return;
    }

    // FR1.2 触发点：任务启动前增量同步 guide.md 文件指纹表。
    //   扫描千级文件可达数百毫秒~秒级，与任务目录初始化一起移到后台线程，避免
    //   「启动/继续」点击卡死 UI；期间先置 Running（按钮置灰防重复启动）。
    a_pTask->SetStatus(ETaskStatus::Running);
    emit sigTaskChanged(a_pTask);
    StageEvent(a_pTask, QStringLiteral("prep"),
        QStringLiteral("… 启动预备（同步 guide 指纹 / 初始化任务目录）"));

    const QPointer<CTaskInfo> pGuard(a_pTask);
    const QString sTaskId = a_pTask->Id();
    CProjectManager* const pPm = m_pPm;
    if (m_thStartPrep.joinable()) {
        m_thStartPrep.join();   // guards 已保证无重叠，此处仅防御（立即返回）
    }
    m_thStartPrep = std::thread([this, pGuard, sTaskId, pPm]() {
        pPm->SyncGuideIncremental();   // 纯文件 IO，内部吞错，失败不阻断
        pPm->InitTaskDir(sTaskId);
        QMetaObject::invokeMethod(this, [this, pGuard]() {
            if (pGuard.isNull()) {
                return;   // 预备期间任务被删除/归档回收
            }
            CTaskInfo* pTask = pGuard.data();
            // 预备窗口内可能有别的任务抢先起跑（runner 忙且非当前任务）→ 本任务让位。
            if (IsRunnerBusy() && m_pCurTask != pTask) {
                pTask->SetStatus(ETaskStatus::Paused);
                StageEvent(pTask, QStringLiteral("busy"),
                    QStringLiteral("已有任务运行中，本任务暂置暂停，请先完成或停止当前任务"));
                emit sigTaskChanged(pTask);
                return;
            }
            if (pTask->StopRequested()) {
                pTask->SetStatus(ETaskStatus::Stopped);
                StageEvent(pTask, QStringLiteral("stopped"), QStringLiteral("任务已停止"));
                emit sigTaskFinished(pTask, false);
                return;
            }
            RunCurrentStage(pTask);
        }, Qt::QueuedConnection);
    });
}

void CTaskOrchestrator::RunSingleStage(CTaskInfo* a_pTask, EStage a_eStage) {
    // 需求3：右键「再次执行」——仅重跑指定阶段，完成后停在该阶段（Paused），不推进后续、不判死。
    if (a_pTask == nullptr) {
        return;
    }
    if (a_pTask->Status() == ETaskStatus::Running) {
        StageEvent(a_pTask, QStringLiteral("rerun_busy"),
                   QStringLiteral("任务运行中，无法再次执行单阶段"));
        return;
    }
    // 堵抢占口子（与 StartTask 同口径）：别的任务在跑则拒绝。
    if (IsRunnerBusy() && a_pTask != m_pCurTask) {
        a_pTask->SetStatus(ETaskStatus::Paused);
        StageEvent(a_pTask, QStringLiteral("rerun_busy"),
                   QStringLiteral("已有任务运行中，无法再次执行单阶段"));
        emit sigTaskChanged(a_pTask);
        return;
    }
    if (!m_pPm->HasGuide()) {
        StageEvent(a_pTask, QStringLiteral("need_guide"),
                   QStringLiteral("工程缺少 .autodev/guide.md，请先点击「生成 Guide」"));
        a_pTask->SetStatus(ETaskStatus::Paused);
        return;
    }
    // Done/Failed 终态任务允许重跑单阶段：置回 Paused 以便执行（执行完仍停 Paused，不回终态）。
    if (a_pTask->Status() == ETaskStatus::Done || a_pTask->Status() == ETaskStatus::Failed) {
        a_pTask->SetStatus(ETaskStatus::Paused);
        emit sigTaskChanged(a_pTask);   // 终态→活跃：触发列表把任务迁回当前栏
    }
    a_pTask->SetPauseRequested(false);
    a_pTask->SetStopRequested(false);
    a_pTask->ResetResumeCount();
    a_pTask->SetLastSessionId(QString());

    m_pPm->InitTaskDir(a_pTask->Id());
    a_pTask->SetCurrentStage(a_eStage);
    m_bSingleStageRerun = true;   // Advance 完成后据此停原阶段，不推进
    StageEvent(a_pTask, QStringLiteral("rerun_start"),
               QStringLiteral("↻ 再次执行「%1」（仅本阶段，不推进后续）").arg(StageNameZh(a_eStage)));
    RunCurrentStage(a_pTask);
}

void CTaskOrchestrator::PauseTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    a_pTask->SetPauseRequested(true);
    StageEvent(a_pTask, QStringLiteral("pause_requested"),
                       QStringLiteral("已请求暂停（将在当前阶段完成后生效）"));
}

void CTaskOrchestrator::StopTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    a_pTask->SetStopRequested(true);
    // 2026.08.15 主动停止即时收尾：runner Stop 已改静默（不再借 kill 触发的 Crashed→sigFinished
    //   收尾——旧路径把用户停止误报成「claude 进程崩溃 + 阶段失败」），任务终态在此直接落 Stopped；
    //   崩溃宽限期（IsGraceActive）/ 快速失败重试等待（IsRetryPending）同样要停，
    //   否则停止请求会被其后的自动续跑/重试覆盖。
    if (m_pRunner && m_pCurTask == a_pTask
        && (m_pRunner->IsRunning() || m_pRunner->IsGraceActive()
            || m_pRunner->IsRetryPending())) {
        m_pRunner->Stop();
        a_pTask->SetStatus(ETaskStatus::Stopped);
        StageEvent(a_pTask, QStringLiteral("stopped"), QStringLiteral("任务已停止"));
        emit sigTaskFinished(a_pTask, false);
    }
    // 评审并行 runner（若本任务正在评审并行中）一并终止。
    if (m_pCurTask == a_pTask) {
        StopReviewRunners();
    }
    // 编译门是独立异步进程，停止时一并终止，避免 OnBuildFinished 在已停止的任务上继续推进。
    if (m_pBuildProc && m_pBuildProc->state() != QProcess::NotRunning && m_pCurTask == a_pTask) {
        m_pBuildProc->kill();
        m_pBuildProc->waitForFinished(3000);
    }
}

void CTaskOrchestrator::StopAll() {
    // FR-7 最低验收：切工程前停止当前任务 runner + 编译门，并置空 m_pCurTask。
    // 2026.08.22 先 join 后台备份线程：备份期间 BackupTree 长时间使用 CProjectManager
    // 的 store 指针，若不等待就切工程（Open → delete m_pBackupStore）会 use-after-free；
    // 备份为纯文件 IO（秒级），阻塞等待可接受。
    if (m_thBackup.joinable()) {
        m_thBackup.join();
    }
    m_bBackupRunning = false;
    if (m_thAdhocBackup.joinable()) {
        m_thAdhocBackup.join();
    }
    // OnRunnerFinished 首行已判 m_pCurTask==nullptr 早返回，故 runner 即便随后异步
    // 投递 finished 信号也不会访问到将被 ClearTasks delete 的任务对象。
    // 不 waitForFinished：避免阻塞 UI；竞态由 m_pCurTask 置空 + 早返回兜住。
    if (m_pRunner != nullptr && (m_pRunner->IsRunning() || m_pRunner->IsGraceActive())) {
        m_pRunner->Stop();   // 2026.08.15 崩溃宽限期一并终止
    }
    StopReviewRunners();   // 评审并行 runner 一并终止，防切工程泄漏进程
    if (m_pBuildProc != nullptr && m_pBuildProc->state() != QProcess::NotRunning) {
        m_pBuildProc->kill();
        m_pBuildProc->waitForFinished(3000);
    }
    m_pCurTask = nullptr;
    m_aPendingTaskInjects.clear();   // 2026.08.19 介入排队随任务上下文一并作废
}

bool CTaskOrchestrator::IsRunnerBusy() const {
    // 主 runner 在跑 或 任一评审并行 runner 在跑 → 全工程忙（堵抢占口子用）。
    // 2026.08.15 崩溃宽限期/快速失败重试等待期间 IsRunning()==false 但 runner 仍被占用，一并计入。
    if (m_pRunner != nullptr
        && (m_pRunner->IsRunning() || m_pRunner->IsGraceActive()
            || m_pRunner->IsRetryPending())) {
        return true;
    }
    for (CAgentRunnerBase* pRunner : m_aReviewRunners) {
        if (pRunner != nullptr
            && (pRunner->IsRunning() || pRunner->IsGraceActive()
                || pRunner->IsRetryPending())) {
            return true;
        }
    }
    return false;
}

void CTaskOrchestrator::SetReviewParallel(int a_iN) {
    // 1=串行回退（评审阶段走原单 runner 路径）；>1=多视角并行。限制 1~8，非法兜底 3。
    m_iReviewParallel = (a_iN >= 1 && a_iN <= 8) ? a_iN : 3;
}

QList<EStage> CTaskOrchestrator::TaskFlow(const CTaskInfo* a_pTask) {
    // 2026.08.15 (流程档位) 清单为空 → 完整规范流（旧任务兼容）；非空 → 按规范序过滤勾选阶段。
    if (a_pTask == nullptr) {
        return CStageMachine::GetFlowStages(ETaskType::Feature);
    }
    return ResolveFlow(a_pTask->Type(), a_pTask->FlowStages());
}

int CTaskOrchestrator::ReviewViewCount(const CTaskInfo* a_pTask, EStage a_eStage) const {
    // 简单档 CodeReview 单视角（省 fan-out/fan-in 开销，ParseReviewDecision 串行判）；
    // 其余评审阶段与标准/严格档 = 全局配置 m_iReviewParallel。
    if (a_eStage == EStage::CodeReview && a_pTask != nullptr
        && a_pTask->Tier() == ETier::Simple) {
        return 1;
    }
    return m_iReviewParallel;
}

void CTaskOrchestrator::StopReviewRunners() {
    // 终止所有评审并行 runner。不 delete（parent=this 托管）；
    // 清 pending 防 OnReviewViewFinished 在 stop 后仍触发 fan-in。
    for (CAgentRunnerBase* pRunner : m_aReviewRunners) {
        if (pRunner != nullptr && (pRunner->IsRunning() || pRunner->IsGraceActive())) {
            pRunner->Stop();   // 2026.08.15 崩溃宽限期一并终止（否则宽限收尾仍会触发 fan-in）
        }
    }
    m_aPendingViews.clear();
}

void CTaskOrchestrator::StartReviewParallel(CTaskInfo* a_pTask, const SStageDef& a_stDef,
                                            const QString& a_sPrompt) {
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    // 清理上一轮残留 runner（deleteLater：Qt 安全删除，避免累积 + 不与 parent=this 冲突）
    for (CAgentRunnerBase* pRunner : m_aReviewRunners) {
        if (pRunner != nullptr) {
            pRunner->Stop();
            pRunner->deleteLater();
        }
    }
    m_aReviewRunners.clear();
    m_aPendingViews.clear();
    m_aViewResults.clear();

    // 2026.08.15 (流程档位) 本轮实际视角数按档位取（简单档 CodeReview=1），并记录供 fan-in 聚合。
    const int iN = ReviewViewCount(a_pTask, a_stDef.eStage);
    m_iCurReviewViews = iN;
    // 输出文件 stem：cr.md → cr；review_design.md → review_design
    const QString sOut = a_stDef.sOutput;
    const QString sStem = sOut.endsWith(QStringLiteral(".md"))
                          ? sOut.left(sOut.size() - 3) : sOut;
    const QString sTaskRelDir = QStringLiteral(".autodev/tasks/%1").arg(a_pTask->Id());

    StageEvent(a_pTask, QStringLiteral("review_fanout_start"),
        QStringLiteral("▶ %1 启动 %2 视角并行评审").arg(a_stDef.sNameZh).arg(iN));

    for (int i = 0; i < iN; ++i) {
        const QString sViewId = QStringLiteral("v%1").arg(i + 1);
        const QString sViewOut = sStem + QStringLiteral("_") + sViewId + QStringLiteral(".md");
        m_aPendingViews.insert(sViewId);

        auto* pRunner = CAgentRunnerFactory::Create(this);   // parent=this → Qt 托管生命周期
        m_aReviewRunners.append(pRunner);

        // Lambda 捕获 viewId + task 指针，转发对话流信号（带 viewId）、finished 转 OnReviewViewFinished。
        // 遵循 OnAdhoc* 纪律：不读 m_pCurTask 路由、不直接调 Advance（并发零串扰的根本）。
        // system 噪音行与 OnRunnerInfo 同口径：只落 run.log 复盘、不上屏（防刷屏卡死 UI）。
        connect(pRunner, &CAgentRunnerBase::sigInfo, this,
            [this, a_pTask, sViewId](const QString& a_sLine) {
                WriteRunLog(a_pTask, QStringLiteral("system"),
                            QStringLiteral("[%1] %2").arg(sViewId, a_sLine));
                if (a_sLine.startsWith(QStringLiteral("[system"))) {
                    return;
                }
                emit sigStageLog(a_pTask, QStringLiteral("[%1] %2").arg(sViewId, a_sLine));
            });
        connect(pRunner, &CAgentRunnerBase::sigAssistantText, this,
            [this, a_pTask, sViewId](const QString& a_sText) {
                WriteRunLog(a_pTask, QStringLiteral("assistant"),
                            QStringLiteral("[%1] %2").arg(sViewId, a_sText));
                emit sigAssistantText(a_pTask, a_sText, sViewId);
            });
        connect(pRunner, &CAgentRunnerBase::sigToolUse, this,
            [this, a_pTask, sViewId](const QString& a_sTool, const QString& a_sInput) {
                WriteRunLog(a_pTask, QStringLiteral("tool_use"),
                            QStringLiteral("[%1] %2 %3").arg(sViewId, a_sTool, a_sInput));
                emit sigToolUse(a_pTask, a_sTool, a_sInput, sViewId);
            });
        connect(pRunner, &CAgentRunnerBase::sigToolResult, this,
            [this, a_pTask, sViewId](const QString& a_sResult) {
                WriteRunLog(a_pTask, QStringLiteral("tool_result"),
                            QStringLiteral("[%1] %2").arg(sViewId, a_sResult));
                emit sigToolResult(a_pTask, a_sResult, sViewId);
            });
        // 2026.08.21 评审视角 runner 的真实计量同样即时累计（收尾 sigFinished/
        //   LastReqCount 在 m_bUsageLive 时已报 0，无双计）。
        connect(pRunner, &CAgentRunnerBase::sigUsageDelta, this,
            [this, a_pTask](qint64 a_iIn, qint64 a_iOut) {
                a_pTask->AddUsage(0.0, a_iIn, a_iOut);
                a_pTask->AddRequests(1);
                m_iLiveIn += a_iIn;   // 2026.08.27 本轮实时计量（评审视角一并汇总）
                m_iLiveOut += a_iOut;
                emit sigLiveUsage(m_iLiveIn, m_iLiveOut);
                emit sigTaskChanged(a_pTask);
            });
        connect(pRunner,
                qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                    &CAgentRunnerBase::sigFinished),
                this, [this, a_pTask, sViewId, pRunner](bool a_bOk, const QString& a_sResult,
                                               qint64 a_iIn, qint64 a_iOut,
                                               double a_dCost, const QString& a_sSid) {
                    OnReviewViewFinished(a_pTask, sViewId, a_bOk, a_sResult,
                                         a_iIn, a_iOut, a_dCost, a_sSid,
                                         pRunner->LastReqCount());
                });

        // 视角 prompt：基础 prompt + 独立评审员提示 + 输出文件覆盖（避免覆盖其他视角报告）
        const QString sViewPrompt = a_sPrompt + QStringLiteral(
            "\n\n## 多视角并行评审（视角 %1/%2）\n"
            "你是第 %1 位**独立**评审员，共有 %2 位评审员同时独立评审同一对象。"
            "请完全独立地给出判断，不要假设其他评审员的结论。\n"
            "**重要**：请把你的完整评审报告（含文末单行「决策：通过」或「决策：驳回」）"
            "写入文件 `%3/%4`（而**不是** `%3/%5`），以免覆盖其他评审员的报告。\n"
        ).arg(i + 1).arg(iN).arg(sTaskRelDir).arg(sViewOut).arg(sOut);

        pRunner->StartStage(a_pTask->ProjectDir(), sViewPrompt, a_stDef.sTools,
                            MaxTurnsFor(a_pTask, a_stDef.eStage));
    }
}

void CTaskOrchestrator::OnReviewViewFinished(CTaskInfo* a_pTask, const QString& a_sViewId,
                                             bool a_bOk, const QString& a_sResult,
                                             qint64 a_iIn, qint64 a_iOut,
                                             double a_dCost, const QString& a_sSid,
                                             int a_iReqs) {
    if (a_pTask == nullptr || !m_aPendingViews.contains(a_sViewId)) {
        return;   // 已被 StopReviewRunners 清理或无任务 → 丢弃（防悬垂 / 防重复 fan-in）
    }
    Q_UNUSED(a_sResult);   // 视角决策以其独立输出文件为准（与 ParseReviewDecision 同源）

    // 累加该视角 cost（每视角一次；fan-in 不重复累加）
    a_pTask->AddUsage(a_dCost, a_iIn, a_iOut);
    a_pTask->AddRequests(a_iReqs);   // 2026.08.15 该视角 API 请求数

    // 读该视角独立输出文件，复用 Tier1 行锚定正则提取决策
    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    const QString sOut = (pDef != nullptr) ? pDef->sOutput : QString();
    const QString sStem = sOut.endsWith(QStringLiteral(".md"))
                          ? sOut.left(sOut.size() - 3) : sOut;
    const QString sViewPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/") +
                              sStem + QStringLiteral("_") + a_sViewId + QStringLiteral(".md");

    SViewResult st;
    st.bOk = a_bOk;
    st.sSid = a_sSid;
    st.dCost = a_dCost; st.iIn = a_iIn; st.iOut = a_iOut;
    st.eDec = EReviewDecision::Pass;   // 默认 Pass：无决策行（max-turns/abstain）不参与一票否决
    if (QFileInfo::exists(sViewPath)) {
        QFile stFile(sViewPath);
        if (stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            st.sSummary = QTextStream(&stFile).readAll();
        }
    }
    static const QRegularExpression reDecision(QStringLiteral("(?m)^\\s*决策\\s*[:：]\\s*(.+?)\\s*$"));
    QString sLastDecision;
    auto itMatch = reDecision.globalMatch(st.sSummary);
    while (itMatch.hasNext()) {
        sLastDecision = itMatch.next().captured(1).trimmed();
    }
    if (!sLastDecision.isEmpty()) {
        const QString sLow = sLastDecision.toLower();
        if (sLow == QStringLiteral("驳回") || sLow.contains(QStringLiteral("reject")) ||
            sLow.contains(QStringLiteral("不通过")) || sLow.contains(QStringLiteral("未通过"))) {
            st.eDec = EReviewDecision::Reject;
        }   // 其余（通过/pass/含 pass）保持 Pass
    }
    m_aViewResults.insert(a_sViewId, st);
    m_aPendingViews.remove(a_sViewId);

    StageEvent(a_pTask, QStringLiteral("review_view_done"),
        QStringLiteral("✔ 视角 %1 完成（%2）").arg(a_sViewId)
            .arg(st.eDec == EReviewDecision::Reject ? QStringLiteral("驳回") : QStringLiteral("通过")));

    if (!m_aPendingViews.isEmpty()) {
        return;   // 还有视角未完成
    }
    FanInReview(a_pTask);
}

void CTaskOrchestrator::FanInReview(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    if (pDef == nullptr) {
        return;
    }
    const EStage eCur = a_pTask->CurrentStage();
    const QString sOut = pDef->sOutput;
    const QString sStem = sOut.endsWith(QStringLiteral(".md"))
                          ? sOut.left(sOut.size() - 3) : sOut;

    // 聚合决策（2026.08.15 评审多数决）：
    //   严格档 = 一票否决（现状逃生门）；标准/简单档 = 严格多数决——Reject 票**过半**才驳回
    //   （N=3 需 2 票；N=2 平票 1:1 → Pass 带意见，偏向推进）；N=1 不走 fan-in（串行路径不变）。
    //   未过半但存在驳回票 → Pass + 少数派意见携带（不阻断流程，意见随产物与后续 prompt 携带）。
    int iReject = 0, iPass = 0, iAbstain = 0;
    for (auto it = m_aViewResults.constBegin(); it != m_aViewResults.constEnd(); ++it) {
        if (it.value().eDec == EReviewDecision::Reject) { ++iReject; }
        else if (it.value().eDec == EReviewDecision::Pass) { ++iPass; }
        else { ++iAbstain; }
    }
    const int iN = m_aViewResults.size();
    const EReviewDecision eAgg =
        (a_pTask->Tier() == ETier::Strict)
            ? ((iReject > 0) ? EReviewDecision::Reject : EReviewDecision::Pass)
            : ((iReject * 2 > iN) ? EReviewDecision::Reject : EReviewDecision::Pass);
    const bool bMinority = (eAgg == EReviewDecision::Pass && iReject > 0);

    // 合并各视角正文（去决策行）写回 sOutput，文末写聚合决策行——保 cr.md 回灌闭环格式。
    // 少数派意见段插在决策行**之前**；末行必须保持字节精确的「决策：通过/驳回」，
    //   否则 ParseReviewDecision Tier1 取最后决策行会漏、Tier2 扫尾时被段内「驳回」字样误判。
    const int iViews = (m_iCurReviewViews > 0) ? m_iCurReviewViews : m_iReviewParallel;
    QString sMerged;
    for (int i = 0; i < iViews; ++i) {
        const QString sViewId = QStringLiteral("v%1").arg(i + 1);
        const SViewResult st = m_aViewResults.value(sViewId);
        if (st.sSummary.isEmpty()) {
            continue;
        }
        QString sBody = st.sSummary;
        static const QRegularExpression reDecision(QStringLiteral("(?m)^\\s*决策\\s*[:：].*$"));
        sBody.remove(reDecision);
        sMerged += QStringLiteral("## 视角 %1（%2）\n%3\n\n")
            .arg(sViewId)
            .arg(st.eDec == EReviewDecision::Reject ? QStringLiteral("驳回") : QStringLiteral("通过"))
            .arg(sBody.trimmed());
    }
    if (bMinority) {
        sMerged += QStringLiteral(
            "## 少数派驳回意见（不阻断，供后续阶段参考）\n"
            "以下视角给出驳回但未达多数，未阻断流程；后续阶段请评估这些意见，"
            "低成本可吸纳的请直接处理：\n\n");
    }
    sMerged += QStringLiteral("\n决策：%1\n")
        .arg(eAgg == EReviewDecision::Reject ? QStringLiteral("驳回") : QStringLiteral("通过"));

    const QString sOutPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/") + sOut;
    QFile stOut(sOutPath);
    if (stOut.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream(&stOut) << sMerged;
    }

    // 摘要 + 历史（类比 Advance 的 RecordSummary + AppendHistory）；多数决 Pass 带意见时注明携带
    const QString sSummary = QStringLiteral("评审并行完成：%1 通过 / %2 驳回 / %3 弃权 → 整体%4%5")
        .arg(iPass).arg(iReject).arg(iAbstain)
        .arg(eAgg == EReviewDecision::Reject ? QStringLiteral("驳回") : QStringLiteral("通过"))
        .arg(bMinority ? QStringLiteral("（未达多数：含少数派驳回意见，已携带至后续阶段）") : QString());
    RecordSummary(a_pTask, eCur, true, sSummary);
    a_pTask->AppendHistory({eCur, a_pTask->StageIter(), true, sSummary});
    emit sigTaskChanged(a_pTask);
    StageEvent(a_pTask, QStringLiteral("review_fanin_done"), sSummary);

    StopReviewRunners();   // 清理本轮 runner（停止 + 清 pending）
    FinishReviewStage(a_pTask, eAgg);
}

void CTaskOrchestrator::FinishReviewStage(CTaskInfo* a_pTask, EReviewDecision a_eDec) {
    if (a_pTask == nullptr) {
        return;
    }
    const EStage eCur = a_pTask->CurrentStage();
    a_pTask->ResetResumeCount();
    a_pTask->SetLastSessionId(QString());

    if (a_eDec == EReviewDecision::Reject) {
        // 2026.08.15 (流程档位) 回炉目标被用户取消勾选时，退到评审阶段在流内的前一阶段
        //   （防跳到未勾选阶段跑一轮不属于本流水线的工作）。
        EStage eBack = CStageMachine::RejectTarget(eCur);
        const QList<EStage> aFlow = TaskFlow(a_pTask);
        if (!aFlow.contains(eBack)) {
            eBack = CStageMachine::PrevStage(aFlow, eCur).value_or(EStage::Intake);
        }
        a_pTask->IncStageIter();
        if (a_pTask->StageIter() > MaxIter()) {
            a_pTask->SetCurrentStage(eBack);
            a_pTask->SetPauseRequested(true);   // 升级到用户
            WriteEscalationReport(a_pTask, eBack);
            const QString sDigest = ExtractRejectDigest(a_pTask);
            StageEvent(a_pTask, QStringLiteral("escalation"),
                QStringLiteral("⚠ %1 已迭代 %2 次仍被驳回，已暂停并升级到用户处理｜报告：%3｜阻塞：%4")
                    .arg(StageNameZh(eBack)).arg(MaxIter())
                    .arg(QStringLiteral(".autodev/tasks/%1/escalation.md").arg(a_pTask->Id()))
                    .arg(sDigest.left(120)));
            emit sigTaskChanged(a_pTask);
            return;   // 升级，不自动推进
        }
        a_pTask->SetCurrentStage(eBack);
        const QString sDigest = ExtractRejectDigest(a_pTask);
        StageEvent(a_pTask, QStringLiteral("reject"),
            QStringLiteral("↩ 评审驳回 → 回退到「%1」（第 %2 次迭代）%3")
                .arg(StageNameZh(eBack)).arg(a_pTask->StageIter())
                .arg(sDigest.isEmpty() ? QString()
                     : QStringLiteral("｜阻塞摘要：%1").arg(sDigest)));
        QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
        return;
    }

    // Pass → 下一阶段（按任务生效流推进；清单为空即完整规范流，行为同旧版）
    a_pTask->ResetStageIter();
    const std::optional<EStage> oNext = CStageMachine::NextStage(TaskFlow(a_pTask), eCur);
    if (!oNext.has_value()) {
        a_pTask->SetStatus(ETaskStatus::Done);
        StageEvent(a_pTask, QStringLiteral("done"), QStringLiteral("🎉 任务全部完成"));
        emit sigTaskFinished(a_pTask, true);
        return;
    }
    a_pTask->SetCurrentStage(*oNext);
    emit sigTaskChanged(a_pTask);
    QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
}

void CTaskOrchestrator::RunCurrentStage(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    ResetLiveUsage();   // 2026.08.27 新阶段开始：实时计量清零

    // 控制标志检查（阶段边界）
    if (a_pTask->StopRequested()) {
        a_pTask->SetStatus(ETaskStatus::Stopped);
        StageEvent(a_pTask, QStringLiteral("stopped"),
                           QStringLiteral("任务已停止"));
        emit sigTaskFinished(a_pTask, false);
        return;
    }
    if (a_pTask->PauseRequested()) {
        a_pTask->SetStatus(ETaskStatus::Paused);
        StageEvent(a_pTask, QStringLiteral("paused"),
                           QStringLiteral("任务已暂停"));
        emit sigTaskChanged(a_pTask);   // #2 状态变更即时刷新按钮（启动禁用/暂停可用）
        return;
    }

    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    if (pDef == nullptr) {
        a_pTask->SetStatus(ETaskStatus::Failed);
        emit sigTaskFinished(a_pTask, false);
        return;
    }

    // 阶段输入自愈预检（#5）：声明输入缺失 → 回退到生产阶段重新生成，
    // 避免评审/消费阶段反复报"缺 requirements.md 等"空耗轮次。
    if (!EnsureStageInputs(a_pTask, *pDef)) {
        return;  // 已回退到上游阶段并安排重跑
    }

    a_pTask->SetStatus(ETaskStatus::Running);
    emit sigTaskChanged(a_pTask);   // #2 进入运行态即时禁用「启动/继续」按钮，防重复点击
    StageEvent(a_pTask, QStringLiteral("stage_start"),
                       QStringLiteral("▶ %1").arg(pDef->sNameZh));

    // FR-6：改码阶段启动 Claude 前全量备份源码（Q5 默认=编排器层整目录快照），失败不阻断流程。
    //   整目录扫描 + 逐文件 zstd 压缩可达秒级 → 后台线程执行，完成后回主线程 LaunchStage。
    const bool bMutation = (pDef->eStage == EStage::Develop ||
                            pDef->eStage == EStage::BugFix ||
                            pDef->eStage == EStage::CodeReview);
    if (bMutation) {
        m_iCurBackupSeq = 0;   // 每阶段开始重置（防上一阶段残留序号被误回填）
        const QPointer<CTaskInfo> pGuard(a_pTask);
        const QString sTaskId = a_pTask->Id();
        const QString sStageName = pDef->sNameZh;
        const SStageDef stDef = *pDef;
        CProjectManager* const pPm = m_pPm;
        if (m_thBackup.joinable()) {
            m_thBackup.join();   // guards 已保证无重叠，此处仅防御（立即返回）
        }
        m_bBackupRunning = true;
        m_pPm->ResetBackupAbort();   // 2026.08.29 新备份线程前复位中止标志
        m_thBackup = std::thread([this, pGuard, sTaskId, sStageName, stDef, pPm]() {
            const int iSeq = pPm->BackupTree(QStringLiteral("stage"), sTaskId,
                                             sStageName, QString(), 0);   // 纯文件 IO（统一 CAS 库）
            m_bBackupRunning = false;   // 2026.08.22 备份结束满（在 invokeMethod 前置，避免下轮 join 前的窗口）
            QMetaObject::invokeMethod(this, [this, pGuard, iSeq, stDef]() {
                if (pGuard.isNull()) {
                    return;   // 备份期间任务被删除/归档回收
                }
                CTaskInfo* pTask = pGuard.data();
                // 备份窗口内可能有别的任务抢先起跑（runner 忙且非当前任务）→ 本任务让位。
                if (IsRunnerBusy() && m_pCurTask != pTask) {
                    pTask->SetStatus(ETaskStatus::Paused);
                    StageEvent(pTask, QStringLiteral("busy"),
                        QStringLiteral("已有任务运行中，本任务暂置暂停，请先完成或停止当前任务"));
                    emit sigTaskChanged(pTask);
                    return;
                }
                if (iSeq > 0) {
                    m_iCurBackupSeq = iSeq;   // 记下供 stage_done 回填摘要
                    StageEvent(pTask, QStringLiteral("backup"),
                        QStringLiteral(" snapshot #%1（改码前源码备份）").arg(iSeq));
                } else {
                    // 备份失败不阻断流程（仅记一条事件），源码回退能力退化为「无本次快照」。
                    StageEvent(pTask, QStringLiteral("backup_failed"),
                        QStringLiteral(" 改码前源码备份失败，本次无回退快照"));
                }
                if (pTask->StopRequested()) {
                    pTask->SetStatus(ETaskStatus::Stopped);
                    StageEvent(pTask, QStringLiteral("stopped"), QStringLiteral("任务已停止"));
                    emit sigTaskFinished(pTask, false);
                    return;
                }
                LaunchStage(pTask, stDef);
            }, Qt::QueuedConnection);
        });
        return;
    }
    LaunchStage(a_pTask, *pDef);
}

void CTaskOrchestrator::LaunchStage(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    // 2026.08.19 换任务时上一任务的介入排队作废（排队指令只属于提交时的任务）。
    if (a_pTask != m_pCurTask) {
        m_aPendingTaskInjects.clear();
    }
    QString sPrompt = BuildPrompt(a_pTask, a_stDef);
    // 2026.08.15 (熔断人工决策) 指导意见已并入本轮 prompt，一次性消费，防泄漏到后续阶段。
    if (!a_pTask->ResumeGuidance().isEmpty()) {
        a_pTask->SetResumeGuidance(QString());
    }
    // 2026.08.15 (联网开关) config.ini [Agent] WebSearch=0 时全阶段禁用联网检索
    //   （deny 黑名单拼接；bypass 模式下 deny 仍生效，见 StageDef.h 工具集注释）。
    QString sTools = a_stDef.sTools;
    if (!m_bWebSearch) {
        if (sTools.isEmpty()) {
            sTools = QStringLiteral("WebSearch,WebFetch");
        } else {
            sTools += QStringLiteral(",WebSearch,WebFetch");
        }
    }

    EnsureRunner();
    m_pCurTask = a_pTask;
    m_sLastAssistantText.clear();
    // 记录本轮阶段启动时刻：评审决策时据此判定评审输出文件是否本轮重写（防读到上一轮遗留旧结论）。
    // 放在异步备份之后记录（真正的 runner 启动点），避免备份耗时影响文件时间戳判定。
    m_dtStageStart = QDateTime::currentDateTime();

    // 评审阶段 + 并行开启 → fan-out 多视角（各写独立 <out>_v<k>.md，fan-in 聚合）；
    // 否则原单 runner 路径（viewId 空，完全向后兼容）。工作目录=工程根。
    // 2026.08.15 (流程档位) 视角数按档位取：简单档 CodeReview=1 → 落串行路径。
    if (ReviewViewCount(a_pTask, a_stDef.eStage) > 1 && IsLightReviewStage(a_stDef.eStage)) {
        StartReviewParallel(a_pTask, a_stDef, sPrompt);
    } else if (!a_pTask->LastSessionId().isEmpty()) {
        // 2026.08.17 中断续跑：任务带 lastSessionId（异常退出残留 / 失败任务再次执行）→
        //   不整阶段从头重跑，--resume 续会话 + 短续跑指令。全量阶段 prompt 重复注入会
        //   诱发重做已完成工作（token 翻倍）；ProceedToNext 推进阶段时已清 sid，
        //   正常顺序启动不受影响。2026.08.19 介入排队（DSH 兜底）并入短续跑指令——
        //   保持续跑语义不回退成全量 prompt。
        QString sResume = QStringLiteral(
            "继续完成本阶段未完成的工作，从上次中断处接着做；不要重复已完成的改动。");
        if (!m_aPendingTaskInjects.isEmpty()) {
            sResume += QStringLiteral("\n\n【用户介入指令】以下是任务运行期间用户提交的"
                                      "追加指令，请在继续工作时优先遵循、一并落实：\n%1")
                          .arg(m_aPendingTaskInjects.join(QStringLiteral("\n")));
            m_aPendingTaskInjects.clear();
        }
        m_pRunner->StartStage(a_pTask->ProjectDir(), sResume,
            sTools, MaxTurnsFor(a_pTask, a_stDef.eStage),
            a_pTask->LastSessionId());
    } else if (!m_aPendingTaskInjects.isEmpty()) {
        // 2026.08.19 介入排队消费（DSH 轮中插话受限的兜底）：排在 prompt 最前面，
        //   强调是运行期间用户提交的指令；一次性消费。评审并行分支不消费（评审
        //   prompt 语义独立，指令留给下一个生产阶段生效）。
        const QString sInjects = m_aPendingTaskInjects.join(QStringLiteral("\n"));
        m_aPendingTaskInjects.clear();
        sPrompt = QStringLiteral(
            "【用户介入指令】以下内容是任务运行期间用户提交的追加指令，"
            "请在执行本阶段工作时优先遵循、一并落实：\n%1\n\n%2").arg(sInjects, sPrompt);
        m_pRunner->StartStage(a_pTask->ProjectDir(), sPrompt, sTools,
                              MaxTurnsFor(a_pTask, a_stDef.eStage));
    } else {
        m_pRunner->StartStage(a_pTask->ProjectDir(), sPrompt, sTools,
                              MaxTurnsFor(a_pTask, a_stDef.eStage));
    }
}

QString CTaskOrchestrator::BuildPrompt(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    QFile stFile(GetPromptsDir() + QStringLiteral("/") + a_stDef.sPromptFile);
    QString sTpl;
    if (stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stIn(&stFile);
        sTpl = stIn.readAll();
    } else {
        sTpl = QStringLiteral("（提示：prompt 模板 %1 缺失，请在 prompts/ 目录补充）\n"
                              "请按本阶段的职责直接开始工作。").arg(a_stDef.sPromptFile);
    }

    const QString sTaskDir = QStringLiteral(".autodev/tasks/%1").arg(a_pTask->Id());
    sTpl.replace(QStringLiteral("{{TASK_ID}}"), a_pTask->Id());
    sTpl.replace(QStringLiteral("{{TASK_DIR}}"), sTaskDir);
    sTpl.replace(QStringLiteral("{{PROJECT_DIR}}"), a_pTask->ProjectDir());
    sTpl.replace(QStringLiteral("{{USER_REQUEST}}"), a_pTask->UserRequest());

    // 2026.08.12 (修驳回死循环) 迭代号：所有阶段都注入，供 prompt 模板做「第 N 次迭代」收敛。
    sTpl.replace(QStringLiteral("{{ITER}}"), QString::number(a_pTask->StageIter()));

    // 2026.08.15 (驳回差异化引导) 反馈闭环通用化：所有可被 RejectTarget 回退的生产阶段
    //   (ConfirmReq/Design/Plan/Develop/RootCause/BugFix) 回炉时，把上一轮评审产物全文
    //   + 递进式策略要求注入 prompt。此前仅 Develop/BugFix 注入 cr.md（2026.08.12），
    //   Design/Plan 等回炉拿到的 prompt 与首次完全一致、且无「换思路」引导，是死循环根源。
    //   不走 aInputs 声明评审文件（会触发 EnsureStageInputs 在首次进入时因文件不存在而
    //   误跳评审空跑一轮），而是这里直读文件。
    if (!RejectFeedbackFile(a_stDef.eStage).isEmpty()) {
        const QString sReviewPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/")
                                  + RejectFeedbackFile(a_stDef.eStage);
        QString sFeedback;
        if (QFileInfo::exists(sReviewPath)) {
            QFile stReview(sReviewPath);
            if (stReview.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream stIn(&stReview);
                sFeedback = stIn.readAll();
            }
        }
        QString sBlock;
        if (!sFeedback.isEmpty()) {
            // 人类可读「第 N 次迭代」= StageIter()+1（iter=0 为首次执行）。
            sBlock = QStringLiteral(
                "\n\n## 上轮评审驳回项（必须逐条解决）\n"
                "这是第 %1 次迭代。上轮评审结论如下，请优先解决其中的 critical/major 阻塞项"
                "（不要重写已通过的部分），并在最终总结里逐条对照说明每条的处理结果"
                "（已解决/已驳回理由/已变更设计）：\n\n%2\n"
                "\n## 本轮策略要求（按迭代递进，禁止沿用未收敛的旧思路）\n%3\n"
            ).arg(a_pTask->StageIter() + 1).arg(sFeedback)
             .arg(IterationStrategyHint(a_pTask->StageIter()));
        }
        // 2026.08.15 (熔断人工决策) 指导意见随下一轮生产阶段 prompt 注入，最高优先级。
        if (!a_pTask->ResumeGuidance().isEmpty()) {
            sBlock += QStringLiteral(
                "\n\n## 人工指导意见（熔断后用户决策，最高优先级，优先于上述策略要求）\n%1\n")
                .arg(a_pTask->ResumeGuidance());
        }
        // 模板有 {{REJECT_FEEDBACK}} 锚点就替换；无锚点但有反馈则追加末尾，保证可见。
        if (sTpl.contains(QStringLiteral("{{REJECT_FEEDBACK}}"))) {
            sTpl.replace(QStringLiteral("{{REJECT_FEEDBACK}}"), sBlock);
        } else if (!sBlock.isEmpty()) {
            sTpl += sBlock;
        }
    }

    // 2026.08.15 (流程档位) 自定义精简流程说明：本阶段声明输入被流过滤（对应阶段未勾选）时，
    //   告知 agent 哪些文档实际可用、被滤文档按不存在处理，按现有资料最小必要推进——
    //   覆盖 Develop 缺 plan.md/design.md、CodeReview 缺 plan.md 等一切勾选组合，无需专用模板。
    {
        QStringList aDropped;
        const QStringList aKept = FilterInputs(a_stDef, TaskFlow(a_pTask), &aDropped);
        if (!aDropped.isEmpty()) {
            const bool bCoding = (a_stDef.eStage == EStage::Develop
                                  || a_stDef.eStage == EStage::BugFix
                                  || a_stDef.eStage == EStage::CodeReview);
            const QString sBlock = QStringLiteral(
                "\n\n## 本任务流水线说明（自定义精简流程）\n"
                "本任务未包含产出以下文档的阶段，模板中提及它们之处一律按「不存在」处理："
                "%1。\n请直接依据实际可用的输入（%2）与工程现有代码工作。%3\n")
                .arg(aDropped.join(QStringLiteral("、")))
                .arg(aKept.join(QStringLiteral("、")))
                .arg(bCoding
                     ? QStringLiteral("没有计划/设计文档时，先在内部形成最小实现方案"
                                      "（1~3 处改动点）再动手，保持改动最小化。")
                     : QString());
            sTpl += sBlock;
        }
    }

    // 2026.08.15 (评审多数决) 上游评审少数派驳回意见软注入：不阻断、不回炉，
    //   交本阶段评估、低成本吸纳（评审产物本身已留档，这里是行动通道）。
    {
        const QString sMinority = MinorityFeedbackBlock(a_pTask, a_stDef);
        if (!sMinority.isEmpty()) {
            sTpl += QStringLiteral(
                "\n\n## 上游评审少数派意见（不阻断，请评估；低成本可吸纳的请直接处理）\n%1\n")
                .arg(sMinority);
        }
    }

    // 2026.08.15 (记忆跟着任务走) 每任务经验记忆注入：.autodev/tasks/<id>/memory.md 是本任务
    //   此前各阶段/助手沉淀的经验（构建命令、踩坑、关键路径），全文注入供参考；并要求 agent
    //   把本轮新经验用 Edit 追加到末尾（句式与 record.md 强制记录规则同款）。
    //   超长截尾保留最近（防长期任务记忆无限膨胀吃掉上下文）。软锚点 {{TASK_MEMORY}}：
    //   模板有则替换、无则追加保底（14 个阶段模板无需逐个改）。
    {
        QString sMemory = (m_pPm != nullptr) ? m_pPm->ReadTaskMemory(a_pTask->Id()) : QString();
        if (sMemory.size() > 8000) {
            sMemory = sMemory.right(8000);
        }
        const QString sBlock = QStringLiteral(
            "\n\n## 本任务经验记忆（memory.md）\n%1\n"
            "（来源：`.autodev/tasks/%2/memory.md`，是本任务此前各阶段沉淀的经验，请优先参考。）\n"
            "若本轮你获得了值得跨阶段记住的经验——实际可用的构建/测试命令、踩过的坑及其解法、"
            "关键文件路径、有效的实现套路——请在结束前向该文件**末尾追加**一条，格式："
            "`- [<阶段名>] <一条经验>`。必须先 Read 该文件再用 Edit 在末尾追加；"
            "文件不存在时允许首次用 Write 创建；**文件已存在后严禁用 Write 覆盖整个文件**。"
            "无新经验则不要改动它。")
            .arg(sMemory.isEmpty() ? QStringLiteral("（暂无——本任务尚未沉淀经验）") : sMemory)
            .arg(a_pTask->Id());
        if (sTpl.contains(QStringLiteral("{{TASK_MEMORY}}"))) {
            sTpl.replace(QStringLiteral("{{TASK_MEMORY}}"), sBlock);
        } else {
            sTpl += sBlock;
        }
    }

    // 验证阶段：注入目标工程构建命令（FR2.3 / Q9，复用 ResolveBuildCommand）。
    //   模板内放 {{BUILD_CMD}} 占位符；探测不到则注入「(未配置/未探测到)」提示，不抛。
    if (a_stDef.eStage == EStage::Verify) {
        const QString sBuild = ResolveBuildCommand(a_pTask);
        const QString sBlock = QStringLiteral(
            "\n\n## 目标工程构建命令\n%1\n"
            "（来源：.autodev/build.txt 或根目录 .sln 自动探测；为空表示未探测到构建方式，"
            "请在指引下自行判断如何编译。）")
            .arg(sBuild.isEmpty() ? QStringLiteral("(未配置/未探测到)") : sBuild);
        if (sTpl.contains(QStringLiteral("{{BUILD_CMD}}"))) {
            sTpl.replace(QStringLiteral("{{BUILD_CMD}}"), sBlock);
        } else {
            sTpl += sBlock;   // 模板无锚点也保底注入
        }
    }

    // (A) 编译门结论注入：CodeReview/Test 读 build_verdict.md，把「构建已验」的权威信号显式交给评审，
    //   消除 agent 自行 grep run.log/task.json 推断构建状态的自我怀疑（be3fd900 根因之一）。
    //   Verify 不注入——它自带 {{BUILD_CMD}} 且要实地编译，有自己的构建结论来源。
    if (a_stDef.eStage == EStage::CodeReview || a_stDef.eStage == EStage::Test) {
        const QString sVerdict = ReadBuildVerdict(a_pTask);
        const QString sBlock = QStringLiteral(
            "\n\n## 上游编译门结论\n%1\n"
            "（来源：任务目录 build_verdict.md，由 Develop 后的编译校验门写入。"
            "pass=已编译通过；fail=未通过（尾部日志见该文件，开发可能已在续跑中部分修复）；"
            "skip=未配置构建命令或启动失败。请据此判断，禁止 grep run.log/task.json 推断构建状态。）")
            .arg(sVerdict.isEmpty()
                 ? QStringLiteral("(无编译门记录——可能本轮未触发编译门，以你本轮亲自读到/编译的结果为准)")
                 : sVerdict);
        if (sTpl.contains(QStringLiteral("{{BUILD_VERDICT}}"))) {
            sTpl.replace(QStringLiteral("{{BUILD_VERDICT}}"), sBlock);
        } else {
            sTpl += sBlock;   // 软锚点：模板无锚点也保底追加
        }
    }
    return sTpl;
}

void CTaskOrchestrator::OnRunnerInfo(const QString& a_sLine) {
    if (m_pCurTask == nullptr) {
        return;
    }
    // 完整流水落 run.log（含被 UI 过滤掉的 system 行，便于事后复盘）
    WriteRunLog(m_pCurTask, QStringLiteral("system"), a_sLine);
    // 精简日志：过滤 session/init 等 system 噪音行（保留 [错误] 等有价值信息）
    if (a_sLine.startsWith(QStringLiteral("[system"))) {
        return;
    }
    emit sigStageLog(m_pCurTask, a_sLine);
}

void CTaskOrchestrator::OnRunnerAssistantText(const QString& a_sText) {
    m_sLastAssistantText = a_sText;  // 缓存最新文本用作摘要（保留全文供摘要）
    if (m_pCurTask == nullptr) {
        return;
    }
    WriteRunLog(m_pCurTask, QStringLiteral("assistant"), a_sText);
    // 全文上抛 UI：由 CExecutionPanel 渲染成 Markdown 助手块，不再在此截断
    emit sigAssistantText(m_pCurTask, a_sText);
}

void CTaskOrchestrator::OnRunnerToolUse(const QString& a_sTool, const QString& a_sInput) {
    if (m_pCurTask == nullptr) {
        return;
    }
    WriteRunLog(m_pCurTask, QStringLiteral("tool_use"),
                a_sTool + QStringLiteral(" ") + a_sInput);
    // 全文入参上抛 UI：由 CExecutionPanel 渲染成可折叠工具卡片，不再在此截断
    emit sigToolUse(m_pCurTask, a_sTool, a_sInput);
}

void CTaskOrchestrator::OnRunnerToolResult(const QString& a_sResult) {
    if (m_pCurTask == nullptr) {
        return;
    }
    WriteRunLog(m_pCurTask, QStringLiteral("tool_result"), a_sResult);
    // 全文结果上抛 UI：回填到前一张未闭合的工具卡片，不再在此截断
    emit sigToolResult(m_pCurTask, a_sResult);
}

void CTaskOrchestrator::OnRunnerFinished(bool a_bOk, const QString& a_sResult,
                                         qint64 a_iIn, qint64 a_iOut,
                                         double a_dCost, const QString& a_sSid) {
    CTaskInfo* pTask = m_pCurTask;
    if (pTask == nullptr) {
        return;
    }
    // 记录本阶段 claude 会话 id，供 max-turns/编译失败时 --resume 续跑。
    if (!a_sSid.isEmpty()) {
        pTask->SetLastSessionId(a_sSid);
    }
    pTask->AddUsage(a_dCost, a_iIn, a_iOut);
    if (m_pRunner != nullptr) {
        pTask->AddRequests(m_pRunner->LastReqCount());   // 2026.08.15 本阶段 API 请求数
    }
    ResetLiveUsage();   // 2026.08.27 阶段结束：本轮实时计量清零
    Advance(pTask, a_bOk, a_sResult);
}

void CTaskOrchestrator::Advance(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sResult) {
    const EStage eCur = a_pTask->CurrentStage();
    const SStageDef* pDef = FindStage(eCur);

    // 摘要：优先 result，其次缓存文本
    QString sSummary = a_sResult;
    if (sSummary.isEmpty()) {
        sSummary = m_sLastAssistantText;
    }
    RecordSummary(a_pTask, eCur, a_bOk, sSummary);
    // FR-1：仅 Verify 阶段额外写一条验证结论行（与统一行并存，AC-1.1/1.3）。
    if (eCur == EStage::Verify) {
        RecordVerifySummary(a_pTask, a_bOk, a_sResult);
    }
    a_pTask->AppendHistory({eCur, a_pTask->StageIter(), a_bOk, sSummary});
    emit sigTaskChanged(a_pTask);

    // 2026.08.20 (自动命名→Agent 定稿)：未填名称的临时名任务，Intake 成功后把
    //   任务内容 + guide.md 拼成提示词交给 Agent 起名（仅一次，回填后清 autoNamed 标记）。
    if (eCur == EStage::Intake && a_bOk && a_pTask->AutoNamed()) {
        AutoNameNow(a_pTask);
    }

    // 需求3：单阶段重跑（右键「再次执行」）——记录产物/历史后即停在原阶段，不推进、不续跑、不判死。
    //   无论本阶段成功或失败（含 max-turns），都置 Paused 结束；后续阶段产物不受影响。
    if (m_bSingleStageRerun) {
        m_bSingleStageRerun = false;
        a_pTask->SetStatus(ETaskStatus::Paused);
        StageEvent(a_pTask, a_bOk ? QStringLiteral("rerun_done") : QStringLiteral("rerun_failed"),
                   QStringLiteral("%1 %2（仅重跑本阶段，未推进后续）")
                       .arg(a_bOk ? QStringLiteral("✔") : QStringLiteral("✗"))
                       .arg(pDef ? pDef->sNameZh : StageNameZh(eCur)));
        emit sigTaskChanged(a_pTask);
        return;
    }

    // 2026.08.17 停滞自动续跑：runner 停滞检测命中（subtype=stall，长时间零输出疑似网络/
    //   中继挂起，进程已被 watchdog 终止）。非真错误——已做工作都在会话里，--resume 接着跑；
    //   与 max-turns 续跑共享本阶段预算（MaxResume），耗尽走正常失败路径（消息含停滞原因）。
    if (!a_bOk && m_pRunner != nullptr && m_pRunner->LastWasStall()
        && !a_pTask->LastSessionId().isEmpty()
        && a_pTask->ResumeCount() < MaxResume()) {
        a_pTask->IncResumeCount();
        StageEvent(a_pTask, QStringLiteral("stall_resume"),
            QStringLiteral("⚠ %1 长时间无输出（疑似网络/中继挂起），已终止并自动续跑（第 %2/%3 次）")
                .arg(pDef ? pDef->sNameZh : QString())
                .arg(a_pTask->ResumeCount())
                .arg(MaxResume()));
        ReRunResume(a_pTask, QStringLiteral(
            "继续完成本阶段未完成的工作，从上次中断处接着做；不要重复已完成的改动。"));
        return;
    }

    // (C) 轻量评审阶段 max-turns 降级（兜底，治本之关键）：
    //   CodeReview/ReviewReq/ReviewDesign/ReviewPlan/RootCauseVerify 命中 max-turns 时不直接判死——
    //   先续跑 1 次催落盘结论（评审语义=立即收尾，非开发语义=接着干活）；续跑仍 max-turns 则降级
    //   走 ProceedToNext，由 ParseReviewDecision 按输出文件决策行判定（有决策行按它，无则 Tier3 默认
    //   Pass 放行）。Test 不在内——它是真验证门，silent-pass 风险高，保留原 IsResumeEligible 续跑+Failed。
    if (!a_bOk && m_pRunner != nullptr && m_pRunner->LastWasMaxTurns()
        && IsLightReviewStage(eCur)) {
        if (!a_pTask->LastSessionId().isEmpty()
            && a_pTask->ResumeCount() < ReviewResume()) {
            a_pTask->IncResumeCount();
            StageEvent(a_pTask, QStringLiteral("review_resume"),
                QStringLiteral("⚠ %1 达到轮次上限，催促落盘结论后续跑（第 %2/%3 次）")
                    .arg(pDef ? pDef->sNameZh : QString())
                    .arg(a_pTask->ResumeCount())
                    .arg(ReviewResume()));
            ReRunResume(a_pTask, ReviewResumeHint(a_pTask, eCur));
            return;
        }
        // 续跑预算用尽仍 max-turns → 降级放行（终态，不再回退，无环路）
        StageEvent(a_pTask, QStringLiteral("review_maxturns_degrade"),
            QStringLiteral("⚠ %1 续跑后仍未收敛，降级按已落盘结论判定（无结论则默认通过）")
                .arg(pDef ? pDef->sNameZh : QString()));
        ProceedToNext(a_pTask, a_sResult);
        return;
    }

    // 续跑：max-turns 命中（非真错误）且阶段可续跑且未超上限 → 自动 --resume 续跑，而非判死。
    if (!a_bOk && m_pRunner != nullptr && m_pRunner->LastWasMaxTurns()
        && !a_pTask->LastSessionId().isEmpty()
        && IsResumeEligible(eCur)
        && a_pTask->ResumeCount() < MaxResume()) {
        a_pTask->IncResumeCount();
        StageEvent(a_pTask, QStringLiteral("resume"),
            QStringLiteral("⚠ %1 达到轮次上限，已完成部分已落盘，自动续跑（第 %2/%3 次）")
                .arg(pDef ? pDef->sNameZh : QString())
                .arg(a_pTask->ResumeCount())
                .arg(MaxResume()));
        ReRunResume(a_pTask, QStringLiteral(
            "继续完成本阶段未完成的工作，从上次中断处接着做；不要重复已完成的改动。"));
        return;
    }

    if (!a_bOk) {
        a_pTask->SetStatus(ETaskStatus::Failed);
        a_pTask->SetLastError(a_sResult);
        // subtype 感知的失败原因：max-turns 续跑耗尽 vs 真错误，避免空消息（result 在 max-turns 时常为空）。
        QString sWhy = a_sResult.left(300);
        // (E) subtype 感知的失败原因：区分「续跑耗尽」与「不可续跑/未续跑」，避免显示固定上限误导
        //   （be3fd900 实测 CodeReview ResumeCount=0 却显示「(3)」）。
        if (m_pRunner != nullptr && m_pRunner->LastWasMaxTurns()) {
            sWhy = (a_pTask->ResumeCount() > 0)
                ? QStringLiteral("达到轮次上限，已续跑 %1 次仍未收敛").arg(a_pTask->ResumeCount())
                : QStringLiteral("达到轮次上限（本阶段不可续跑或未触发续跑）");
        }
        StageEvent(a_pTask, QStringLiteral("stage_failed"),
            QStringLiteral("✗ %1 失败：%2").arg(pDef ? pDef->sNameZh : QString()).arg(sWhy));
        emit sigTaskFinished(a_pTask, false);
        return;
    }

    StageEvent(a_pTask, QStringLiteral("stage_done"),
        QStringLiteral("✔ %1 完成").arg(pDef ? pDef->sNameZh : QString()));

    // FR-6（Q6 默认=a）：阶段成功结束、产物已落盘，把本次摘要回填到本轮备份版本（非首次版本）。
    MaybeBackfillBackupSummary(a_pTask, eCur, sSummary);

    // 编译校验门：Develop/BugFix 产出后、CodeReview 前，跑一次构建拦 broken 编辑。
    //   通过 → ProceedToNext；失败 → 续跑修复或（预算耗尽）放行下游 CodeReview/Test 继续把关。
    if (eCur == EStage::Develop || eCur == EStage::BugFix) {
        RunCompileGate(a_pTask);
        return;
    }
    ProceedToNext(a_pTask, a_sResult);
}

void CTaskOrchestrator::ProceedToNext(CTaskInfo* a_pTask, const QString& a_sResult) {
    if (a_pTask == nullptr) {
        return;
    }
    const EStage eCur = a_pTask->CurrentStage();
    // 阶段真正推进：清零本阶段续跑/修复计数与会话 id，下一阶段从干净态开始。
    a_pTask->ResetResumeCount();
    a_pTask->SetLastSessionId(QString());

    // 评审阶段：解析通过/驳回
    if (IsReviewStage(eCur)) {
        const EReviewDecision eDec = ParseReviewDecision(a_pTask, eCur, a_sResult);
        if (eDec == EReviewDecision::Reject) {
            // 2026.08.15 (流程档位) 回炉目标不在生效流内 → 退到评审阶段的前一个流内阶段
            EStage eBack = CStageMachine::RejectTarget(eCur);
            const QList<EStage> aFlow = TaskFlow(a_pTask);
            if (!aFlow.contains(eBack)) {
                eBack = CStageMachine::PrevStage(aFlow, eCur).value_or(EStage::Intake);
            }
            a_pTask->IncStageIter();
            if (a_pTask->StageIter() > MaxIter()) {
                a_pTask->SetCurrentStage(eBack);
                a_pTask->SetPauseRequested(true);  // 升级到用户
                WriteEscalationReport(a_pTask, eBack);
                const QString sDigest = ExtractRejectDigest(a_pTask);
                StageEvent(a_pTask, QStringLiteral("escalation"),
                    QStringLiteral("⚠ %1 已迭代 %2 次仍被驳回，已暂停并升级到用户处理"
                                   "｜报告：%3｜阻塞：%4")
                        .arg(StageNameZh(eBack)).arg(MaxIter())
                        .arg(QStringLiteral(".autodev/tasks/%1/escalation.md").arg(a_pTask->Id()))
                        .arg(sDigest.left(120)));
            } else {
                a_pTask->SetCurrentStage(eBack);
                const QString sDigest = ExtractRejectDigest(a_pTask);
                StageEvent(a_pTask, QStringLiteral("reject"),
                    QStringLiteral("↩ 评审驳回 → 回退到「%1」（第 %2 次迭代）%3")
                        .arg(StageNameZh(eBack))
                        .arg(a_pTask->StageIter())
                        .arg(sDigest.isEmpty() ? QString() :
                             QStringLiteral("｜阻塞摘要：%1").arg(sDigest)));
            }
            QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
            return;
        }
        // Pass
        a_pTask->ResetStageIter();
    }

    const std::optional<EStage> oNext = CStageMachine::NextStage(TaskFlow(a_pTask), eCur);
    if (!oNext.has_value()) {
        a_pTask->SetStatus(ETaskStatus::Done);
        StageEvent(a_pTask, QStringLiteral("done"),
            QStringLiteral("🎉 任务全部完成"));
        emit sigTaskFinished(a_pTask, true);
        return;
    }

    a_pTask->SetCurrentStage(*oNext);
    emit sigTaskChanged(a_pTask);
    QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
}

void CTaskOrchestrator::ReRunResume(CTaskInfo* a_pTask, const QString& a_sCont) {
    // 续跑原语：--resume <lastSessionId> 恢复同一 claude 会话上下文，带针对性 prompt。
    if (a_pTask == nullptr) {
        return;
    }
    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    // 重置 m_dtStageStart 以正确判定本轮（续跑）评审输出文件新鲜度；助手文本缓存也清空。
    m_dtStageStart = QDateTime::currentDateTime();
    m_sLastAssistantText.clear();
    // 任务侧 --resume 续跑（方案 §6.3）：pending 回退告知前置注入（一次性）。
    QString sPrompt = a_sCont;
    if (m_pPm != nullptr) {
        int iNoticeTarget = 0;
        QString sNoticeOwner;
        // 2026.08.22 owner 校验：任务侧 notice 只由该任务自身消费（防被 global 助手轮抢先清除，
        // 导致任务 --resume 续跑看不到告知）；owner 不匹配则保留待正主。
        if (m_pPm->LoadPendingNotice(iNoticeTarget, sNoticeOwner)
            && sNoticeOwner == a_pTask->Id()) {
            sPrompt.prepend(QStringLiteral(
                "【系统回退通知】工作目录已回退到版本 #%1：此前对话中描述的文件修改已不存在于磁盘。"
                "请以当前磁盘内容为准，不要基于已被回退的修改继续工作。\n---\n").arg(iNoticeTarget));
            m_pPm->ClearPendingNotice();
        }
    }
    EnsureRunner();
    m_pRunner->StartStage(a_pTask->ProjectDir(), sPrompt,
                          pDef ? pDef->sTools : QString(),
                          MaxTurnsFor(a_pTask, a_pTask->CurrentStage()),
                          a_pTask->LastSessionId());
}

void CTaskOrchestrator::RunCompileGate(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    const QString sCmd = ResolveBuildCommand(a_pTask);
    if (sCmd.isEmpty()) {
        // 无构建命令可执行（未配置 build.txt 且未发现 .sln）→ 跳过编译门，直接推进。
        // (A) 落一份 skip 态 verdict，让下游 CodeReview/Test 知晓「未跑构建」而非「构建失败」。
        WriteBuildVerdict(a_pTask, QStringLiteral("skip"), 0, QString(), QString());
        StageEvent(a_pTask, QStringLiteral("compile_skip"),
            QStringLiteral("未配置 .autodev/build.txt 且未发现 .sln，跳过编译校验门"));
        ProceedToNext(a_pTask, QString());
        return;
    }
    // 异步跑构建，避免阻塞 UI；stdout/stderr 累积到 m_bufBuild，结束在 OnBuildFinished 续接。
    if (m_pBuildProc == nullptr) {
        m_pBuildProc = new QProcess(this);
        connect(m_pBuildProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int a_iCode, QProcess::ExitStatus) { OnBuildFinished(a_iCode); });
        connect(m_pBuildProc, &QProcess::readyReadStandardOutput, this, [this]() {
            m_bufBuild += m_pBuildProc->readAllStandardOutput(); });
        connect(m_pBuildProc, &QProcess::readyReadStandardError, this, [this]() {
            m_bufBuild += m_pBuildProc->readAllStandardError(); });
    }
    m_bufBuild.clear();
    m_sLastBuildCmd = sCmd;   // (A) 缓存本轮构建命令，供 OnBuildFinished 写 verdict
    m_pBuildProc->setWorkingDirectory(a_pTask->ProjectDir());
    StageEvent(a_pTask, QStringLiteral("compile_start"), QStringLiteral("▶ 编译校验门：%1").arg(sCmd));
    // sCmd 是完整命令行（可能含引号/重定向），经 cmd /c 执行。
    m_pBuildProc->start(QStringLiteral("cmd.exe"),
                        QStringList() << QStringLiteral("/c") << sCmd);
    if (!m_pBuildProc->waitForStarted(10000)) {
        // (A) 进程都没起来 = 未实际构建，记 skip 态（带启动失败原因）。
        WriteBuildVerdict(a_pTask, QStringLiteral("skip"), 0, m_sLastBuildCmd,
                          m_pBuildProc->errorString());
        StageEvent(a_pTask, QStringLiteral("compile_fail"),
            QStringLiteral("编译门启动失败：%1").arg(m_pBuildProc->errorString()));
        ProceedToNext(a_pTask, QString());
    }
}

void CTaskOrchestrator::OnBuildFinished(int a_iExitCode) {
    CTaskInfo* pTask = m_pCurTask;
    if (pTask == nullptr) {
        return;
    }
    // 停止/暂停时编译门进程被 kill 也会触发本回调——此时不应继续推进，直接吞掉。
    if (pTask->StopRequested() || pTask->Status() == ETaskStatus::Stopped) {
        m_bufBuild.clear();
        return;
    }
    const QString sLog = QString::fromLocal8Bit(m_bufBuild);
    m_bufBuild.clear();
    const QString sTail = sLog.right(4000);

    if (a_iExitCode == 0) {
        // (A) 编译通过：落 pass 态 verdict，下游 CodeReview/Test 据此跳过重复构建。
        WriteBuildVerdict(pTask, QStringLiteral("pass"), 0, m_sLastBuildCmd, QString());
        StageEvent(pTask, QStringLiteral("compile_pass"), QStringLiteral("✔ 编译通过"));
        ProceedToNext(pTask, QString());
        return;
    }
    // 失败：有续跑预算 → 把编译错误喂回 --resume 续跑修复；否则放行下游。
    // (A) 无论续跑还是放行，都落 fail 态 verdict（续跑后若通过，会被下一次门的 pass 覆盖）。
    WriteBuildVerdict(pTask, QStringLiteral("fail"), a_iExitCode, m_sLastBuildCmd, sTail);
    if (pTask->ResumeCount() < MaxResume()) {
        pTask->IncResumeCount();
        StageEvent(pTask, QStringLiteral("compile_fail_retry"),
            QStringLiteral("⚠ 编译失败，错误喂回续跑修复（第 %1/%2 次）")
                .arg(pTask->ResumeCount()).arg(MaxResume()));
        ReRunResume(pTask, QStringLiteral(
            "刚才的改动编译失败。请在根因层面修复这些编译错误，不要重写已正确的部分。\n\n"
            "编译器输出（尾部）：\n%1").arg(sTail));
    } else {
        StageEvent(pTask, QStringLiteral("compile_fail"),
            QStringLiteral("⚠ 编译仍未通过（续跑上限已用尽），放行至代码审查/测试阶段继续把关"));
        ProceedToNext(pTask, QString());
    }
}

QString CTaskOrchestrator::ResolveBuildCommand(CTaskInfo* a_pTask) const {
    if (a_pTask == nullptr) {
        return QString();
    }
    const QString sRoot = a_pTask->ProjectDir();
    // 1) 用户/agent 显式配置优先（最可控）。
    QFile stBt(sRoot + QStringLiteral("/.autodev/build.txt"));
    if (stBt.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString sLine = QTextStream(&stBt).readLine().trimmed();
        if (!sLine.isEmpty()) {
            return sLine;
        }
    }
    // 2) CMake 工程：读 CMakePresets.json 取首个 buildPreset（如 qt6-release），
    //    configure+build 一条命令（RunCompileGate 经 cmd /c 执行，&& 可解析）；
    //    无 presets 则退回通用 cmake -S -B build。本工程自此走 cmake，不再依赖手维护 .sln。
    if (QFileInfo::exists(sRoot + QStringLiteral("/CMakeLists.txt"))) {
        QString sCfgPreset, sBuildPreset;
        QFile stPf(sRoot + QStringLiteral("/CMakePresets.json"));
        if (stPf.open(QIODevice::ReadOnly)) {
            const QJsonDocument stDoc = QJsonDocument::fromJson(stPf.readAll());
            stPf.close();
            const QJsonArray aBuild = stDoc.object()
                .value(QStringLiteral("buildPresets")).toArray();
            if (!aBuild.isEmpty()) {
                const QJsonObject oBP = aBuild.first().toObject();
                sBuildPreset = oBP.value(QStringLiteral("name")).toString();
                sCfgPreset = oBP.value(QStringLiteral("configurePreset")).toString();
            }
        }
        if (!sBuildPreset.isEmpty() && !sCfgPreset.isEmpty()) {
            // buildPreset 已带 configuration（qt6-release→Release），无需再 --config。
            return QStringLiteral("cmake --preset %1 && cmake --build --preset %2")
                .arg(sCfgPreset, sBuildPreset);
        }
        // 无 presets：通用 configure+build（需目标机已装 cmake 且默认 generator 可用）。
        return QStringLiteral("cmake -S \"%1\" -B \"%1/build\" && "
                              "cmake --build \"%1/build\" --config Release").arg(sRoot);
    }
    // 3) 自动探测根目录唯一 .sln（无 CMake 的传统 VS 工程；entryList 只列根目录文件）。
    QDir stRoot(sRoot);
    const QStringList aSln = stRoot.entryList({ QStringLiteral("*.sln") }, QDir::Files);
    if (aSln.size() == 1) {
        const QString sMsbuild = LocateMsBuild();
        if (sMsbuild.isEmpty()) {
            return QString();   // 找不到 msbuild → 跳过，避免假失败
        }
        return QStringLiteral("\"%1\" \"%2\" /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo")
            .arg(sMsbuild, sRoot + QStringLiteral("/") + aSln.first());
    }
    return QString();   // 4) 都没有 → 调用方跳过编译门
}

void CTaskOrchestrator::WriteBuildVerdict(CTaskInfo* a_pTask, const QString& a_sStatus,
                                          int a_iExitCode, const QString& a_sCmd,
                                          const QString& a_sTail) {
    // (A) 编译门结论持久化：落 build_verdict.md，供 CodeReview/Test 的 BuildPrompt 直读，
    //   把「构建已验」的权威信号显式传给评审阶段，消除 agent 自行 grep run.log 推断的动机。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    QString sR = QStringLiteral(
        "# 编译门结论\n\n"
        "- 状态：%1\n- 时间：%2\n- 命令：%3\n")
        .arg(a_sStatus,
             QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
             a_sCmd.isEmpty() ? QStringLiteral("(无)") : a_sCmd);
    if (a_sStatus != QStringLiteral("skip")) {
        sR += QStringLiteral("- 退出码：%1\n").arg(a_iExitCode);
    }
    if (!a_sTail.isEmpty()) {
        sR += QStringLiteral("\n## 日志尾部\n\n```\n%1\n```\n").arg(a_sTail);
    }
    const QString sPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/build_verdict.md");
    QFile stOut(sPath);
    if (stOut.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream(&stOut) << sR;
    }
}

QString CTaskOrchestrator::ReadBuildVerdict(CTaskInfo* a_pTask) const {
    // (A) 读取本任务编译门结论（BuildPrompt 注入 {{BUILD_VERDICT}} 时调用）。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return QString();
    }
    QFile stIn(m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/build_verdict.md"));
    if (!stIn.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QTextStream(&stIn).readAll();
}

QString CTaskOrchestrator::LocateMsBuild() const {
    // 优先用 PATH 中的 msbuild；否则经 vswhere 解析 VS2022 的 MSBuild。
    const QString sFromPath = QStandardPaths::findExecutable(QStringLiteral("msbuild"));
    if (!sFromPath.isEmpty()) {
        return sFromPath;
    }
    QProcess vp;
    vp.start(QStringLiteral("cmd.exe"), QStringList()
        << QStringLiteral("/c")
        << QStringLiteral("\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\""
                          " -latest -products * -requires Microsoft.Component.MSBuild"
                          " -find MSBuild\\**\\Bin\\MSBuild.exe"));
    if (vp.waitForFinished(8000)) {
        const QString sFound = QString::fromUtf8(vp.readAllStandardOutput()).trimmed();
        if (!sFound.isEmpty()) {
            return sFound;
        }
    }
    return QString();   // 未找到 → 调用方据此跳过编译门
}

int CTaskOrchestrator::EstimateDevTurns(CTaskInfo* a_pTask) const {
    // 按 plan.md 的任务数估算开发轮次：任务越多预算越高，但封顶 150、下限 40。
    if (a_pTask == nullptr) {
        return 80;
    }
    QFile stP(a_pTask->ProjectDir() + QStringLiteral("/") +
              QStringLiteral(".autodev/tasks/%1/plan.md").arg(a_pTask->Id()));
    if (!stP.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 80;   // 无 plan（如 bugfix 流或尚未生成）→ 默认
    }
    const QString sPlan = QTextStream(&stP).readAll();
    // 计任务标识：T<数字>（TankWar 风格）或二级/三级标题（##/###）。
    QRegularExpression re(QStringLiteral("(T\\d+|(^|\\n)\\s{0,3}#{2,3}\\s)"));
    int iTasks = 0;
    auto it = re.globalMatch(sPlan);
    while (it.hasNext()) {
        it.next();
        ++iTasks;
    }
    iTasks = qMax(1, iTasks);
    return qBound(40, 40 + 10 * iTasks, 150);
}

bool CTaskOrchestrator::IsResumeEligible(EStage a_eStage) {
    // 仅长运行的代码改写阶段允许 max-turns 续跑（文档/评审阶段短，续跑意义不大）。
    // Verify 编译失败可 --resume 续跑修复（design §3.2.6）。
    return a_eStage == EStage::Develop
        || a_eStage == EStage::BugFix
        || a_eStage == EStage::Test
        || a_eStage == EStage::Verify;
}

bool CTaskOrchestrator::IsLightReviewStage(EStage a_eStage) {
    // (C) 轻量评审：产出是报告/决策文件，max-turns 时降级放行安全（下游 Verify/Test 把关）。
    //   Test 不在内——真验证门，保留 IsResumeEligible 续跑 + Failed。
    switch (a_eStage) {
        case EStage::ReviewReq:
        case EStage::ReviewDesign:
        case EStage::ReviewPlan:
        case EStage::RootCauseVerify:
        case EStage::CodeReview:
            return true;
        default:
            return false;
    }
}

QString CTaskOrchestrator::ReviewResumeHint(CTaskInfo* a_pTask, EStage a_eStage) {
    // (C) 评审 max-turns 续跑提示：阶段感知，催立即落盘结论 + 决策行，停止检索/构建状态排查。
    //   通用「继续完成本阶段工作」对评审=继续检索，会重蹈 grep run.log 覆辙，故必须阶段感知。
    const SStageDef* pDef = FindStage(a_eStage);
    const QString sOut = (pDef != nullptr && !pDef->sOutput.isEmpty())
        ? pDef->sOutput : QStringLiteral("cr.md");
    const QString sPath = QStringLiteral(".autodev/tasks/%1/%2")
                              .arg(a_pTask->Id(), sOut);
    return QStringLiteral(
        "本轮轮次已耗尽。请【立即停止】一切检索、复核与构建状态排查，"
        "把已得到的结论写入 %1，并在文末单独一行明确写出决策：\n"
        "  决策：通过\n或\n  决策：驳回\n"
        "只依据本轮已读到的源码/文档下结论，然后结束。"
        "禁止再去读 run.log / task.json / 构建日志推断构建状态。").arg(sPath);
}

QString CTaskOrchestrator::RejectFeedbackFile(EStage a_eStage) {
    // 2026.08.15 (驳回差异化引导) 生产阶段 → 驳回它的评审产物文件（相对任务目录）。
    //   非生产阶段（评审/验证等）返回空，不注入反馈块。并行评审 fan-in 后同样合并写入该文件。
    switch (a_eStage) {
        case EStage::ConfirmReq:  return QStringLiteral("review_requirements.md");
        case EStage::Design:      return QStringLiteral("review_design.md");
        case EStage::Plan:        return QStringLiteral("review_plan.md");
        case EStage::RootCause:   return QStringLiteral("review_root_cause.md");
        case EStage::Develop:
        case EStage::BugFix:      return QStringLiteral("cr.md");
        default:                  return QString();
    }
}

QString CTaskOrchestrator::IterationStrategyHint(int a_iIter) {
    // 2026.08.15 (驳回差异化引导) 递进式策略：每轮驳回后重跑的 prompt 必须带与上一轮
    //   不同的引导，否则同样的 prompt + 同样的思路 → 同样的产物 → 死循环。
    //   a_iIter = StageIter()（驳回后已自增；1=第 1 次回炉，2=第 2 次，>=3=第 3 次）。
    switch (a_iIter) {
        case 1:
            return QStringLiteral(
                "逐条针对性解决上述驳回项；未被评审质疑的部分保持不动。");
        case 2:
            return QStringLiteral(
                "上一轮沿用了旧思路仍未通过评审。本轮禁止原样重复上一版方案："
                "先在开头列出旧方案的错误假设（评审为什么两次都不满意），"
                "再给出一条明显不同的实现路线，然后按新路线重做。");
        default:
            return QStringLiteral(
                "前两轮均未收敛。本轮放弃大而全：采用最保守、最小改动的可行方案，"
                "主动收缩范围（先保证评审的 critical/major 阻塞项全部解决），"
                "输出前逐条自检评审意见是否全部闭环，宁可保守也不要引入新风险。");
    }
}

bool CTaskOrchestrator::MaybeAutoNameTask(CTaskInfo* a_pTask) {
    // 2026.08.20 (自动命名) 定稿素材源——只读两处：
    //   ① 任务内容：用户原始需求（多行取「信息量最大」的一行：最长且 ≤80 字，退首个
    //      非空行）——任务名首先应反映用户要什么；
    //   ② 工程 guide.md：首个非泛型标题（尾注括号优先）——任务内容为空时兜底。
    //   不再读 requirements.md / intake.md 等阶段产物（模板腔标题，「需求收集 — 任务 xxx」
    //   「用户原始需求」两轮误命名皆源于此）。
    //   统一清洗（去 #/*/_/` 标记与「— 任务 id」装饰）后按「一句话简短描述」口径：
    //   取首子句（不足 10 字顺延到下一子句边界），截断 24 字。SetName 发 sigChanged，
    //   由 CMainWindow 落盘 task.json 并刷新 UI。

    // 泛型素材判定：模板章节名/阶段名/纯元数据，作任务名无信息量，全部跳过。
    static const QStringList aGeneric = {
        QStringLiteral("需求收集"), QStringLiteral("需求概述"), QStringLiteral("需求分析"),
        QStringLiteral("需求规格"), QStringLiteral("需求描述"), QStringLiteral("需求确认"),
        QStringLiteral("需求文档"), QStringLiteral("功能需求"), QStringLiteral("非功能需求"),
        QStringLiteral("验收标准"), QStringLiteral("待澄清"), QStringLiteral("概述"),
        QStringLiteral("概要"), QStringLiteral("总结"), QStringLiteral("背景"),
        QStringLiteral("目标"), QStringLiteral("设计"), QStringLiteral("方案"),
        QStringLiteral("需求"), QStringLiteral("任务"), QStringLiteral("项目"),
        QStringLiteral("requirements"), QStringLiteral("intake"),
        QStringLiteral("overview"), QStringLiteral("summary"),
    };
    // 去模板装饰：标题里常见的「— 任务 <id>」段、编号/条目号前缀（1. / FR2. / AC3.）。
    static const QRegularExpression reDecor(
        QStringLiteral("[\\s]*[—–-]+[\\s]*任务[\\s]*[0-9a-zA-Z\\-]+|[\\b](?:FR|NFR|AC|Q)[\\d]+[\\.]|^[\\s]*[\\d]+[.、][\\s]*"));
    // 注：[\\b] 非词界断言（char class 内无 \b 断言语义），条目号匹配用「行首+编号」已覆盖，
    //   中缀 FR2. 场景由清洗阶段兜底，此处容忍。
    auto isGeneric = [](const QString& a_s) {
        QString s = a_s;
        // 去尾缀阶段标注：需求收集（intake）/ Requirements (Stage 1) 之类。
        static const QRegularExpression reStageTag(
            QStringLiteral("[（(]\\s*(?:intake|stage\\s*\\d+|需求收集|阶段\\s*\\d+)\\s*[)）]\\s*$"),
            QRegularExpression::CaseInsensitiveOption);
        s.remove(reStageTag);
        s.remove(QLatin1Char('.'));
        s = s.trimmed();
        if (s.size() < 4) { return true; }   // 太短必是泛型（如「需求」「概述」）
        const QString sLow = s.toLower();
        if (std::any_of(aGeneric.cbegin(), aGeneric.cend(),
                        [&sLow](const QString& g) { return sLow == g.toLower(); })) {
            return true;
        }
        // 2026.08.20 三次修订：intake.md 的模板章节名（「用户原始需求」「结构化整理」
        //   「2. 动机与背景」等）包含式命中即视为泛型——此前「用户原始需求」被当任务名。
        static const QStringList aGenericSub = {
            QStringLiteral("原始需求"), QStringLiteral("用户需求"), QStringLiteral("结构化整理"),
            QStringLiteral("动机与背景"), QStringLiteral("约束与边界"), QStringLiteral("待澄清"),
            QStringLiteral("想要实现"), QStringLiteral("涉及的模块"), QStringLiteral("结合 guide"),
            QStringLiteral("用户明确"), QStringLiteral("硬性需求"), QStringLiteral("不臆造"),
        };
        return std::any_of(aGenericSub.cbegin(), aGenericSub.cend(),
                           [&s](const QString& g) { return s.contains(g); });
    };

    QString sCandidate;
    // ① 任务内容（用户原始需求）：多行取「信息量最大」的一行（最长且 ≤80 字），单行或
    //   都超长则取首个非空行——首行常是宽泛总起句（如「创建一个网页版小游戏」），其后
    //   的具体描述行更能代表任务内容。
    {
        QString sFirst;
        for (const QString& sLine : a_pTask->UserRequest().split(QLatin1Char('\n'))) {
            const QString s = sLine.trimmed();
            if (s.isEmpty()) { continue; }
            if (sFirst.isEmpty()) { sFirst = s; }
            if (s.size() <= 80 && s.size() > sCandidate.size()) {
                sCandidate = s;
            }
        }
        if (sCandidate.isEmpty()) { sCandidate = sFirst; }
    }
    // ② 工程 guide.md 兜底（任务内容为空时）：首个非泛型标题；标题尾注括号常承载标题
    //   本体（「需求规格 — 任务 xxx（网页版飞机大战）」→「网页版飞机大战」），尾注含
    //   guide/任务/阶段等元词时跳过。
    if (isGeneric(sCandidate)) {
        QFile stGuide(a_pTask->ProjectDir() + QStringLiteral("/.autodev/guide.md"));
        if (stGuide.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString sGuide = QTextStream(&stGuide).readAll();
            static const QRegularExpression reHeading(
                QStringLiteral("(?m)^#{1,3}\\s*(.+?)\\s*$"));
            QRegularExpressionMatchIterator it = reHeading.globalMatch(sGuide);
            static const QRegularExpression reParenTail(
                QStringLiteral("[（(]([^（）()]{4,30})[)）][\\s]*$"));
            while (it.hasNext()) {
                const QString sRaw = it.next().captured(1).trimmed();
                const QRegularExpressionMatch mParen = reParenTail.match(sRaw);
                if (mParen.hasMatch()) {
                    const QString sParen = mParen.captured(1).trimmed();
                    if (!isGeneric(sParen) && !sParen.contains(QStringLiteral("guide"),
                            Qt::CaseInsensitive)) {
                        sCandidate = sParen;
                        break;
                    }
                }
                QString s = sRaw;
                s.remove(reDecor);
                s = s.trimmed();
                if (!isGeneric(s)) { sCandidate = s; break; }
            }
        }
    }

    // 清洗：去 Markdown 强调/代码标记与残留 #/「— 任务 id」装饰，压缩空白；
    //   「一句话简短描述」口径——取首个子句边界（，,。;；：:）之前的短句，首子句不足
    //   10 字则顺延到下一子句边界（「创建一个网页版小游戏：竖版飞机大战」优于只取冒号前），
    //   再截断 24 字。
    sCandidate.remove(reDecor);
    sCandidate = sCandidate.remove(QLatin1Char('#'))
                           .remove(QLatin1Char('*'))
                           .remove(QLatin1Char('`'))
                           .remove(QStringLiteral("__"))
                           .simplified();
    {
        static const QRegularExpression reSep(QStringLiteral("[，,。;；：:]"));
        int iFrom = 0;
        for (;;) {
            const QRegularExpressionMatch mSep = reSep.match(sCandidate, iFrom);
            if (!mSep.hasMatch() || mSep.capturedStart() >= sCandidate.size() - 1) {
                break;   // 无更多子句边界
            }
            const int iLen = mSep.capturedStart();
            if (iLen >= 10 || iLen >= 24) {
                sCandidate = sCandidate.left(iLen).trimmed();
                break;
            }
            iFrom = mSep.capturedStart() + 1;   // 首子句太短：顺延到下一边界
        }
    }
    if (sCandidate.size() > 24) {
        sCandidate = sCandidate.left(24).trimmed();
    }
    if (sCandidate.isEmpty()) {
        return false;   // 无可用素材：保留临时名，标记不清（下次 Intake 重跑再试）
    }

    a_pTask->SetAutoNamed(false);
    a_pTask->SetName(sCandidate);   // 发 sigChanged → task.json 回写 + UI 刷新
    StageEvent(a_pTask, QStringLiteral("auto_named"),
        QStringLiteral("⚙ 任务已自动命名：「%1」").arg(sCandidate));
    return true;
}

bool CTaskOrchestrator::AutoNameNow(CTaskInfo* a_pTask) {
    // 2026.08.20 (Agent 起名)：任务内容（用户原始需求）+ 工程 guide.md 拼提示词，经
    //   adhoc 助手 runner 独立模式（AutoNaming）交 Agent 生成一句话简短名称；结果在
    //   OnAdhocFinished 清洗后 SetName 回填，并发 sigAutoNameDone 供主窗口镜像 DB/刷新。
    //   助手忙/工程未开 → 回退本地启发式 MaybeAutoNameTask（不阻塞命名）。
    if (a_pTask == nullptr) { return false; }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return MaybeAutoNameTask(a_pTask);
    }
    if (m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending())) {
        // 助手被占用（用户提问/压缩/上一次命名中）：本地启发式兜底，避免命名饿死。
        return MaybeAutoNameTask(a_pTask);
    }
    // 素材①：任务内容（用户原始需求，截 2000 字防提示词膨胀）。
    QString sReq = a_pTask->UserRequest().left(2000).trimmed();
    if (sReq.isEmpty()) {
        sReq = QStringLiteral("（用户需求为空，请结合项目指南推断本任务主旨）");
    }
    // 素材②：工程 guide.md（截 4000 字）。
    QString sGuide = m_pPm->ReadGuide().left(4000).trimmed();
    const QString sGuideSec = sGuide.isEmpty()
        ? QStringLiteral("（项目指南为空）")
        : QStringLiteral("以下是当前工程的项目指南（guide.md）：\n---\n%1\n---").arg(sGuide);
    const QString sPrompt = QStringLiteral(
        "【任务命名】请为下面的任务起一个简短的中文任务名称。\n\n"
        "%1\n\n用户的任务需求（任务内容）：\n---\n%2\n---\n\n"
        "命名要求：\n"
        "1. 一句话描述任务要做什么（如「创建一个竖版的飞机大战游戏」），不超过 20 个字；\n"
        "2. 结合项目指南理解任务在工程中的定位，名称要具体、能区分于其他任务；\n"
        "3. 不要包含「任务」「需求」这类泛型词，不要带任务 id；\n"
        "4. 只输出名称本身：不要任何解释、引号、标点结尾或多余文字。")
        .arg(sGuideSec, sReq);
    m_pAutoNameTask = a_pTask;
    m_bAdhocReadOnly = true;    // 纯命名：禁 Bash/Edit/Write
    m_sAdhocPendingPrompt.clear();
    m_eAdhocMode = AutoNaming;
    StageEvent(a_pTask, QStringLiteral("auto_naming"),
        QStringLiteral("⚙ 正在结合任务内容与 guide.md 请 Agent 生成任务名称…"));
    StartAdhocStage(sPrompt);
    return true;
}

void CTaskOrchestrator::RecordSummary(CTaskInfo* a_pTask, EStage a_eStage,
                                      bool a_bOk, const QString& a_sSummary) {
    const QString sName = StageNameZh(a_eStage);
    const QString sLine = QStringLiteral("[%1] %2 | %3 (iter %4) | %5 | %6")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
        .arg(a_pTask->Name())
        .arg(sName)
        .arg(a_pTask->StageIter())
        .arg(a_bOk ? QStringLiteral("OK") : QStringLiteral("FAIL"))
        .arg(a_sSummary.left(160).simplified());
    m_pPm->AppendRecord(sLine);
}

void CTaskOrchestrator::RecordVerifySummary(CTaskInfo* a_pTask, bool a_bOk,
                                            const QString& a_sResult) {
    // FR-1：Verify 阶段在统一行之外额外写一条「验证结论：…」行。
    // 结论来源优先级（FR-1.2）：① verify.md 的「最终验证结论」行；② result；③ m_sLastAssistantText。
    QString sVerdict;

    const SStageDef* pDef = FindStage(EStage::Verify);
    if (pDef != nullptr && !pDef->sOutput.isEmpty()) {
        const QString sPath = a_pTask->ProjectDir() + QStringLiteral("/") +
            QStringLiteral(".autodev/tasks/%1/%2").arg(a_pTask->Id(), pDef->sOutput);
        QFile stFile(sPath);
        if (stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stIn(&stFile);
            const QString sAll = stIn.readAll();
            // 行锚定「最终验证结论」，容忍全/半角冒号与首尾空白（与 ParseReviewDecision 同款正则思路）。
            static const QRegularExpression reVerdict(
                QStringLiteral("(?m)^\\s*最终验证结论\\s*[:：]\\s*(.+?)\\s*$"));
            const QRegularExpressionMatch m = reVerdict.match(sAll);
            if (m.hasMatch()) {
                sVerdict = m.captured(1).trimmed();
            }
        }
    }

    // 回退：结论行 / 文件缺失 → result 为空则用缓存助手文本（取前 160 字）。
    if (sVerdict.isEmpty()) {
        const QString& sSrc = a_sResult.isEmpty() ? m_sLastAssistantText : a_sResult;
        sVerdict = sSrc.left(160).simplified();
    }

    // 去掉 verify.md 模板常见的全角引号「」，避免结论行内容带装饰括号。
    sVerdict.remove(QStringLiteral("「")).remove(QStringLiteral("」"));

    const QString sConclusion = a_bOk
        ? QStringLiteral("通过")
        : QStringLiteral("失败");
    const QString sLine = QStringLiteral("[%1] %2 | 验证 (iter %3) | 验证结论：%4%5")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
        .arg(a_pTask->Name())
        .arg(a_pTask->StageIter())
        .arg(sConclusion)
        .arg(sVerdict.isEmpty() ? QString() : QStringLiteral(" - ") + sVerdict);
    m_pPm->AppendRecord(sLine);   // 唯一写盘路径（FR-1.4）
}

EReviewDecision CTaskOrchestrator::ParseReviewDecision(CTaskInfo* a_pTask,
                                                       EStage a_eReview,
                                                       const QString& a_sRaw) {
    // 决策文本来源优先级：本轮真实输出 > 评审输出文件。
    // 1) 本轮 result + 最后一段助手文本——一定来自本轮 stream，最可靠。
    // 2) 评审输出文件（如 review_requirements.md）——仅当本轮被重写（mtime >= m_dtStageStart）才采信，
    //    否则可能是上一轮迭代遗留的旧「决策：驳回」，直接套用会造成误判（导致任务在输入已就绪后仍被反复驳回）。
    QString sText = a_sRaw + QStringLiteral("\n") + m_sLastAssistantText;

    const SStageDef* pDef = FindStage(a_eReview);
    if (pDef != nullptr && !pDef->sOutput.isEmpty()) {
        const QString sPath = a_pTask->ProjectDir() + QStringLiteral("/") +
                              QStringLiteral(".autodev/tasks/%1/%2").arg(a_pTask->Id(), pDef->sOutput);
        const QFileInfo stFi(sPath);
        // 新鲜度门槛：文件存在且本轮启动后才被写入，才并入决策文本。
        // m_dtStageStart 无效（理论不会）时退化为不读文件，避免误用过期内容。
        if (stFi.exists() && m_dtStageStart.isValid() &&
            stFi.lastModified() >= m_dtStageStart) {
            QFile stFile(sPath);
            if (stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream stIn(&stFile);
                sText += QStringLiteral("\n") + stIn.readAll();
            }
        }
    }

    // 2026.08.12 (修假驳回) 三层决策解析，取代原关键词全文本子串匹配：
    //   原「驳回/reject/不通过/未通过」子串匹配会误判——「无不通过项」「未通过的检查项为零」
    //   这类正面表述也命中 → 假驳回 → 死循环熔断。所有评审模板已要求文末单行「决策：通过|驳回」，
    //   故改为：Tier1 行锚定正则取最后一条决策行（Test append 到 cr.md 时取最后一条=本阶段决策）；
    //   Tier2 仅当无决策行，在末尾 1500 字查「驳回/reject」独立词（不查「不通过/未通过」子串）；
    //   Tier3 默认 Pass（评审未显式驳回即放行）。
    //
    // —— Tier 1：行锚定，取最后一条「决策：xxx」行（容忍全/半角冒号、首尾空白）——
    static const QRegularExpression reDecision(
        QStringLiteral("(?m)^\\s*决策\\s*[:：]\\s*(.+?)\\s*$"));
    QString sLastDecision;
    auto itMatch = reDecision.globalMatch(sText);
    while (itMatch.hasNext()) {
        sLastDecision = itMatch.next().captured(1).trimmed();
    }
    if (!sLastDecision.isEmpty()) {
        const QString sLow = sLastDecision.toLower();
        if (sLow == QStringLiteral("通过") || sLow.contains(QStringLiteral("pass"))) {
            return EReviewDecision::Pass;
        }
        if (sLow == QStringLiteral("驳回") || sLow.contains(QStringLiteral("reject")) ||
            sLow.contains(QStringLiteral("不通过")) || sLow.contains(QStringLiteral("未通过"))) {
            return EReviewDecision::Reject;
        }
        // 决策行存在但值不规范 → 落入 Tier 2 兜底。
    }

    // —— Tier 2：无有效决策行时，仅在末尾 1500 字查显式 reject 词 ——
    //   只匹配「驳回 / reject」独立词，不匹配「不通过/未通过」子串（假阳性源头）。
    const QString sTail = sText.right(1500);
    const QRegularExpression reRejectWord(
        QStringLiteral("驳回|\\breject\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (reRejectWord.match(sTail).hasMatch()) {
        return EReviewDecision::Reject;
    }

    // —— Tier 3：默认通过（评审未显式驳回即放行）——
    return EReviewDecision::Pass;
}

QString CTaskOrchestrator::ExtractRejectDigest(CTaskInfo* a_pTask) {
    // 取最近一次评审输出(cr.md)的阻塞摘要：去掉决策行后的正文尾部 ~300 字。
    // 仅供驳回/熔断事件文本带出，让 UI 一眼可见上轮驳了什么（不参与决策判定）。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return QString();
    }
    const QString sPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/cr.md");
    if (!QFileInfo::exists(sPath)) {
        return QString();
    }
    QFile stFile(sPath);
    if (!stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QString sBody = QTextStream(&stFile).readAll();
    // 去掉最后的「决策：xxx」行，只保留问题正文。
    static const QRegularExpression reDecision(
        QStringLiteral("(?m)^\\s*决策\\s*[:：].*$"));
    sBody.remove(reDecision);
    return sBody.right(300).simplified();
}

void CTaskOrchestrator::WriteEscalationReport(CTaskInfo* a_pTask, EStage a_eBack) {
    // 熔断时把驳回焦点 + 最近 5 条阶段历史写进 escalation.md，供用户接管决策。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const QString sDigest = ExtractRejectDigest(a_pTask);
    const QList<CTaskInfo::SStageRun>& aHist = a_pTask->History();
    const int iStart = (aHist.size() > 5) ? (aHist.size() - 5) : 0;

    QString sReport;
    sReport += QStringLiteral("# 升级报告：迭代熔断\n\n");
    sReport += QStringLiteral("- 任务：%1（%2）\n").arg(a_pTask->Name(), a_pTask->Id());
    sReport += QStringLiteral("- 回退目标阶段：%1\n").arg(StageNameZh(a_eBack));
    sReport += QStringLiteral("- 已迭代次数：%1（上限 %2）\n")
                   .arg(a_pTask->StageIter()).arg(MaxIter());
    sReport += QStringLiteral("\n## 本轮驳回焦点\n\n%1\n").arg(sDigest);
    sReport += QStringLiteral("\n## 最近 %1 条阶段历史\n\n").arg(aHist.size() - iStart);
    for (int i = iStart; i < aHist.size(); ++i) {
        const CTaskInfo::SStageRun& r = aHist[i];
        sReport += QStringLiteral("- [%1] iter=%2 %3 — %4\n")
                       .arg(StageNameZh(r.eStage)).arg(r.iIter)
                       .arg(r.bOk ? QStringLiteral("OK") : QStringLiteral("FAIL"))
                       .arg(r.sSummary.left(120).simplified());
    }
    sReport += QStringLiteral(
        "\n## 建议\n\n评审与开发未能在 %1 次迭代内收敛。请人工查看上方驳回焦点，"
        "决定：① 直接修改代码后重跑；② 放宽验收标准；③ 调整任务描述。\n")
        .arg(MaxIter());

    const QString sOutPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/escalation.md");
    QFile stOut(sOutPath);
    if (stOut.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream(&stOut) << sReport;
    }
}

bool CTaskOrchestrator::IsReviewStage(EStage a_eStage) {
    switch (a_eStage) {
        case EStage::ReviewReq:
        case EStage::ReviewDesign:
        case EStage::ReviewPlan:
        case EStage::RootCauseVerify:
        case EStage::CodeReview:
        case EStage::Test:
            return true;
        default:
            return false;
    }
}

int CTaskOrchestrator::MaxTurnsFor(CTaskInfo* a_pTask, EStage a_eStage) const {
    switch (a_eStage) {
        case EStage::Develop:    return EstimateDevTurns(a_pTask);   // 按 plan.md 任务数动态给
        case EStage::CodeReview: return 40;   // (F) 审查多文件需更多预算；(C) 降级兜底防判死
        case EStage::BugFix:
        case EStage::Test:
        case EStage::Verify:   return 80;                          // 验证：插调试+编译+读日志+视觉+清理，同 BugFix/Test 档
        default:
            // 2026.08.15 (评审瘦身+流程档位) 轻量评审轮次按档位收紧：穷尽式核查交给 CodeReview，
            //   轻评审只做结构/风险判断（原全档 30 → 标准 18/简单 12；严格档保持 30 不变）。
            if (IsLightReviewStage(a_eStage) && a_pTask != nullptr) {
                switch (a_pTask->Tier()) {
                    case ETier::Simple:   return 12;
                    case ETier::Standard: return 18;
                    case ETier::Strict:   return 30;
                }
            }
            return 30;
    }
}

QString CTaskOrchestrator::MinorityFeedbackBlock(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    // 前一阶段（生效流内）是评审阶段且其产物含「## 少数派驳回意见」标记 → 抽取该段返回。
    //   标记段由 FanInReview 在多数决 Pass 带意见时写入；抽取到下一个 ## 标题或决策行为止。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return QString();
    }
    const QList<EStage> aFlow = TaskFlow(a_pTask);
    const std::optional<EStage> oPrev = CStageMachine::PrevStage(aFlow, a_stDef.eStage);
    if (!oPrev.has_value() || !IsReviewStage(*oPrev)) {
        return QString();
    }
    const SStageDef* pPrevDef = FindStage(*oPrev);
    if (pPrevDef == nullptr || pPrevDef->sOutput.isEmpty()) {
        return QString();
    }
    const QString sPath = m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/")
                         + pPrevDef->sOutput;
    QFile stFile(sPath);
    if (!stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    const QString sText = QTextStream(&stFile).readAll();
    static const QString sMarker = QStringLiteral("## 少数派驳回意见");
    const int iPos = sText.indexOf(sMarker);
    if (iPos < 0) {
        return QString();
    }
    // 截到下一个顶级标题或决策行（标记段之后的内容即各驳回视角正文引述）
    static const QRegularExpression reEnd(QStringLiteral("(?m)^(?:## |决策\\s*[:：])"));
    int iLen = sText.size() - iPos;
    auto itMatch = reEnd.globalMatch(sText, iPos + sMarker.size());
    if (itMatch.hasNext()) {
        iLen = itMatch.next().capturedStart() - iPos;
    }
    return sText.mid(iPos, iLen).trimmed();
}

QString CTaskOrchestrator::StageNameZh(EStage a_eStage) const {
    const SStageDef* pDef = FindStage(a_eStage);
    return pDef ? pDef->sNameZh : QString::number(static_cast<int>(a_eStage));
}

void CTaskOrchestrator::WriteRunLog(CTaskInfo* a_pTask, const QString& a_sCategory,
                                    const QString& a_sText) {
    // 每条流水带秒级时间戳 + 分类，落任务私有 run.log（#6 完整日志归档，便于复盘改进）。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const QString sLine = QStringLiteral("[%1] [%2] %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
        .arg(a_sCategory)
        .arg(a_sText);
    m_pPm->AppendTaskLog(a_pTask->Id(), sLine);
}

void CTaskOrchestrator::StageEvent(CTaskInfo* a_pTask, const QString& a_sKind,
                                   const QString& a_sText) {
    // 统一入口：阶段事件先落 run.log 再发信号，避免每个 emit 处重复写日志。
    WriteRunLog(a_pTask, a_sKind, a_sText);
    emit sigStageEvent(a_pTask, a_sKind, a_sText);
}

bool CTaskOrchestrator::EnsureStageInputs(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    // #5 阶段输入自愈：逐个检查声明输入是否存在；缺失则回退到生产该文件的上游阶段重跑，
    // 而不是让评审/消费阶段反复报「缺文件」空耗轮次。生产阶段在流程上必先于消费阶段，
    // 回退安全；计入 StageIter 复用既有升级机制防死循环。
    if (a_pTask == nullptr) {
        return true;
    }
    const QString sTaskDir = QStringLiteral(".autodev/tasks/%1").arg(a_pTask->Id());
    const QString sRoot = a_pTask->ProjectDir();

    // 2026.08.15 (流程档位) 输入按生效流过滤：产物阶段未勾选的输入不检查、不回退
    //   （如简单档 Develop 不再要求 plan.md/design.md），否则会弹回未勾选阶段空跑一轮。
    const QStringList aInputs = FilterInputs(a_stDef, TaskFlow(a_pTask));
    for (const QString& sInput : aInputs) {
        // 输入可能是任务私有目录下（requirements.md）或工程级（.autodev/guide.md）
        const QString sRel = sInput.startsWith(QStringLiteral(".autodev/"))
            ? sInput
            : (sTaskDir + QStringLiteral("/") + sInput);
        const QString sPath = QDir::cleanPath(sRoot + QStringLiteral("/") + sRel);
        if (QFileInfo::exists(sPath)) {
            continue;   // 存在，检查下一个
        }

        // 缺失：反查生产该文件的上游阶段（sOutput == sInput）
        EStage eProducer = EStage::Intake;
        bool bFound = false;
        for (const SStageDef& sDef : GetStageDefs()) {
            if (sDef.sOutput == sInput) {
                eProducer = sDef.eStage;
                bFound = true;
                break;
            }
        }
        if (!bFound) {
            continue;   // 无生产者（如 guide.md 由 BuildGuideAsync 生成），不在此处理
        }

        // 计入迭代次数；超上限则升级到用户，避免回退死循环
        a_pTask->IncStageIter();
        if (a_pTask->StageIter() > MaxIter()) {
            a_pTask->SetCurrentStage(eProducer);
            a_pTask->SetPauseRequested(true);
            StageEvent(a_pTask, QStringLiteral("escalation"),
                QStringLiteral("⚠ 多次回退仍未生成 %1，已暂停并升级到用户处理").arg(sInput));
            return false;
        }

        a_pTask->SetCurrentStage(eProducer);
        const SStageDef* pProdDef = FindStage(eProducer);
        StageEvent(a_pTask, QStringLiteral("need_input"),
            QStringLiteral("⚠ 缺少 %1，回退到「%2」重新生成")
                .arg(sInput, pProdDef ? pProdDef->sNameZh : QString()));
        QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
        return false;   // 已安排重跑，调用方应立即返回
    }
    return true;   // 所有声明输入就绪
}

// ---------------------------------------------------------------------------
// 提示词 agent（独立于状态机的常驻助手会话，#1/#2/#3/#4）
//   - 第二个 CClaudeRunner 实例 m_pAdhocRunner，独立 QProcess，与任务 runner 严格隔离。
//   - 输出经 sigAdhoc* 路由到「助手」Tab，不触碰 m_pCurTask、不调 Advance。
//   - 撤销：每次提问 --fork-session 产生新 sid 分支（实测不污染旧 sid），栈存 (prompt,sid)，撤销=弹栈。
//   - 压缩：/compact 在 headless 无效 → 「摘要→开新会话重播种」两步 + --autocompact 150k 自动兜底。
// ---------------------------------------------------------------------------
void CTaskOrchestrator::EnsureAdhocRunner() {
    // 2026.08.18 后端漂移重建：换后端后旧 runner 的会话链（sid/撤销栈）跨后端无
    //   意义——非占用时整组清（runner + m_sAdhocSid + 撤销栈 + guide 喂入基线），
    //   回到首条新会话状态；占用中本次仍用旧后端。
    if (m_pAdhocRunner != nullptr) {
        if (m_pAdhocRunner->BackendName() == CAgentRunnerFactory::BackendName()) {
            return;
        }
        if (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending()) {
            return;
        }
        m_pAdhocRunner->deleteLater();
        m_pAdhocRunner = nullptr;
        m_sAdhocSid.clear();
        m_aAdhocStack.clear();
        m_sLastGuideHash.clear();
        m_aAdhocPendingInjects.clear();   // 2026.08.19 会话链整体作废，排队指令随行
    }
    m_pAdhocRunner = CAgentRunnerFactory::Create(this);
    // 连 ad-hoc 专属槽（不复用 OnRunner*，与任务 runner 隔离）。
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigInfo,
            this, &CTaskOrchestrator::OnAdhocInfo);
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigAssistantText,
            this, &CTaskOrchestrator::OnAdhocAssistantText);
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigToolUse,
            this, &CTaskOrchestrator::OnAdhocToolUse);
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigToolResult,
            this, &CTaskOrchestrator::OnAdhocToolResult);
    // 运行中上下文占用（每条 assistant 消息的 usage）→ 直通助手面板进度条实时刷新。
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigContextFill,
            this, &CTaskOrchestrator::sigAdhocContext);
    // 2026.08.27 助手轮次实时计量：本轮累计 token 直通面板（Claude Code 式直观反馈）。
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigUsageDelta, this,
            [this](qint64 a_iIn, qint64 a_iOut) {
                m_iLiveIn += a_iIn;
                m_iLiveOut += a_iOut;
                emit sigLiveUsage(m_iLiveIn, m_iLiveOut);
            });
    // 流式实时刷新（绝对值直通显示，不入账）：与上一连接共展 sigLiveUsage。
    connect(m_pAdhocRunner, &CAgentRunnerBase::sigLivePartial, this,
            &CTaskOrchestrator::sigLiveUsage);
    connect(m_pAdhocRunner,
            qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                &CAgentRunnerBase::sigFinished),
            this, &CTaskOrchestrator::OnAdhocFinished);
}

void CTaskOrchestrator::StartAdhocStage(const QString& a_sPrompt) {
    EnsureAdhocRunner();
    ResetLiveUsage();   // 2026.08.27 新轮次开始：实时计量清零
    const QString sWorkDir = m_pPm ? m_pPm->ProjectDir() : QString();
    // deny 黑名单（bypass 下 allow 失效、deny 生效）：readOnly=只读分析禁 Bash/Edit/Write；
    //   非 readOnly=ToolsDev() 现返回空串=全开。
    const QString sTools = m_bAdhocReadOnly
        ? QStringLiteral("Bash,Edit,Write")
        : ToolsDev();
    // fork 链：有当前 sid 则 --resume + --fork-session 产生新 sid 分支，旧 sid 保持不污染
    //   （实测确认 → #4 O(1) 撤销可行）；无 sid（首条提问 / CompactReseed 前已清空）则纯新建会话。
    const bool bFork = !m_sAdhocSid.isEmpty();
    // 2026.08.15 autocompact 比例制：K（千 token）= 窗口 × [Adhoc] AutoCompactPct ÷ 100 ÷ 1000，
    //   窗口优先 [Adhoc] ContextLimit 用户覆盖、未设按助手最近所用模型探测（替代旧千 token 档位）。
    const int iAutoCompactK = m_pPm
        ? m_pPm->AutoCompactKFor(m_pAdhocRunner->LastModel()) : 150;
    m_dtAdhocStart = QDateTime::currentDateTime();   // 2026.08.15 耗时归集：本次调用起点
    m_pAdhocRunner->StartStage(sWorkDir, a_sPrompt, sTools, 40,
                               m_sAdhocSid, bFork, iAutoCompactK, /*a_bEnableAskUser=*/true);
    emit sigAdhocRunning();   // 2026.08.19 面板进运行态（手动发送已自置，此处兜自动续发/压缩）
}

int CTaskOrchestrator::MaxIter() const {
    // 2026.08.15 [Policy] MaxIter 现读（设置保存即生效）；m_pPm 为空回退默认 3。
    return (m_pPm != nullptr) ? m_pPm->LoadMaxIter() : 3;
}

int CTaskOrchestrator::MaxResume() const {
    // 2026.08.15 [Policy] MaxResume 现读；m_pPm 为空回退默认 3。
    return (m_pPm != nullptr) ? m_pPm->LoadMaxResume() : 3;
}

int CTaskOrchestrator::ReviewResume() const {
    // 2026.08.15 [Policy] ReviewResume 现读；m_pPm 为空回退默认 1。
    return (m_pPm != nullptr) ? m_pPm->LoadReviewResume() : 1;
}

void CTaskOrchestrator::InjectTaskMessage(CTaskInfo* a_pTask, const QString& a_sText) {
    // FR-3：确定性 C++ 路由（NFR-7，不让 LLM 充当路由器）。
    //   Running → InjectUserTurn + injected 事件（可见反馈，AC-3.1）；非 Running → inject_ignored（AC-3.3）。
    if (a_pTask == nullptr || a_sText.trimmed().isEmpty()) {
        return;
    }
    const bool bRunning = (a_pTask == m_pCurTask &&
                          m_pRunner != nullptr && m_pRunner->IsRunning());
    if (!bRunning) {
        // FR-3.4：非 Running 明确提示，不静默丢弃、无副作用。
        StageEvent(a_pTask, QStringLiteral("inject_ignored"),
            QStringLiteral(" 任务未运行，已忽略介入指令（请先启动任务）"));
        return;
    }
    const bool bOk = m_pRunner->InjectUserTurn(a_sText);
    if (bOk) {
        // claude 后端：stream-json stdin 轮中追加 user 帧，当前轮立即收到新指令。
        StageEvent(a_pTask, QStringLiteral("injected"),
            QStringLiteral(" 已注入用户指令（当前轮立即生效）：%1").arg(a_sText.left(120)));
        return;
    }
    // 2026.08.19 DSH 后端协议不支持轮中插话：排队到下一阶段 prompt 前置注入
    //   （同任务有效；换任务/停止即作废，见 LaunchStage / Reset）。
    m_aPendingTaskInjects.append(a_sText);
    StageEvent(a_pTask, QStringLiteral("injected"),
        QStringLiteral(" 已排队用户指令（本阶段结束后的下一阶段注入）：%1")
            .arg(a_sText.left(120)));
}

void CTaskOrchestrator::SubmitAdhoc(const QString& a_sText, bool a_bReadOnly, const QString& a_sTaskId) {
    // 2026.08.19 运行中追加指令（含 Claude Code / DSH 两后端）：
    //   - 真运行中（IsRunning）：claude 后端 InjectUserTurn 即时轮中注入（stream-json
    //     stdin 追加 user 帧，当前轮立即看到）；DSH 后端协议不支持轮中插话 → 排队，
    //     本轮成功收尾后自动按同会话续发（OnAdhocFinished 消费队列）。
    //   - 崩溃宽限/重试等待中：runner 被占用但无稳定会话可注入，维持「请稍候」。
    if (m_pAdhocRunner != nullptr && m_pAdhocRunner->IsRunning()) {
        // 轮中介入（方案 §2.3）：非只读文本注入前同口径拍 turn 快照（失败不阻断）。
        if (!a_bReadOnly && !a_sText.trimmed().isEmpty() && m_pPm != nullptr
            && !m_pPm->ProjectDir().isEmpty()) {
            const QString sOwner = a_sTaskId.isEmpty()
                ? QStringLiteral("global") : a_sTaskId;
            // 2026.08.26 turn 快照改后台（大工程 UI 卡死修复，见 AsyncAdhocBackup）。
            AsyncAdhocBackup(sOwner, a_sText);
        }
        if (!a_sText.trimmed().isEmpty()
            && m_pAdhocRunner->InjectUserTurn(a_sText)) {
            emit sigAdhocEvent(QStringLiteral("injected"),
                QStringLiteral("已向正在执行的助手轮注入追加指令（立即生效）：%1")
                    .arg(a_sText.left(120)));
        } else if (!a_sText.trimmed().isEmpty()) {
            m_aAdhocPendingInjects.append(a_sText);
            emit sigAdhocEvent(QStringLiteral("queued"),
                QStringLiteral("已排队追加指令（本轮结束后自动发送）：%1")
                    .arg(a_sText.left(120)));
        }
        return;
    }
    if (m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsGraceActive() || m_pAdhocRunner->IsRetryPending())) {
        // 2026.08.15 忙判定补崩溃宽限/重试等待中：此时 IsRunning()==false 但 runner 仍被占用，
        //   放行会在宽限轮询/延迟重拉落地时与新提问互相踩。
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("助手正在处理上一条指令，请稍候"));
        return;
    }
    if (a_sText.trimmed().isEmpty()) {
        return;
    }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("请先打开一个工程再使用助手"));
        return;
    }
    m_bAdhocReadOnly = a_bReadOnly;
    m_sAdhocLastTaskId = a_sTaskId;   // 2026.08.19 自动续发排队指令时沿用同任务档（记忆注入）
    m_iAdhocResume = 0;   // 2026.08.15 新提问：max-turns 自动续跑计数清零
    m_eAdhocMode = Submitting;
    m_sAdhocPendingPrompt = a_sText;   // 原始提问入栈（注入内容不进撤销栈，保持用户可见语义）

    // guide.md 投喂策略（需求3）：仅「首次新建会话」或「guide.md 自上次投喂后发生变化」时注入，
    //   避免每次续问都重复投喂冗长指南；用内容 SHA256 判变，哈希随 assistant.json 持久化。
    //   即：初始化投喂 → 后续打开工程仅恢复上下文不投喂 → guide.md（重新生成 / 同步 agent 改动）后自动重投。
    QString sPrompt = a_sText;
    const QString sGuide = m_pPm->ReadGuide();
    if (!sGuide.isEmpty()) {
        const QString sHash = QString::fromLatin1(
            QCryptographicHash::hash(sGuide.toUtf8(), QCryptographicHash::Sha256).toHex());
        const bool bNeedFeed = m_sAdhocSid.isEmpty() || sHash != m_sLastGuideHash;
        if (bNeedFeed) {
            m_sLastGuideHash = sHash;
            emit sigAdhocStateChanged();   // 哈希变化 → 存盘 assistant.json
            const bool bFirst = m_sAdhocSid.isEmpty();
            if (bFirst) {
                // 首次新建会话：仍全文投喂（保证模型一定拿到工程认知，一次性成本）。
                sPrompt = QStringLiteral(
                    "以下是当前工程的项目指南（guide.md）。请先阅读以了解项目基本情况，再回答用户问题："
                    "\n---\n%1\n---\n\n\n用户问题：%2").arg(sGuide, a_sText);
            } else {
                // 2026.08.27 变更重投瘦身（大工程助手每步极慢主因之一）：旧实现把整份新
                //   guide.md 塞进 user 消息——大工程几十~上百 KB，且内容每轮都变缓存
                //   救不了，每轮多付一次大 prefill。改为只投一句话让 agent 用 Read 自取：
                //   读多少缓存多少，与 Claude Code 的按需探索同口径。
                sPrompt = QStringLiteral(
                    "【项目指南已更新】.autodev/guide.md 自上次阅读后有变化，"
                    "请先用 Read 工具读取 .autodev/guide.md 同步项目认知，"
                    "再处理用户问题。\n\n\n用户问题：%1").arg(a_sText);
            }
        }
    }
    // 需求4：非只读会话强制 agent 自治写 record.md——实际修改源码后必须追加一行，
    //   作为 guide.md 增量同步的触发信号与输入（sTools 已放行 Write/Edit）。每轮非只读都提醒，
    //   不依赖 guide 投喂周期。必须用 Edit 追加（先 Read 取末尾再 Edit），严禁 Write 覆盖 record.md。
    if (!a_bReadOnly) {
        sPrompt.append(QStringLiteral(
            "\n\n---\n【强制记录规则】如果你在本次回答中实际修改了工程源码文件，结束前必须向 "
            "`.autodev/record.md` **追加**恰好一行，格式："
            "`[yyyy-MM-dd HH:mm] 助手 | <简述改了哪些文件/做了什么>`（时间用 `date` 命令取当前时刻）。"
            "必须用 Edit 工具在文件末尾追加（先 Read 再 Edit 末尾行），**严禁用 Write 覆盖整个 record.md**。"
            "纯查询/只读操作不要写。此记录是项目指南（guide.md）自动同步的依据。"));
    }
    // 2026.08.15 (记忆跟着任务走) 选中任务时注入其 memory.md 经验；非只读轮附追加规则
    //   （与上面 record.md 规则同款句式：Edit 末尾追加、严禁 Write 覆盖）。
    //   只读轮有记忆输入、无写入规则；无任务（全局助手档）两者皆不注入。
    if (!a_sTaskId.isEmpty() && m_pPm != nullptr) {
        const QString sMemory = m_pPm->ReadTaskMemory(a_sTaskId);
        if (!sMemory.isEmpty()) {
            sPrompt.append(QStringLiteral(
                "\n\n---\n【任务经验记忆】以下是当前任务（id=%1）此前沉淀的经验"
                "（`.autodev/tasks/%1/memory.md`），动手前请先参考：\n%2\n")
                .arg(a_sTaskId, sMemory));
        }
        if (!a_bReadOnly) {
            sPrompt.append(QStringLiteral(
                "\n【强制经验沉淀规则】若本次你实际修改了工程源码或有了新发现"
                "（实际可用的构建命令、踩坑及解法、关键文件路径），结束前必须向 "
                "`.autodev/tasks/%1/memory.md` **末尾追加**一条经验，格式："
                "`- [助手] <一条经验>`。必须先 Read 再用 Edit 在末尾追加"
                "（文件不存在时允许首次用 Write 创建）；**严禁用 Write 覆盖整个文件**。"
                "纯查询/只读操作不要写。").arg(a_sTaskId));
        }
    }
    // —— 统一备份（方案 §6.1/§6.3）：非只读轮投递前同步快照 + 回退告知注入 ——
    if (!a_bReadOnly) {
        const QString sOwner = a_sTaskId.isEmpty() ? QStringLiteral("global") : a_sTaskId;
        int iNoticeTarget = 0;
        QString sNoticeOwner;
        // 2026.08.22 owner 校验：notice 只由同 owner（任务/global）的下一轮消费，
        // 防跨档误吞（任务侧 --resume 续跑仍能看到自己的告知）。
        if (m_pPm->LoadPendingNotice(iNoticeTarget, sNoticeOwner)
            && sNoticeOwner == sOwner) {
            sPrompt.prepend(QStringLiteral(
                "【系统回退通知】工作目录已回退到版本 #%1：此前对话中描述的文件修改已不存在于磁盘。"
                "请以当前磁盘内容为准，不要基于已被回退的修改继续工作。\n---\n").arg(iNoticeTarget));
            m_pPm->ClearPendingNotice();
        }
        // 2026.08.26 turn 快照改后台（大工程 UI 卡死修复，见 AsyncAdhocBackup）：
        //   异步快照不阻塞提问；失败仅事件提示。回退告知注入（上方）仍同步（只读 notice.json，轻量）。
        AsyncAdhocBackup(sOwner, a_sText);
    }
    StartAdhocStage(sPrompt);
}

void CTaskOrchestrator::UndoAdhoc() {
    // 2026.08.23 守卫补崩溃宽限/重试等待（与 Compact/Submit 同口径）：此时 IsRunning()
    //   == false 但 runner 仍被占用，放行撤销会在宽限轮询/重试落地时弹栈换基点，
    //   与仍在途的本轮收尾（压栈/事件回放）互踩出孤儿块。
    if (m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending())) {
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("助手运行中，无法撤销"));
        return;
    }
    m_aAdhocPendingInjects.clear();   // 2026.08.19 撤销回退链：未发出的排队指令一并作废
    if (m_aAdhocStack.isEmpty()) {
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("没有可撤销的提问"));
        return;
    }
    m_aAdhocStack.removeLast();
    // 回退到父 sid：栈空则回到无会话态，否则取新栈顶的 sid 作为链基点。
    m_sAdhocSid = m_aAdhocStack.isEmpty() ? QString() : m_aAdhocStack.last().second;
    emit sigAdhocStateChanged();   // #1 撤销链变化 → 存盘
    emit sigAdhocUndo();
    // 撤销后无法精确还原 fill：栈空显示 0，否则置 -1（面板显示「—」），下次提问刷新。
    emit sigAdhocContext(m_aAdhocStack.isEmpty() ? 0 : -1);
}

// —— 助手会话链持久化（#1）：sid + (prompt,sid) 撤销栈的存取 ——
QJsonArray CTaskOrchestrator::AdhocStackToJson() const {
    QJsonArray aArr;
    for (const QPair<QString, QString>& pr : m_aAdhocStack) {
        QJsonObject o;
        o[QStringLiteral("prompt")] = pr.first;
        o[QStringLiteral("sid")]    = pr.second;
        aArr.append(o);
    }
    return aArr;
}

void CTaskOrchestrator::LoadAdhocState(const QString& a_sSid, const QJsonArray& a_aStack) {
    // 恢复（工程打开时一次性调用）：直接覆盖 sid/栈，不发信号（避免回写盘）。
    m_sAdhocSid = a_sSid;
    m_aAdhocStack.clear();
    for (const QJsonValue& v : a_aStack) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            m_aAdhocStack.append({ o.value(QStringLiteral("prompt")).toString(),
                                   o.value(QStringLiteral("sid")).toString() });
        }
    }
}

void CTaskOrchestrator::CompactAdhoc() {
    // 2026.08.15 占用判定含宽限/重试等待（与 SubmitAdhoc 同口径）。
    if (m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending())) {
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("助手运行中，无法压缩"));
        return;
    }
    if (m_sAdhocSid.isEmpty()) {
        emit sigAdhocEvent(QStringLiteral("warn"),
            QStringLiteral("还没有对话，无需压缩"));
        return;
    }
    // /compact 在 headless -p 无效 → 走「摘要→开新会话重播种」两步。
    // 第一步：在当前 sid 的 fork 分支上生成摘要（fork 不污染原 sid），result 作摘要。
    m_eAdhocMode = CompactSummarize;
    emit sigAdhocEvent(QStringLiteral("compact_start"),
        QStringLiteral("⏳ 正在总结对话以压缩上下文…"));
    StartAdhocStage(QStringLiteral(
        "请用约 300 token 精炼总结我们这次对话至今的内容，用于在新会话中恢复上下文。"
        "只输出总结正文，结构为：1) 用户意图与关键决定；2) 已查看/改动的文件；3) 待办与注意事项。"));
}

void CTaskOrchestrator::StopAdhoc() {
    // #4 用户主动停止：先发「已停止」事件（⏹）再 kill 进程，让助手流留下停止痕迹。
    // 2026.08.15 崩溃宽限期（IsGraceActive）一并终止；重试等待中（IsRetryPending）也须 Stop()
    //   清掉延迟重拉标志，否则停止后 5 秒被 RelaunchStage 复活。
    if (m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending())) {
        emit sigAdhocEvent(QStringLiteral("stopped"),
            QStringLiteral("⏹ 已停止"));
        m_pAdhocRunner->Stop();
    }
    m_eAdhocMode = Idle;
    m_sAdhocPendingPrompt.clear();
    emit sigAdhocFinished();
}

bool CTaskOrchestrator::IsAdhocRunning() const {
    // 2026.08.15 占用判定含宽限/重试等待：此时进程未跑但 runner 仍被占用（UI 停止按钮等依此判定）。
    return m_pAdhocRunner != nullptr
        && (m_pAdhocRunner->IsRunning() || m_pAdhocRunner->IsGraceActive()
            || m_pAdhocRunner->IsRetryPending());
}

void CTaskOrchestrator::OnAdhocInfo(const QString& a_sLine) {
    // 过滤 system 噪音（与任务日志一致），其余原样上抛「助手」Tab。
    if (a_sLine.startsWith(QStringLiteral("[system"))) {
        return;
    }
    emit sigAdhocLog(a_sLine);
}

void CTaskOrchestrator::OnAdhocAssistantText(const QString& a_sText) {
    emit sigAdhocAssistantText(a_sText);
}

void CTaskOrchestrator::OnAdhocToolUse(const QString& a_sTool, const QString& a_sInput) {
    emit sigAdhocToolUse(a_sTool, a_sInput);
}

void CTaskOrchestrator::OnAdhocToolResult(const QString& a_sResult) {
    emit sigAdhocToolResult(a_sResult);
}

void CTaskOrchestrator::OnAdhocFinished(bool a_bOk, const QString& a_sResult,
                                        qint64 a_iIn, qint64 a_iOut,
                                        double a_dCost, const QString& a_sSid) {
    // 2026.08.15 助手消耗归集：成功调用即上报（提问/压缩两步都算助手消耗，输入已含缓存读/写）。
    //   归集到哪个任务由 CMainWindow 按当前档决定（编排器不持有 UI 选中态）。
    // 2026.08.29 修「助手输入/输出恒 0」：runner 本轮实时上抛过 sigUsageDelta 时收尾
    //   sigFinished 报 0/0（防任务侧双计），助手独立流量只能靠下面的回退用
    //   m_iLiveIn/m_iLiveOut 归集——旧序 ResetLiveUsage() 在函数顶部先行清零，回退
    //   条件 (m_iLiveIn>0||m_iLiveOut>0) 恒假，sigAdhocUsage(0,0,…) → 面板
    //   SAsst::iInTok/iOutTok 永不增长（未选任务时底部「输入:0 输出:0」）。注释
    //   「先取值后清」与实现相反，现为真·先取值后清零（同函数内延后几行，
    //   「本轮 ↑↓」标签收起时机无可感知差异）。
    if (a_bOk) {
        const qint64 iElapsedMs = m_dtAdhocStart.isValid()
            ? m_dtAdhocStart.msecsTo(QDateTime::currentDateTime()) : 0;
        // 2026.08.27 runner 已逐消息实时上抛（sigUsageDelta）时收尾报 0/0（防任务侧双计），
        //   此处回退用本轮实时累计值归集——助手消耗统计不丢失。
        qint64 iIn = a_iIn, iOut = a_iOut;
        if (iIn == 0 && iOut == 0 && (m_iLiveIn > 0 || m_iLiveOut > 0)) {
            iIn = m_iLiveIn;
            iOut = m_iLiveOut;
        }
        emit sigAdhocUsage(iIn, iOut, a_dCost, iElapsedMs,
                           m_pAdhocRunner ? m_pAdhocRunner->LastReqCount() : 0);
    }
    ResetLiveUsage();   // 2026.08.29 归集完成后才清零（顺带 sigLiveUsage(0,0) 收起「本轮」显示）
    m_dtAdhocStart = QDateTime();

    switch (m_eAdhocMode) {
    case AutoNaming: {
        // 2026.08.20 (Agent 起名)：只取结果首行清洗为名称——不动用户会话栈、不推进
        //   m_sAdhocSid（fork 出的分支一次性使用）、不触发 guide 增量同步。
        CTaskInfo* pTask = m_pAutoNameTask;
        m_pAutoNameTask = nullptr;
        m_eAdhocMode = Idle;
        m_sAdhocPendingPrompt.clear();
        emit sigAdhocFinished();   // 助手面板退出运行态（命名过程会短暂占用输入栏）
        if (pTask == nullptr) { break; }
        if (!a_bOk) {
            StageEvent(pTask, QStringLiteral("auto_name_failed"),
                QStringLiteral("✗ Agent 自动命名失败：%1（任务名保持不变）")
                    .arg(a_sResult.left(120)));
            emit sigAutoNameDone(pTask, false, QString());
            break;
        }
        // 清洗：首行 → 去引号/书名号/Markdown 标记/空白 → 截 24 字。
        QString sName = a_sResult.section(QLatin1Char('\n'), 0, 0).trimmed();
        static const QRegularExpression reNoise(
            QStringLiteral("^[\"'“”「『<>#*`]+|[\"'“”「』<>#*`。；;，,]+$"));
        sName.remove(reNoise);
        sName = sName.simplified();
        if (sName.size() > 24) { sName = sName.left(24).trimmed(); }
        if (sName.size() < 2) {   // 空名/乱输出：保持原名
            StageEvent(pTask, QStringLiteral("auto_name_failed"),
                QStringLiteral("✗ Agent 未给出有效名称，任务名保持不变"));
            emit sigAutoNameDone(pTask, false, QString());
            break;
        }
        pTask->SetAutoNamed(false);
        pTask->SetName(sName);   // 发 sigChanged → task.json 回写 + UI 刷新
        StageEvent(pTask, QStringLiteral("auto_named"),
            QStringLiteral("⚙ Agent 已自动命名：「%1」").arg(sName));
        emit sigAutoNameDone(pTask, true, sName);
        break;
    }
    case Submitting: {
        if (!a_bOk) {
            // 2026.08.15 ad-hoc max-turns 误报修复（7ce53172 实测）：--max-turns 40 打满时
            //   is_error=true 且 result 为空 → 旧代码显示「✗ 助手调用失败：」空白，且丢弃断点
            //   sid（用户「继续」只能从旧 sid fork，丢掉已跑的轮次）。max-turns 非真错误：
            //   先自动 --resume 续跑（上限 2 次）；耗尽也不判死——压栈保住断点，提示发「继续」接续。
            if (m_pAdhocRunner != nullptr && m_pAdhocRunner->LastWasMaxTurns()
                && !a_sSid.isEmpty()) {
                m_sAdhocSid = a_sSid;   // 链基点推进到断点 sid：后续 fork 从断点续，不丢已完成轮次
                if (m_iAdhocResume < 2) {
                    ++m_iAdhocResume;
                    emit sigAdhocEvent(QStringLiteral("resume"),
                        QStringLiteral("⚠ 达到轮次上限，已完成部分已落盘，自动续跑（第 %1/2 次）")
                            .arg(m_iAdhocResume));
                    StartAdhocStage(QStringLiteral(
                        "继续完成未完成的工作，从上次中断处接着做；不要重复已完成的改动。"));
                    return;   // 保持 Submitting 模式与 pendingPrompt，续跑成功后正常压栈
                }
                // 续跑预算耗尽：压栈断点（本条提问至此告一段落），交还用户接续
                m_aAdhocStack.append({ m_sAdhocPendingPrompt, a_sSid });
                emit sigAdhocStateChanged();   // 链推进 → 存盘
                m_eAdhocMode = Idle;
                m_sAdhocPendingPrompt.clear();
                emit sigAdhocEvent(QStringLiteral("stage_failed"),
                    QStringLiteral("⚠ 达到轮次上限，已自动续跑 2 次仍未收敛；会话已保留，发送「继续」可接着做"));
                emit sigAdhocContext(-1);
                emit sigAdhocFinished();
                return;
            }
            emit sigAdhocEvent(QStringLiteral("stage_failed"),
                QStringLiteral("✗ 助手调用失败：%1").arg(a_sResult.left(200)));
            // #6 失败（崩溃/异常退出/返回 is_error）时也刷新上下文按钮：发 -1 显示「—」，
            //   明确表示本轮未正常完成、占用未知，而非停留在误导性的「0」
            //   （Reset() 后未收到 result 行 → m_iLastContextFill 恒为 0，成功路径才会填真实值）。
            emit sigAdhocContext(-1);
            m_eAdhocMode = Idle;
            m_sAdhocPendingPrompt.clear();
            emit sigAdhocFinished();
            return;
        }
        // 压栈 (prompt, newSid) 并推进链基点；sid 为空（异常）则不压栈，链不动。
        if (!a_sSid.isEmpty()) {
            m_aAdhocStack.append({ m_sAdhocPendingPrompt, a_sSid });
            m_sAdhocSid = a_sSid;
            emit sigAdhocStateChanged();   // #1 会话链推进 → 存盘
        }
        m_sAdhocPendingPrompt.clear();
        m_eAdhocMode = Idle;
        emit sigAdhocModel(m_pAdhocRunner ? m_pAdhocRunner->LastModel() : QString());
        emit sigAdhocContext(m_pAdhocRunner ? m_pAdhocRunner->LastContextFill() : 0);
        // 需求4：非只读会话完成 → 触发 guide.md 增量同步（SyncGuideAsync 内部按 record 水位线判定，
        //   无新改动则静默跳过不起 agent）。agent 若已自治写了 record，此处即检测到新内容。
        //   2026.08.27 改空闲 3 分钟节流（见构造函数 m_tGuideSyncIdle）：不再与下一轮提示词
        //   并发抢网关；活跃使用期间持续顺延，停下 3 分钟后才同步（含手动菜单立即可同步）。
        if (!m_bAdhocReadOnly && m_pPm != nullptr) {
            m_tGuideSyncIdle.start();
        }
        emit sigAdhocEvent(QStringLiteral("done"), QStringLiteral("已完成"));
        // 2026.08.27 运行中排队的追加指令自动续发提前到 sigAdhocFinished 之前（同链续跑）：
        //   原实现在收尾信号之后再续发——主窗口收尾链（OnAdhocRunFinished）会把「激活助手」
        //   的会话换载进编排器（用户在运行中切到/新建了别的助手时），续发指令就落进了别的
        //   助手的会话与日志；面板还会闪一下假空闲。现在续发轮与本轮同会话同助手，运行态
        //   经 sigAdhocRunning 幂空延续（面板 ● 不落），排队全部消费完才发 sigAdhocFinished。
        //   失败/续跑中断路径不消费（队列保留，待下一次成功提问后续发，原语义不变）。
        if (!m_aAdhocPendingInjects.isEmpty()) {
            const QString sNext = m_aAdhocPendingInjects.takeFirst();
            SubmitAdhoc(sNext, m_bAdhocReadOnly, m_sAdhocLastTaskId);
            return;
        }
        emit sigAdhocFinished();
        return;
    }
    case CompactSummarize: {
        m_sAdhocSummary = a_sResult.trimmed();
        if (!a_bOk || m_sAdhocSummary.isEmpty()) {
            emit sigAdhocEvent(QStringLiteral("compact_fail"),
                QStringLiteral("✗ 未能生成摘要，已取消压缩"));
            m_eAdhocMode = Idle;
            emit sigAdhocFinished();
            return;
        }
        // 第二步：临时清空 sid → StartAdhocStage 走新建会话分支投递摘要；
        //   失败时由 CompactReseed 分支从栈顶恢复原 sid（栈在此期间未动）。
        m_eAdhocMode = CompactReseed;
        m_sAdhocSid.clear();
        StartAdhocStage(m_sAdhocSummary);
        return;
    }
    case CompactReseed: {
        if (!a_bOk || a_sSid.isEmpty()) {
            // 重播种失败：回退到压缩前链基点（栈未动），保持会话连续。
            m_sAdhocSid = m_aAdhocStack.isEmpty() ? QString() : m_aAdhocStack.last().second;
            emit sigAdhocEvent(QStringLiteral("compact_fail"),
                QStringLiteral("✗ 压缩失败，已保留原对话"));
        } else {
            // 重置链：仅保留一个「已压缩」哨兵节点，新 sid 为基点。
            m_aAdhocStack.clear();
            m_aAdhocStack.append({ QStringLiteral("[已压缩上下文]"), a_sSid });
            m_sAdhocSid = a_sSid;
            emit sigAdhocStateChanged();   // #1 链重置 → 存盘
            emit sigAdhocModel(m_pAdhocRunner ? m_pAdhocRunner->LastModel() : QString());
            emit sigAdhocContext(m_pAdhocRunner ? m_pAdhocRunner->LastContextFill() : 0);
            emit sigAdhocEvent(QStringLiteral("compact_done"),
                QStringLiteral("✔ 已压缩上下文"));
        }
        m_eAdhocMode = Idle;
        emit sigAdhocFinished();
        return;
    }
    case Idle:
    default: {
        // 不应发生（无模式却收到 finished）：仅恢复空闲、启用输入栏。
        m_eAdhocMode = Idle;
        emit sigAdhocFinished();
        return;
    }
    }
}

