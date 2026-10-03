#include "CTaskOrchestrator.h"

#include <QFile>
#include "core/CFrwDevice.h"
#include <QTextStream>
#include <QDateTime>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSet>
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
#include "review/CReviewSdkClient.h"
#include "review/CReviewService.h"

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
        // 2026.09.03 多助手并发：任一助手会话仍占用（运行/宽限/重试）都算活跃，顺延。
        if (IsAnyAdhocRunning()
            || (m_pRunner != nullptr && m_pRunner->IsRunning())) {
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
    // 2026-09-30 预审服务回收（先停在跑轮；服务自身析构会停线程）。
    if (m_pSdkReview != nullptr) {
        m_pSdkReview->Stop();
        m_pSdkReview->deleteLater();
        m_pSdkReview = nullptr;
    }
    // 2026.09.03 多助手并发：释放全部助手会话（runner 交 deleteLater 随父对象回收）。
    for (auto it = m_aAdhoc.begin(); it != m_aAdhoc.end(); ++it) {
        if (it.value()->pRunner != nullptr) {
            it.value()->pRunner->deleteLater();
        }
        delete it.value();
    }
    m_aAdhoc.clear();
}

void CTaskOrchestrator::AsyncAdhocBackup(const QString& a_sAsstId, const QString& a_sOwner,
                                         const QString& a_sPrompt) {
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
    const QString sAsstId = a_sAsstId;
    m_thAdhocBackup = std::thread([this, pPm, sOwner, sPrompt, sAsstId]() {
        const int iSeq = pPm->BackupTree(QStringLiteral("turn"), sOwner,
                                         QString(), sPrompt.left(2000), 0);
        QMetaObject::invokeMethod(this, [this, iSeq, sAsstId]() {
            if (iSeq <= 0) {
                emit sigAdhocEvent(sAsstId, QStringLiteral("backup_failed"),
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
    m_bSingleStageRerun = false;   // 2026.09.28 清残留：单阶段重跑期间被停止时标志不随 Advance
                                    //   复位，残留 true 会把下一个正常任务的盲个阶段误判为重跑截停
    m_hEscalatedReview.remove(a_pTask->Id());   // 2026.09.22 重新启动即作废待裁决的升级（静默升级语义）
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
    m_bSingleStageRerun = false;   // 2026.09.28 停止即作废重跑标志（防残留误伤后续任务）
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
    // 2026-09-30 SDK 静态预审期间停止：预审服务停 + 任务立即落 Stopped（分钟级预审
    //   不能让 UI 等待；OnSdkPreReviewDone 的 StopRequested 分支兜底防双发）。
    if (m_bSdkReviewRunning && m_sSdkReviewTaskId == a_pTask->Id()) {
        if (m_pSdkReview != nullptr) {
            m_pSdkReview->Stop();
        }
        if (a_pTask->Status() != ETaskStatus::Stopped) {
            a_pTask->SetStatus(ETaskStatus::Stopped);
            StageEvent(a_pTask, QStringLiteral("stopped"), QStringLiteral("任务已停止"));
            emit sigTaskFinished(a_pTask, false);
        }
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
    // 2026.09.28 与析构同口径：join 前先请求中止——首轮全量扫描大工程可达数十分钟，
    //   无条件等待会把 UI 卡死在切工程路径上。
    if (m_pPm != nullptr) {
        m_pPm->RequestBackupAbort();
    }
    m_bSingleStageRerun = false;   // 2026.09.28 停止即作废重跑标志
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
    // 2026-09-30 SDK 预审一并停（预审期间切工程/关程序不留后台 LLM 调用）。
    if (m_bSdkReviewRunning && m_pSdkReview != nullptr) {
        m_pSdkReview->Stop();
    }
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
    // 2026.09.28 编译门在跑也计入：旧实现构建期间 m_pRunner 空闲，StartTask 可启动另一
    //   任务并改写 m_pCurTask，OnBuildFinished 会把构建结论记到新任务并错误推进其阶段。
    if (m_pBuildProc != nullptr && m_pBuildProc->state() != QProcess::NotRunning) {
        return true;
    }
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
        CFrwDevice stFile(sViewPath);
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
            m_hEscalatedReview.insert(a_pTask->Id(), eCur);   // 2026.09.22 记住升级时的评审阶段（人工放行从它推进）
            a_pTask->SetCurrentStage(eBack);
            a_pTask->SetPauseRequested(true);   // 升级到用户
            a_pTask->SetStatus(ETaskStatus::Paused);   // 2026.09.22 必须落 Paused：旧路径停在 Running 会被 StartTask 早退拒续（升级后无法「继续」）
            WriteEscalationReport(a_pTask, eBack);
            const QString sDigest = ExtractRejectDigest(a_pTask);
            StageEvent(a_pTask, QStringLiteral("escalation"),
                QStringLiteral("⚠ %1 已迭代 %2 次仍被驳回，已暂停并升级到用户处理｜报告：%3｜阻塞：%4")
                    .arg(StageNameZh(eBack)).arg(MaxIter())
                    .arg(QStringLiteral(".autodev/tasks/%1/escalation.md").arg(a_pTask->Id()))
                    .arg(sDigest.left(120)));
            emit sigTaskChanged(a_pTask);
            emit sigReviewEscalated(a_pTask);   // 2026.09.22 人工介入：UI 弹窗问放行/再迭代/保持暂停
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

void CTaskOrchestrator::ResolveReviewEscalation(CTaskInfo* a_pTask, int a_iChoice) {
    // 2026.09.22 评审升级人工裁决回灌（sigReviewEscalated 弹窗的三个选项）。
    //   守卫：任务空/工程关/待裁决记录已被清（重新启动过）→ 忽略，防对已漂移状态重复动作。
    if (a_pTask == nullptr || m_pPm == nullptr) {
        return;
    }
    const QString sId = a_pTask->Id();
    if (!m_hEscalatedReview.contains(sId)) {
        return;   // 已裁决过或任务重启过：忽略
    }
    const EStage eReview = m_hEscalatedReview.value(sId);
    m_hEscalatedReview.remove(sId);

    if (a_iChoice == 0) {
        // 人工放行：视为评审通过，从升级时的评审阶段按 Pass 口径推进下一阶段。
        a_pTask->SetPauseRequested(false);
        a_pTask->ResetStageIter();
        a_pTask->SetLastSessionId(QString());
        StageEvent(a_pTask, QStringLiteral("manual_pass"),
            QStringLiteral("✔ 人工确认：评审驳回经用户确认放行，继续推进（质量责任转用户确认）"));
        const std::optional<EStage> oNext = CStageMachine::NextStage(TaskFlow(a_pTask), eReview);
        if (!oNext.has_value()) {
            a_pTask->SetCurrentStage(eReview);
            a_pTask->SetStatus(ETaskStatus::Done);
            StageEvent(a_pTask, QStringLiteral("done"), QStringLiteral("🎉 任务全部完成"));
            emit sigTaskFinished(a_pTask, true);
            return;
        }
        a_pTask->SetCurrentStage(*oNext);
        emit sigTaskChanged(a_pTask);
        QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
        return;
    }
    if (a_iChoice == 1) {
        // 再迭代一轮：重置迭代计数（新一轮完整预算）后重跑回炉阶段（升级时已置为当前阶段）。
        a_pTask->SetPauseRequested(false);
        a_pTask->ResetStageIter();
        StageEvent(a_pTask, QStringLiteral("manual_rerun"),
            QStringLiteral("↻ 人工介入：重置迭代计数，重跑当前阶段"));
        emit sigTaskChanged(a_pTask);
        QTimer::singleShot(0, this, [this, a_pTask]() { RunCurrentStage(a_pTask); });
        return;
    }
    // 2 = 保持暂停：任务维持 Paused（升级语义不变，用户可稍后点「继续」重跑）。
    StageEvent(a_pTask, QStringLiteral("manual_pause"),
        QStringLiteral("⏸ 人工介入：保持暂停，等待用户处理（可稍后点「继续」重跑）"));
    emit sigTaskChanged(a_pTask);
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
    // 2026-09-30 CodeReview 阶段 SDK 静态预审：变更文件先逐个过 ReviewSdk（分钟级、
    //   后台线程），完成后带发现重入本函数注入 prompt——agent 评审多一份确定性输入。
    //   配置关闭/dll 缺失/无变更清单 → 直接走原路径（零侵入）。
    if (a_stDef.eStage == EStage::CodeReview && MaybeStartSdkPreReview(a_pTask, a_stDef)) {
        return;
    }
    QString sPrompt = BuildPrompt(a_pTask, a_stDef);
    // 2026-09-30 SDK 预审结论块注入（同任务有块则附上；中断续跑路径同样附加）。
    if (a_stDef.eStage == EStage::CodeReview
        && m_sSdkReviewTaskId == a_pTask->Id() && !m_sSdkReviewBlock.isEmpty()) {
        sPrompt += m_sSdkReviewBlock;
    }
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
        // 2026-09-30 SDK 预审结论随续跑指令一并携带（预审已在会话中断前完成）。
        if (m_sSdkReviewTaskId == a_pTask->Id() && !m_sSdkReviewBlock.isEmpty()) {
            sResume += m_sSdkReviewBlock;
        }
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

CReviewService* CTaskOrchestrator::SdkReviewSvc() {
    if (m_pSdkReview == nullptr) {
        m_pSdkReview = new CReviewService(this);   // parent=this，析构统一回收
    }
    return m_pSdkReview;
}

QStringList CTaskOrchestrator::ParseChangedFilesList(const QString& a_sChangedMdAbs,
                                                     const QString& a_sProjectDir) {
    // changed_files.md（开发阶段落盘）行格式："+ <相对路径>" 新增 / "M <相对路径>" 修改 /
    //   "- <相对路径>" 删除——只取前两类且磁盘上仍存在的（写入清单后又被删的剔陈）。
    QStringList aOut;
    if (a_sChangedMdAbs.isEmpty() || a_sProjectDir.isEmpty()
        || !QFileInfo::exists(a_sChangedMdAbs)) {
        return aOut;
    }
    CFrwDevice stFile(a_sChangedMdAbs);
    if (!stFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return aOut;
    }
    const QStringList aLines = QTextStream(&stFile).readAll()
                                   .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (QString sLine : aLines) {
        sLine = sLine.trimmed();
        if (sLine.size() < 3) {
            continue;
        }
        const QChar cMark = sLine.at(0);
        if (cMark != QLatin1Char('+') && cMark != QLatin1Char('M')
            && cMark != QLatin1Char('m')) {
            continue;   // '-' 删除行 / 其他说明文本
        }
        QString sRel = sLine.mid(1).trimmed();
        if (sRel.isEmpty()) {
            continue;
        }
        sRel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (QDir::isAbsolutePath(sRel)) {
            continue;   // 清单约定相对工程根；绝对路径属异常写法，跳过防误审
        }
        const QString sAbs = QDir::cleanPath(a_sProjectDir + QLatin1Char('/') + sRel);
        if (!QFileInfo::exists(sAbs) || !QFileInfo(sAbs).isFile()) {
            continue;
        }
        const QString sKey = sAbs.toLower();   // Windows 大小写不敏感去重
        if (!seen.contains(sKey)) {
            seen.insert(sKey);
            aOut.append(sAbs);
        }
    }
    return aOut;
}

bool CTaskOrchestrator::MaybeStartSdkPreReview(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    if (a_pTask == nullptr || m_pPm == nullptr || m_bSdkReviewRunning) {
        return false;
    }
    if (!m_pPm->LoadReviewSdkOnStage()) {
        return false;   // 配置关闭：回纯 agent 评审
    }
    // 引擎可用性：dll 缺失/导出不全 → 一次性事件提示后按关闭处理（不阻断主流程）。
    CReviewSdkClient* pSdk = CReviewSdkClient::Get();
    QString sErr;
    if (!pSdk->EnsureLoaded(&sErr)) {
        static bool s_bWarned = false;
        if (!s_bWarned) {
            s_bWarned = true;
            StageEvent(a_pTask, QStringLiteral("sdk_review"),
                QStringLiteral(" ⚠ 审查引擎不可用，静态预审已跳过：%1").arg(sErr));
        }
        return false;
    }
    // 变更清单：任务自己的 changed_files.md（非全局最新——多任务并发防串档）。
    const QString sChangedMd = m_pPm->TaskDir(a_pTask->Id())
                             + QStringLiteral("/changed_files.md");
    const QDateTime dtMt = QFileInfo(sChangedMd).lastModified();
    // 复用判定：同任务/同迭代/清单未变且已有结论 → 不重跑（拒绝回炉重审同代码）。
    if (m_sSdkReviewTaskId == a_pTask->Id()
        && m_iSdkReviewIter == a_pTask->StageIter()
        && dtMt.isValid() && m_dtSdkChangedMt.isValid() && m_dtSdkChangedMt == dtMt
        && !m_sSdkReviewBlock.isEmpty()) {
        return false;
    }
    QStringList aFiles = ParseChangedFilesList(sChangedMd, a_pTask->ProjectDir());
    if (aFiles.isEmpty()) {
        // 无送审文件（Develop 未落盘清单/清单为空）：清块直走 agent 评审。
        m_sSdkReviewBlock.clear();
        m_sSdkReviewTaskId.clear();
        return false;
    }
    const int iCap = m_pPm->LoadReviewSdkMaxFiles();
    const bool bTrunc = aFiles.size() > iCap;
    if (bTrunc) {
        aFiles = aFiles.mid(0, iCap);
    }
    CReviewService* pSvc = SdkReviewSvc();
    if (pSvc == nullptr) {
        return false;
    }
    m_bSdkReviewRunning = true;
    m_sSdkReviewTaskId  = a_pTask->Id();
    m_iSdkReviewIter    = a_pTask->StageIter();
    m_dtSdkChangedMt    = dtMt;
    m_sSdkReviewBlock.clear();
    StageEvent(a_pTask, QStringLiteral("sdk_review"),
        QStringLiteral("▶ 静态预审 %1 个变更文件（ReviewSdk %2，后端 %3）%4")
            .arg(QString::number(aFiles.size()), pSdk->Version(),
                 CAgentRunnerFactory::BackendDisplayName(),
                 bTrunc ? QStringLiteral("（超上限截断）") : QString()));
    const QPointer<CTaskInfo> pGuard(a_pTask);
    const SStageDef stDef = a_stDef;
    m_cSdkDone = connect(pSvc, &CReviewService::sigReviewDone, this,
        [this, pGuard, stDef](bool bOk, bool bStopped, const QString& sJson,
                              const QString& sMd, int iFiles, int iFindings) {
            disconnect(m_cSdkDone);
            if (pGuard.isNull()) {
                m_bSdkReviewRunning = false;
                return;   // 预审期间任务被删除/归档回收
            }
            OnSdkPreReviewDone(pGuard.data(), stDef, bOk, bStopped, sJson, sMd,
                               iFiles, iFindings);
        });
    if (!pSvc->Start(a_pTask->ProjectDir(), aFiles, m_pPm->LoadReviewSdkTimeoutSec())) {
        disconnect(m_cSdkDone);
        m_bSdkReviewRunning = false;
        m_sSdkReviewBlock.clear();
        StageEvent(a_pTask, QStringLiteral("sdk_review"),
                   QStringLiteral(" ⚠ 静态预审启动失败，本轮跳过注入"));
        return false;
    }
    return true;
}

void CTaskOrchestrator::OnSdkPreReviewDone(CTaskInfo* a_pTask, const SStageDef& a_stDef,
                                           bool bOk, bool bStopped, const QString& sJson,
                                           const QString& sMd, int iFiles, int iFindings) {
    m_bSdkReviewRunning = false;
    // 报告落盘（任务私有目录，供事后查阅；失败静默——不影响注入）。
    if (m_pPm != nullptr && !sMd.isEmpty()) {
        const QString sPath = m_pPm->TaskDir(a_pTask->Id())
                            + QStringLiteral("/sdk_review.md");
        CFrwDevice stFile(sPath);
        if (stFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            stFile.write(sMd.toUtf8());
            stFile.close();
        }
    }
    // 注入块：正常完成→发现清单（或「未发现问题」）；被停止→部分结果降级说明；
    //   全失败→不注入（agent 自行评审，预审仅是增强输入而非门禁）。
    if (!bOk && !bStopped) {
        m_sSdkReviewBlock.clear();
        StageEvent(a_pTask, QStringLiteral("sdk_review_failed"),
                   QStringLiteral(" ✖ 静态预审失败（全部文件送审失败），本轮不注入"));
    } else if (bStopped) {
        m_sSdkReviewBlock = QStringLiteral(
            "\n\n## SDK 静态审查发现（ReviewSdk 预审被停止，仅部分完成）\n"
            "静态预审被提前停止：送审 %1 个文件、发现 %2 条。以下为已完成部分，"
            "请以你自己的代码审查为主。\n```json\n%3\n```\n")
            .arg(QString::number(iFiles), QString::number(iFindings), sJson.left(16000));
        StageEvent(a_pTask, QStringLiteral("sdk_review"),
            QStringLiteral(" ◼ 静态预审被停止（送审 %1，发现 %2 条）")
                .arg(QString::number(iFiles), QString::number(iFindings)));
    } else if (iFindings == 0) {
        m_sSdkReviewBlock = QStringLiteral(
            "\n\n## SDK 静态审查发现（ReviewSdk 预审）\n"
            "送审 %1 个变更文件，静态审查未发现问题。\n")
            .arg(QString::number(iFiles));
        StageEvent(a_pTask, QStringLiteral("sdk_review"),
                   QStringLiteral(" ✔ 静态预审完成：送审 %1，未发现问题")
                       .arg(QString::number(iFiles)));
    } else {
        m_sSdkReviewBlock = QStringLiteral(
            "\n\n## SDK 静态审查发现（ReviewSdk 预审结果，需复核）\n"
            "以下是对本轮变更文件逐个静态审查的发现（共 %1 条，级别 high/medium/low，"
            "完整报告见 {{TASK_DIR}}/sdk_review.md）。请逐条核实后纳入你的审查结论：\n"
            "- 属实且达到 critical/major 严重度的并入问题列表并影响决策；\n"
            "- 误报/过时的在总体评价中说明排除理由（不必逐条展开）。\n"
            "```json\n%2\n```\n")
            .arg(QString::number(iFindings), sJson.left(16000));
        StageEvent(a_pTask, QStringLiteral("sdk_review"),
            QStringLiteral(" ✔ 静态预审完成：送审 %1，发现 %2 条（详情 sdk_review.md）")
                .arg(QString::number(iFiles), QString::number(iFindings)));
    }
    // —— 续跑 CodeReview 阶段（守卫同备份线程口径）——
    if (a_pTask->StopRequested()) {
        if (a_pTask->Status() != ETaskStatus::Stopped) {   // StopTask 可能已收尾，防双发
            a_pTask->SetStatus(ETaskStatus::Stopped);
            StageEvent(a_pTask, QStringLiteral("stopped"), QStringLiteral("任务已停止"));
            emit sigTaskFinished(a_pTask, false);
        }
        return;
    }
    if (IsRunnerBusy() && m_pCurTask != a_pTask) {
        a_pTask->SetStatus(ETaskStatus::Paused);
        StageEvent(a_pTask, QStringLiteral("busy"),
            QStringLiteral("已有任务运行中，本任务暂置暂停，请先完成或停止当前任务"));
        emit sigTaskChanged(a_pTask);
        return;
    }
    LaunchStage(a_pTask, a_stDef);
}

QString CTaskOrchestrator::BuildPrompt(CTaskInfo* a_pTask, const SStageDef& a_stDef) {
    CFrwDevice stFile(GetPromptsDir() + QStringLiteral("/") + a_stDef.sPromptFile);
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
            CFrwDevice stReview(sReviewPath);
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

    // 2026.09.22 (2d9e4c0c 教训) 文档生产阶段 max-turns 降级：Design/Plan 轮次耗尽但
    //   本阶段产物已在本轮落盘（StageOutputFresh：存在+非空+mtime>=阶段起点）→ 不判死、
    //   不再续跑，直接放行推进——产物事实优先于轮次计数，质量由下游评审把关
    //   （ReviewDesign/ReviewPlan 驳回可回炉，复用轻评审降级同思路）。
    if (!a_bOk && m_pRunner != nullptr && m_pRunner->LastWasMaxTurns()
        && IsDocProduceStage(eCur)
        && pDef != nullptr && !pDef->sOutput.isEmpty()
        && StageOutputFresh(a_pTask, eCur)) {
        StageEvent(a_pTask, QStringLiteral("maxturns_output_ok"),
            QStringLiteral("⚠ %1 达到轮次上限，但 %2 已落盘，放行推进（质量由下游评审把关）")
                .arg(pDef->sNameZh).arg(pDef->sOutput));
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

    // 2026.09.22 假完成拦截（2d9e4c0c 实证）：Design 派后台探查代理后结束回合等通知，
    //   runner 把「回合结束」当阶段成功，design.md 根本没写 → 下一阶段 need_input 把整个
    //   阶段推倒重来，30 轮探索全部作废。产物未落盘时不推下一阶段：--resume 同会话催落盘
    //   （证据都在会话里，只差写出文件，续跑几轮即可收尾）；催一次仍无产物才走既有回退路径。
    if (a_bOk && IsDocProduceStage(eCur)
        && pDef != nullptr && !pDef->sOutput.isEmpty()
        && !StageOutputFresh(a_pTask, eCur)
        && !a_pTask->LastSessionId().isEmpty()
        && a_pTask->ResumeCount() < ReviewResume()) {
        a_pTask->IncResumeCount();
        StageEvent(a_pTask, QStringLiteral("flush_output"),
            QStringLiteral("⚠ %1 回合已结束但 %2 尚未写出，同会话续跑催落盘（第 %3/%4 次）")
                .arg(pDef->sNameZh).arg(pDef->sOutput)
                .arg(a_pTask->ResumeCount()).arg(ReviewResume()));
        ReRunResume(a_pTask, QStringLiteral(
            "你上一回合结束了，但本阶段产物文件尚未写出。请【立即】把已完成的探索与分析结论"
            "写入 %1（此前已写的部分保留并补全），然后结束。"
            "禁止再派发后台代理或任何新的探索；若个别探查未返回，基于已有证据给出保守结论并标注未知项。")
            .arg(QStringLiteral(".autodev/tasks/%1/%2").arg(a_pTask->Id(), pDef->sOutput)));
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
                m_hEscalatedReview.insert(a_pTask->Id(), eCur);   // 2026.09.22 记住升级时的评审阶段（人工放行从它推进）
                a_pTask->SetCurrentStage(eBack);
                a_pTask->SetPauseRequested(true);  // 升级到用户
                a_pTask->SetStatus(ETaskStatus::Paused);   // 2026.09.22 必须落 Paused：旧路径停在 Running 会被 StartTask 早退拒续（升级后无法「继续」）
                WriteEscalationReport(a_pTask, eBack);
                const QString sDigest = ExtractRejectDigest(a_pTask);
                StageEvent(a_pTask, QStringLiteral("escalation"),
                    QStringLiteral("⚠ %1 已迭代 %2 次仍被驳回，已暂停并升级到用户处理"
                                   "｜报告：%3｜阻塞：%4")
                        .arg(StageNameZh(eBack)).arg(MaxIter())
                        .arg(QStringLiteral(".autodev/tasks/%1/escalation.md").arg(a_pTask->Id()))
                        .arg(sDigest.left(120)));
                emit sigTaskChanged(a_pTask);
                emit sigReviewEscalated(a_pTask);   // 2026.09.22 人工介入：UI 弹窗问放行/再迭代/保持暂停
                return;   // 2026.09.28 升级即停：不再调度 RunCurrentStage（对齐 FinishReviewStage；
                          //   旧实现升级后仍 singleShot，若定时器触发前用户已裁决放行/再迭代会
                          //   造成同阶段双启动竞态）
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
    CFrwDevice stBt(sRoot + QStringLiteral("/.autodev/build.txt"));
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
        CFrwDevice stPf(sRoot + QStringLiteral("/CMakePresets.json"));
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
    CFrwDevice stIn(m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/build_verdict.md"));
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
    CFrwDevice stP(a_pTask->ProjectDir() + QStringLiteral("/") +
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

int CTaskOrchestrator::EstimateDocTurns(CTaskInfo* a_pTask) const {
    // 2026.09.22 (2d9e4c0c 教训)：文档生产阶段（Design/Plan）轮次按 requirements.md 规模动态给。
    //   大需求（17FR+26AC、跨 4 个代码区域）固定 30 轮连探索都做不完；行数是需求体量
    //   最稳定的代理指标（FR/AC 条目越多行数越多），每 6 行 +1 轮，下限 50、上限 110。
    if (a_pTask == nullptr) {
        return 50;
    }
    CFrwDevice stR(a_pTask->ProjectDir() + QStringLiteral("/") +
              QStringLiteral(".autodev/tasks/%1/requirements.md").arg(a_pTask->Id()));
    if (!stR.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 50;   // 无需求文档（如 bugfix 流）→ 默认档
    }
    const QString sReq = QTextStream(&stR).readAll();
    const int iLines = sReq.count(QLatin1Char('\n')) + 1;
    return qBound(50, 40 + iLines / 6, 110);
}

bool CTaskOrchestrator::IsResumeEligible(EStage a_eStage) {
    // 仅长运行的代码改写阶段允许 max-turns 续跑（文档/评审阶段短，续跑意义不大）。
    // Verify 编译失败可 --resume 续跑修复（design §3.2.6）。
    // 2026.09.22 (2d9e4c0c 教训)：Design/Plan 纳入——design.md/plan.md 是增量落盘的，
    //   --resume 保住已探索证据接着写；此前每次重跑都是零记忆从零探索，同样 30 轮的墙撞 3 次
    //   （实测烧掉 17.1M input token 后任务判死，而孤儿进程在判死后 23 分钟写完了完整的 design.md）。
    return a_eStage == EStage::Develop
        || a_eStage == EStage::BugFix
        || a_eStage == EStage::Test
        || a_eStage == EStage::Verify
        || a_eStage == EStage::Design
        || a_eStage == EStage::Plan;
}

bool CTaskOrchestrator::IsDocProduceStage(EStage a_eStage) {
    // 2026.09.22：产出单一文档、内容增量落盘、下游有评审把关的生产阶段（Design/Plan）。
    //   与 IsLightReviewStage 对偶：评审是「读代码写结论」，此处是「读代码写方案」，
    //   max-turns 时凭已落盘产物降级放行都安全（质量由 ReviewDesign/ReviewPlan 把关）。
    return a_eStage == EStage::Design || a_eStage == EStage::Plan;
}

bool CTaskOrchestrator::StageOutputFresh(CTaskInfo* a_pTask, EStage a_eStage) const {
    // 2026.09.22：阶段产物「本轮已写出」判定——文件存在、非空、mtime >= m_dtStageStart
    //   （LaunchStage/ReRunResume 起点；m_dtStageStart 无效时退化为仅存在性判定）。
    //   缺失/过期 = 假完成，编排器据此催落盘或回退，不再被「回合结束」表象蒙蔽。
    const SStageDef* pDef = FindStage(a_eStage);
    if (a_pTask == nullptr || m_pPm == nullptr
        || pDef == nullptr || pDef->sOutput.isEmpty()) {
        return false;
    }
    const QFileInfo stFi(m_pPm->TaskDir(a_pTask->Id()) + QStringLiteral("/")
                         + pDef->sOutput);
    return stFi.exists() && stFi.size() > 0
        && (!m_dtStageStart.isValid()
            || stFi.lastModified() >= m_dtStageStart);
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
    // 2026.08.20 (自动命名) 素材源定稿：只读任务内容（用户原始需求）。2026.09.21
    //   用户口径收紧：**自动命名只根据任务内容，不读工程信息**——原 guide.md 标题
    //   兜底分支已删（工程信息不是本任务的内容，混入会带偏命名）。也不读
    //   requirements.md / intake.md 等阶段产物（模板腔标题，「需求收集 — 任务 xxx」
    //   「用户原始需求」两轮误命名皆源于此）。
    //   统一清洗（去 #/*/_/` 标记与「— 任务 id」装饰、TrimNamePunct 剥尾标点）后按
    //   「一句话简短描述」口径：取首子句（不足 10 字顺延到下一子句边界），截断 24 字。
    //   SetName 发 sigChanged，由 CMainWindow 落盘 task.json 并刷新 UI。

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
    // ① 任务内容（用户原始需求）：2026.09.10 v2.2.152 选行修复——首个非空行（总起句）
    //    优先（CTaskInfo::ExtractNameFromRequest 内含编号/引导语剥离+首子句截断，首行泛型/太短时内退「最长且 ≤80 字」行）。
    //    旧「最长行优先」策略把 79 字压线的编号细节条款（第 7 条）当主旨、抠出「资源管理器右键菜单增加」
    //    丢掉任务主旨的实害勿回退——需求文档首行几乎总是主旨陈述，细节行再长也是子项。
    sCandidate = CTaskInfo::ExtractNameFromRequest(a_pTask->UserRequest());
    // 首行/最长行均不可用（空）：退首个非空行原文（后续清洗+泛型复核）。
    if (sCandidate.isEmpty()) {
        for (const QString& sLine : a_pTask->UserRequest().split(QLatin1Char('\n'))) {
            const QString s = sLine.trimmed();
            if (!s.isEmpty()) { sCandidate = s; break; }
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
    sCandidate = CTaskInfo::TrimNamePunct(sCandidate);   // 2026.09.21 尾标点/引号剥离
    if (sCandidate.isEmpty() || isGeneric(sCandidate)) {
        return false;   // 无可用素材/纯泛型（如「如题」）：保留临时名，标记不清
                        //   （下次 Intake 重跑 Agent 定稿再试）——2026.09.21 只根据
                        //   任务内容口径：guide 兜底已删，泛型名宁缺毋滥。
    }

    a_pTask->SetAutoNamed(true);   // 2026.09.10 v2.2.152：本地启发式名只是临时名——保留
    //   autoNamed 标记，Intake 成功后的钩子（Advance）会用 Agent 重新定稿覆盖（实害：旧版
    //   此处清标记，创建时 guide 生成占用 runner → 本地兜底 → 标记被清 → Intake 后 Agent
    //   重命名钩子条件 AutoNamed()==true 不满足 → 「资源管理器右键菜单增加」坏名永固）。
    //   手动改名/Agent 定稿/历史复活各自清标记，不冲突。
    a_pTask->SetName(sCandidate);   // 发 sigChanged → task.json 回写 + UI 刷新
    StageEvent(a_pTask, QStringLiteral("auto_named"),
        QStringLiteral("⚙ 任务临时命名（本地启发式，待 Agent 定稿）：「%1」").arg(sCandidate));
    return true;
}

bool CTaskOrchestrator::AutoNameNow(CTaskInfo* a_pTask, const QString& a_sAsstId) {
    // 2026.08.20 (Agent 起名)：任务内容（用户原始需求）+ 工程 guide.md 拼提示词，经
    //   adhoc 助手 runner 独立模式（AutoNaming）交 Agent 生成一句话简短名称；结果在
    //   OnAdhocFinished 清洗后 SetName 回填，并发 sigAutoNameDone 供主窗口镜像 DB/刷新。
    //   借用助手忙/工程未开 → 回退本地启发式 MaybeAutoNameTask（不阻塞命名）。
    //   2026.09.03 多助手并发：借用 a_sAsstId 助手（缺省第一个会话）的 runner 跑命名
    //   （fork 一次性分支，不动其撤销栈）；该助手忙则本地回退，不影响其他助手并发。
    if (a_pTask == nullptr) { return false; }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return MaybeAutoNameTask(a_pTask);
    }
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr && !m_aAdhoc.isEmpty()) {
        pS = *m_aAdhoc.begin();   // 未指定/已删：取任一会话（命名无会话语义，一次性分支）
    }
    if (pS == nullptr) {
        return MaybeAutoNameTask(a_pTask);   // 无助手会话：本地兜底
    }
    if (pS->pRunner != nullptr
        && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
            || pS->pRunner->IsRetryPending())) {
        // 借用助手被占用（用户提问/压缩/上一次命名中）：本地启发式兜底，避免命名饿死。
        return MaybeAutoNameTask(a_pTask);
    }
    // 素材：任务内容（用户原始需求，截 2000 字防提示词膨胀）——2026.09.21 用户口径：
    //   自动命名只根据任务内容，不读工程信息（guide.md 段已删，并明令 Agent 不读文件）。
    QString sReq = a_pTask->UserRequest().left(2000).trimmed();
    if (sReq.isEmpty()) {
        // 任务内容为空：没有素材可命名——直接本地兜底（也会失败返回，保留临时名），
        //   不再拿「结合项目指南推断」当素材（旧文案会诱导 Agent 读工程信息）。
        return MaybeAutoNameTask(a_pTask);
    }
    const QString sPrompt = QStringLiteral(
        "【任务命名】请为下面的任务起一个简短的中文任务名称。\n\n"
        "用户的任务需求（任务内容全文）：\n---\n%1\n---\n\n"
        "命名要求：\n"
        "1. 只根据上面的任务内容命名，不要读取任何文件或目录；\n"
        "2. 一句话描述任务要做什么，不超过 20 个字；名称通常就是首句主旨句，保留其中\n"
        "   原有的动词开头（如「开发大模型网关插件」不要删成「大模型网关插件」）；\n"
        "3. 需求常为多行编号文档——名称必须概括任务的整体目标（如「自动拉取/提交/推送\n"
        "   代码的工具」），绝不要抓取某一条编号细节、界面元素或子功能片段当名称\n"
        "   （如「资源管理器右键菜单」）；\n"
        "4. 不要包含「任务」「需求」这类泛型词，不要带任务 id；\n"
        "5. 只输出名称本身：不要任何解释、引号、句尾冒号等标点结尾或多余文字。")
        .arg(sReq);
    pS->pAutoNameTask = a_pTask;
    pS->iPerm = PermReadOnly;    // 纯命名：禁 Bash/Edit/Write
    pS->sPendingPrompt.clear();
    pS->eMode = AutoNaming;
    StageEvent(a_pTask, QStringLiteral("auto_naming"),
        QStringLiteral("⚙ 正在根据任务内容请 Agent 生成任务名称…"));
    StartAdhocStage(*pS, sPrompt);
    return true;
}

bool CTaskOrchestrator::AutoNameAsstNow(const QString& a_sAsstId, const QString& a_sPrompts) {
    // 2026-09-22 助手自动重命名：右键菜单「自动重命名」→ 面板收集该助手全部用户提示词
    //   经 sigAsstAutoRename → CMainWindow 转到这里。优先借用该助手自己的会话（fork
    //   一次性只读分支，不动撤销栈）；其忙则借用任一空闲会话；全忙/无会话返回 false。
    //   结果在 OnAdhocFinished 的 AsstAutoNaming 分支解析两行（名称+说明）后发
    //   sigAsstAutoNameDone → 面板回填 sName/sDesc 并落盘。
    if (a_sAsstId.isEmpty() || a_sPrompts.trimmed().isEmpty()) {
        return false;
    }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        return false;   // 工程未开（会话/运行时依赖工程）
    }
    auto fnBusy = [](SAdhocSession* a_pS) {
        return a_pS->pRunner != nullptr
            && (a_pS->pRunner->IsRunning() || a_pS->pRunner->IsGraceActive()
                || a_pS->pRunner->IsRetryPending());
    };
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        // 目标会话尚未建（新建助手还没发过提示词等异常态）：直接建它自己的会话
        //   （用助手自己的粘性后端/全局后端，不借别人的）。
        pS = AdhocSess(a_sAsstId, /*a_bCreate=*/true);
    }
    if (fnBusy(pS)) {
        // 目标会话被占用（用户提问/压缩中）：借用任一空闲会话（命名无会话语义，一次性分支）。
        SAdhocSession* pIdle = nullptr;
        for (SAdhocSession* pCand : m_aAdhoc) {
            if (pCand != pS && !fnBusy(pCand)) { pIdle = pCand; break; }
        }
        if (pIdle == nullptr) {
            return false;   // 全忙：调用方提示稍后再试
        }
        pS = pIdle;
    }
    const QString sPrompt = QStringLiteral(
        "【助手命名】某个 AI 助手收到过下面这批用户提示词，请为该助手起名并写一句说明。\n\n"
        "用户发给该助手的全部提示词（按时间顺序编号）：\n---\n%1\n---\n\n"
        "输出要求：\n"
        "1. 只根据上面的提示词内容总结，不要读取任何文件或目录；\n"
        "2. 第一行：助手名称——不超过 10 个汉字，概括这些提示词共同的主题或用途，\n"
        "   保留动词开头，不要「助手」「任务」这类泛型词；\n"
        "3. 第二行：助手说明——一到两句话（不超过 60 字），概括用户在这个助手里\n"
        "   主要在做什么、达到了什么目的；\n"
        "4. 严格只输出这两行：第一行名称、第二行说明；不要任何解释、引号、编号、\n"
        "   Markdown 标记或多余文字。")
        .arg(a_sPrompts);
    pS->sAutoNameTargetAsst = a_sAsstId;
    pS->iPerm = PermReadOnly;    // 纯命名：禁 Bash/Edit/Write（用户下次提交时由面板回写）
    pS->sPendingPrompt.clear();
    pS->eMode = AsstAutoNaming;
    // 进行中提示直投被命名的助手流（执行可能在借用的其他助手会话上，其 Tab 只亮运行态）。
    emit sigAdhocEvent(a_sAsstId, QStringLiteral("auto_naming"),
        QStringLiteral("⚙ 正在根据历史提示词请 Agent 生成助手名称与说明…"));
    StartAdhocStage(*pS, sPrompt);
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
        CFrwDevice stFile(sPath);
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
            CFrwDevice stFile(sPath);
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
    CFrwDevice stFile(sPath);
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
        // 2026.09.22 (2d9e4c0c 教训) 文档生产阶段动态预算：大需求（17FR+26AC 跨多模块）
        //   30 轮连探索都做不完，max-turns 判死前从未走到写产物那一步；按需求规模动态抬升。
        case EStage::Design:
        case EStage::Plan:       return EstimateDocTurns(a_pTask);
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
    CFrwDevice stFile(sPath);
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
            a_pTask->SetStatus(ETaskStatus::Paused);   // 2026.09.28 对齐 FinishReviewStage/ProceedToNext
                                                          //   升级分支：必须落 Paused，否则停在 Running 会被
                                                          //   StartTask 早退拒续，任务卡死只能重启程序
            StageEvent(a_pTask, QStringLiteral("escalation"),
                QStringLiteral("⚠ 多次回退仍未生成 %1，已暂停并升级到用户处理").arg(sInput));
            emit sigTaskChanged(a_pTask);
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
void CTaskOrchestrator::EnsureAdhocRunner(SAdhocSession& a_S) {
    // 2026.09.03 多助手并发：runner per-助手（独立 QProcess，各起各的进程）。
    // 2026.09.20 助手粘性后端（v2.4.193，用户需求：助手不支持中途换 Harness → 发送时
    //   忽略全局设置、继续用该助手当前 Harness）：会话绑定后端 sBackend（空=未绑定，
    //   首次提交时绑当前全局），runner 按绑定后端创建/保留——全局切换不再清会话链
    //   （v2.4.183 清除语义由粘性取代；runner ≠ 绑定后端的漂移重建仅剩跨机拷贝档等
    //   边缘场景，链清空 + 自愈链仍兑底）。占用中（运行/宽限/重试）本次仍用旧 runner。
    const QString sBound = a_S.sBackend.isEmpty()
        ? CAgentRunnerFactory::BackendName() : a_S.sBackend;
    if (a_S.pRunner != nullptr) {
        if (a_S.pRunner->BackendName() == sBound) {
            return;
        }
        if (a_S.pRunner->IsRunning() || a_S.pRunner->IsGraceActive()
            || a_S.pRunner->IsRetryPending()) {
            return;
        }
        a_S.pRunner->deleteLater();
        a_S.pRunner = nullptr;
        a_S.sSid.clear();
        a_S.aStack.clear();
        a_S.aTranscript.clear();   // 2026.09.02 回灌缓存随会话链一并作废（跨后端历史无意义）
        a_S.sGuideHash.clear();
        a_S.aPendingInjects.clear();   // 会话链整体作废，排队指令随行
        emit sigAdhocPendingDropped(a_S.sId);   // 2026.09.18 面板暂存框同步作废
    }
    a_S.sBackend = sBound;
    a_S.pRunner = CAgentRunnerFactory::CreateFor(sBound, this);
    // 连 ad-hoc 专属槽（不复用 OnRunner*，与任务 runner 隔离）；lambda 捕获该会话 id
    //   作为路由首参——多助手并发时各流各归各的 Tab。
    const QString sId = a_S.sId;
    CAgentRunnerBase* const pR = a_S.pRunner;
    connect(pR, &CAgentRunnerBase::sigInfo, pR,
            [this, sId](const QString& a_sLine) { OnAdhocInfo(sId, a_sLine); });
    connect(pR, &CAgentRunnerBase::sigAssistantText, pR,
            [this, sId](const QString& a_sText) { OnAdhocAssistantText(sId, a_sText); });
    connect(pR, &CAgentRunnerBase::sigToolUse, pR,
            [this, sId](const QString& a_sTool, const QString& a_sInput) {
                OnAdhocToolUse(sId, a_sTool, a_sInput);
            });
    connect(pR, &CAgentRunnerBase::sigToolResult, pR,
            [this, sId](const QString& a_sResult) { OnAdhocToolResult(sId, a_sResult); });
    // 运行中上下文占用（每条 assistant 消息的 usage）→ 直通助手面板进度条实时刷新。
    connect(pR, &CAgentRunnerBase::sigContextFill, pR,
            [this, sId](qint64 a_iFill) { emit sigAdhocContext(sId, a_iFill); });
    // 2026.08.27 助手轮次实时计量：本轮累计 token 直通该助手页内的实时标签
    //   （2026.09.05 per-助手：会话内累计 + 带 id 上抛，多助手并发各自独立不互踩，
    //   同时修复旧全局单值在并发下互相覆盖/清零的混计）。
    connect(pR, &CAgentRunnerBase::sigUsageDelta, pR,
            [this, sId](qint64 a_iIn, qint64 a_iOut) {
                SAdhocSession* pS = m_aAdhoc.value(sId, nullptr);
                if (pS == nullptr) {
                    return;
                }
                pS->iLiveIn  += a_iIn;
                pS->iLiveOut += a_iOut;
                emit sigAdhocLiveUsage(sId, pS->iLiveIn, pS->iLiveOut);
            });
    // 流式实时刷新（绝对值直通显示，不入账）：同样按助手 id 路由。
    connect(pR, &CAgentRunnerBase::sigLivePartial, pR,
            [this, sId](qint64 a_iIn, qint64 a_iOut) {
                emit sigAdhocLiveUsage(sId, a_iIn, a_iOut);
            });
    connect(pR,
            qOverload<bool, const QString&, qint64, qint64, double, const QString&>(
                &CAgentRunnerBase::sigFinished),
            pR,
            [this, sId](bool a_bOk, const QString& a_sResult, qint64 a_iIn, qint64 a_iOut,
                        double a_dCost, const QString& a_sSid) {
                OnAdhocFinished(sId, a_bOk, a_sResult, a_iIn, a_iOut, a_dCost, a_sSid);
            });
}

void CTaskOrchestrator::StartAdhocStage(SAdhocSession& a_S, const QString& a_sPrompt,
                                        bool a_bDropLastUser, int a_iTurnsOverride,
                                        const QString& a_sToolsDeny) {
    EnsureAdhocRunner(a_S);
    // 新轮次开始：该会话本轮实时计量清零（2026.09.05 per-助手，带 id 收起其页内标签；
    //   不再碰全局 m_iLive*——那是任务阶段专用）。
    a_S.iLiveIn  = 0;
    a_S.iLiveOut = 0;
    a_S.bTurnSawTool = false;   // 2026.09.09 v2.1.131 纠偏判据轮首清零
    a_S.sLastAsstText.clear();
    emit sigAdhocLiveUsage(a_S.sId, 0, 0);
    const QString sWorkDir = m_pPm ? m_pPm->ProjectDir() : QString();
    // deny 黑名单（bypass 下 allow 失效、deny 生效）：只读轮=禁 Bash/Edit/Write；
    //   自动/手工审批轮=ToolsDev() 现返回空串=全开（手工审批的约束在提示词层，见
    //   SubmitAdhoc 的【手工审批模式】指令）。2026.09.26 专家模式规划/验证轮经
    //   a_sToolsDeny 覆盖（非 null 即生效：均禁 Edit/Write，保「规划/验证不改码」
    //   的职责分离；执行轮仍全开）。
    const QString sTools = (!a_sToolsDeny.isNull())
        ? a_sToolsDeny
        : ((a_S.iPerm == PermReadOnly)
            ? QStringLiteral("Bash,Edit,Write")
            : ToolsDev());
    // fork 链：有当前 sid 则 --resume + --fork-session 产生新 sid 分支，旧 sid 保持不污染
    //   （实测确认 → #4 O(1) 撤销可行）；无 sid（首条提问 / CompactReseed 前已清空）则纯新建会话。
    const bool bFork = !a_S.sSid.isEmpty();
    // 2026.08.15 autocompact 比例制：K（千 token）= 窗口 × [Adhoc] AutoCompactPct ÷ 100 ÷ 1000，
    //   窗口优先 [Adhoc] ContextLimit 用户覆盖、未设按助手最近所用模型探测（替代旧千 token 档位）。
    const int iAutoCompactK = m_pPm
        ? m_pPm->AutoCompactKFor(a_S.pRunner->LastModel()) : 150;
    a_S.dtStart = QDateTime::currentDateTime();   // 耗时归集：本次调用起点
    // 2026.09.02 跨轮上下文回灌：每轮全新会话的后端（DSH fork 换新 sid、
    //   Codex/Hermes 忽略 resumeSid）且已有会话链（第 2 轮起）时，把最近对话
    //   记录注入 prompt 头部——否则模型完全失忆。claude/pi 原生续接
    //   （KeepsContextAcrossTurns）不注入，防双计。
    QString sFinalPrompt = a_sPrompt;
    const QString sHistory = BuildAdhocHistoryBlock(a_S, a_bDropLastUser);
    if (!sHistory.isEmpty()) {
        sFinalPrompt = sHistory + QStringLiteral("\\n\\n") + a_sPrompt;
    }
    // 2026.09.23 v3.0.219 轮次预算：SubmitAdhoc 按提示词难度评估（iTurnsBudget），
    //   内部链路（命名/续跑等未评估路径）回退全局基值。
    a_S.pRunner->StartStage(sWorkDir, sFinalPrompt, sTools,
                            (a_iTurnsOverride > 0 ? a_iTurnsOverride
                                : (a_S.iTurnsBudget > 0 ? a_S.iTurnsBudget
                                                       : AdhocMaxTurns())),
                            a_S.sSid, bFork, iAutoCompactK, /*a_bEnableAskUser=*/true);
    emit sigAdhocRunning(a_S.sId);   // 面板进该助手运行态（手动发送已自置，此处兜自动续发/压缩）
}

int CTaskOrchestrator::AdhocMaxTurns() const {
    // 2026.09.23 助手单轮 --max-turns 配置化（[Adhoc] MaxTurns，默认 40，10..200）：
    //   大型多文件重构 40 轮常打满（LayoutDesign「打磨布局设计器」12 问 11 次触顶实测），
    //   允许用户按工程工作量放大；每次提问实时读，设置保存/任务级覆盖即时生效。
    return (m_pPm != nullptr) ? m_pPm->LoadAdhocMaxTurns() : 40;
}

bool CTaskOrchestrator::AdhocMaxTurnsAuto() const {
    // 2026.09.23 v3.0.219 [Adhoc] MaxTurnsAuto（缺省开）；m_pPm 空回退开。
    return (m_pPm != nullptr) ? m_pPm->LoadAdhocMaxTurnsAuto() : true;
}

int CTaskOrchestrator::EstimateAdhocTurns(const QString& a_sUserText, int a_iBaseTurns) {
    // 2026.09.23 v3.0.219 工作量评估（用户需求：运行前评估任务难度自动设轮次）：
    //   纯本地启发式——零 token、零额外 LLM 往返、毫秒级。四个信号：
    //   ① 编号/条目行数（“1.” “1、” “①” “-” 开头）——每多一个子任务 +12；
    //   ② 显式点名的源文件（xxx.cpp/.h/.py/…去重）——每个 +10，封顶 5 个；
    //   ③ 重活关键词（重构/重写/全部/批量/自测/构建/编译/测试/验证/排查/修复…去重）——
    //      每个 +8，封顶 4 个；
    //   ④ 文本长度（长需求≈多子任务）——200 字以上每 400 字 +6。
    //   只升不降：a_iBaseTurns 是用户设的底线（小问题不缩水）；封顶 200（与 SpinBox 上限一致）。
    //   实测锚点：LayoutDesign Q12（2 编号+600 字）→ 58；重设计长文（1.5k 字+多关键词）→ 90+；
    //   配合既有 max-turns 自动续跑 2 次（有效预算 ×3），重活也不断流。
    const int iBase = qBound(10, a_iBaseTurns, 200);
    const QString sText = a_sUserText.simplified();
    if (sText.isEmpty()) {
        return iBase;
    }
    // ① 编号/条目行
    int nIssues = 0;
    const QStringList aLines = a_sUserText.split(QLatin1Char('\n'));
    static const QRegularExpression reItem(
        QStringLiteral("^\\s*(?:\\d+[.、)）]|[(（]\\d+[)）]|[①②③④⑤⑥⑦⑧⑨⑩]|[-•*])"));
    for (const QString& sLn : aLines) {
        if (reItem.match(sLn).hasMatch()) {
            ++nIssues;
        }
    }
    // ② 点名源文件（去重）
    static const QRegularExpression reFile(
        QStringLiteral("([A-Za-z0-9_\\-.]+\\.(?:cpp|h|hpp|cc|cxx|c|py|js|ts|qml|ui|rc|md|json|xml|yml|yaml|bat|cmake|txt))(?![A-Za-z0-9_.\\-])"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> aFiles;
    for (auto it = reFile.globalMatch(sText); it.hasNext(); ) {
        aFiles.insert(it.next().captured(1).toLower());
    }
    // ③ 重活关键词（去重命中）
    static const QStringList aHeavy = {
        QStringLiteral("重构"), QStringLiteral("重写"), QStringLiteral("全部"),
        QStringLiteral("所有"), QStringLiteral("整体"), QStringLiteral("批量"),
        QStringLiteral("彻底"), QStringLiteral("自测"), QStringLiteral("构建"),
        QStringLiteral("编译"), QStringLiteral("回归"), QStringLiteral("测试"),
        QStringLiteral("验证"), QStringLiteral("排查"), QStringLiteral("修复"),
        QStringLiteral("迁移"), QStringLiteral("梳理"),
    };
    int nHeavy = 0;
    for (const QString& sKw : aHeavy) {
        if (sText.contains(sKw)) {
            ++nHeavy;
        }
    }
    int iEst = iBase;
    iEst += 12 * qMax(0, nIssues - 1);
    iEst += 10 * qMin(aFiles.size(), 5);
    iEst += 8 * qMin(nHeavy, 4);
    iEst += 6 * qMax(0, (sText.size() - 200) / 400);
    return qBound(iBase, iEst, 200);
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

// 2026.09.26 深度模式运行期覆盖（测试桩，SetAdhocDeepOverride 写）：-1=按 config.ini
//   [Adhoc] ExpertAssistant；0/1=强制关/开。定义前置：SubmitAdhoc 在前、实现在后。
static int g_iAdhocDeepOverride = -1;

void CTaskOrchestrator::SubmitAdhoc(const QString& a_sAsstId, const QString& a_sText,
                                    int a_iPerm, const QString& a_sTaskId,
                                   const QStringList* a_pTranscript) {
    // 2026.09.03 多助手并发：本函数只操作 a_sAsstId 的会话——该助手在跑 → 注入/排队
    //   （同助手追加语义）；空闲 → 起该助手自己的 runner 进程立即执行。其他助手是否
    //   在跑与本调用无关（各会话独立 runner/进程/会话链）。
    SAdhocSession* pS = AdhocSess(a_sAsstId, /*a_bCreate=*/true);
    if (pS->pRunner != nullptr && pS->pRunner->IsRunning()) {
        // 轮中介入（方案 §2.3）：非只读文本注入前同口径拍 turn 快照（失败不阻断）。
        if (a_iPerm != PermReadOnly && !a_sText.trimmed().isEmpty() && m_pPm != nullptr
            && !m_pPm->ProjectDir().isEmpty()) {
            const QString sOwner = a_sTaskId.isEmpty()
                ? QStringLiteral("global") : a_sTaskId;
            // 2026.08.26 turn 快照改后台（大工程 UI 卡死修复，见 AsyncAdhocBackup）。
            AsyncAdhocBackup(a_sAsstId, sOwner, a_sText);
        }
        if (!a_sText.trimmed().isEmpty()
            && pS->pRunner->InjectUserTurn(a_sText)) {
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("injected"),
                QStringLiteral("已向正在执行的助手轮注入追加指令（立即生效）：%1")
                    .arg(a_sText.left(120)));
        } else if (!a_sText.trimmed().isEmpty()) {
            pS->aPendingInjects.append(a_sText);
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("queued"),
                QStringLiteral("已排队追加指令（本轮结束后自动发送）：%1")
                    .arg(a_sText.left(120)));
        }
        return;
    }
    if (pS->pRunner != nullptr
        && (pS->pRunner->IsGraceActive() || pS->pRunner->IsRetryPending())) {
        // 崩溃宽限/重试等待中：runner 被占用但无稳定会话可注入，维持「请稍候」。
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("该助手正在处理上一条指令，请稍候"));
        return;
    }
    if (a_sText.trimmed().isEmpty()) {
        return;
    }
    if (m_pPm == nullptr || m_pPm->ProjectDir().isEmpty()) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("请先打开一个工程再使用助手"));
        return;
    }
    // 2026.09.20 粘性后端绑定/提示：未绑定会话首次提交时绑当前全局（之后不再随全局
    //   变化）；已绑定且 ≠ 全局时——仅当全局自上次提交后变化才发一次提示（避免每轮
    //   噪声），告知用户本助手继续用原 Harness、新建助手才用新后端。
    const QString sGlobal = CAgentRunnerFactory::BackendName();
    if (pS->sBackend.isEmpty()) {
        pS->sBackend = sGlobal;
    } else if (pS->sBackend != sGlobal && pS->sLastSeenGlobal != sGlobal) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("sticky"),
            QStringLiteral("本助手继续使用 %1 后端（全局已切换为 %2）；新建助手将使用 %2")
                .arg(CAgentRunnerFactory::DisplayNameOf(pS->sBackend),
                     CAgentRunnerFactory::DisplayNameOf(sGlobal)));
    }
    pS->sLastSeenGlobal = sGlobal;
    pS->iPerm = a_iPerm;
    pS->sLastTaskId = a_sTaskId;   // 自动续发排队指令时沿用同任务档（记忆注入）
    pS->iResume = 0;   // 新提问：max-turns 自动续跑计数清零
    // 2026.09.23 v3.0.219 工作量评估：按本条提示词难度自动上调轮次预算（MaxTurnsAuto
    //   可关）；预算随会话存续——续跑/纠偏/压缩重发沿用（同一条提问工作量不变）。
    {
        const int iBase = AdhocMaxTurns();
        pS->iTurnsBudget = AdhocMaxTurnsAuto()
            ? EstimateAdhocTurns(a_sText, iBase) : iBase;
        if (pS->iTurnsBudget > iBase) {
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("turns_budget"),
                QStringLiteral("⚙ 工作量评估：本轮任务较重，轮次上限 %1 → %2")
                    .arg(iBase).arg(pS->iTurnsBudget));
        }
    }
    pS->bNudgeUsed = false;   // 2026.09.09 v2.1.131 新提问重置纠偏配额
    pS->bAutoCompactDone = false;   // 2026.09.09 v2.1.132 新提问重置超限自愈配额
    pS->eMode = Submitting;
    pS->sPendingPrompt = a_sText;   // 原始提问入栈（注入内容不进撤销栈，保持用户可见语义）

    // 2026.09.02 跨轮上下文回灌：外部提交携带面板权威对话记录（含重启后自
    //   assistant.json 恢复的历史）→ 整体重建缓存；内部续发（排队指令）不传 →
    //   沿用缓存。本轮提问行先入缓存，StartAdhocStage 回灌时以 a_bDropLastUser
    //   剔除（prompt 尾部已含原文，防重复）；单行截断与面板同口径（1500 字）。
    if (a_pTranscript != nullptr) {
        pS->aTranscript = *a_pTranscript;
    }
    QString sUserLine = a_sText;
    if (sUserLine.size() > 1500) {
        sUserLine = sUserLine.left(1500) + QStringLiteral(" …[截断]");
    }
    AppendAdhocTranscriptLine(*pS, QStringLiteral("用户：") + sUserLine);

    // guide.md 投喂策略（需求3）：仅「首次新建会话」或「guide.md 自上次投喂后发生变化」时注入，
    //   避免每次续问都重复投喂冗长指南；用内容 SHA256 判变，哈希随 assistant.json 持久化。
    //   即：初始化投喂 → 后续打开工程仅恢复上下文不投喂 → guide.md（重新生成 / 同步 agent 改动）后自动重投。
    // 2026.09.09 v2.1.130 用户请求显式框架（实害：pi+本地 spark2.5-4b，请求几~几十字
    //   无标记地夹在 pi 注入的 1700 字全局规则与 AutoCode 尾部 1900 字记录/记忆规则之间，
    //   弱模型对规则 blob 只回「收到」「我来」应付、不动手——加【用户本轮请求】定界与
    //   反应付指令，全后端生效）。
    QString sPrompt = QStringLiteral(
        "【用户本轮请求】\n%1\n\n"
        "（以上为本轮唯一任务：需要动手的请用工具实际执行并给出结果，"
        "禁止只用「收到」「我来」等确认语应付。）").arg(a_sText);
    const QString sGuide = m_pPm->ReadGuide();
    if (!sGuide.isEmpty()) {
        const QString sHash = QString::fromLatin1(
            QCryptographicHash::hash(sGuide.toUtf8(), QCryptographicHash::Sha256).toHex());
        const bool bNeedFeed = pS->sSid.isEmpty() || sHash != pS->sGuideHash;
        if (bNeedFeed) {
            pS->sGuideHash = sHash;
            emit sigAdhocStateChanged(a_sAsstId);   // 哈希变化 → 存盘 assistant.json
            const bool bFirst = pS->sSid.isEmpty();
            if (bFirst) {
                // 首次新建会话：仍全文投喂（保证模型一定拿到工程认知，一次性成本）。
                sPrompt = QStringLiteral(
                    "以下是当前工程的项目指南（guide.md）。请先阅读以了解项目基本情况，再处理用户请求："
                    "\\n---\\n%1\\n---\\n\\n%2").arg(sGuide, sPrompt);
            } else {
                // 2026.08.27 变更重投瘦身（大工程助手每步极慢主因之一）：只投一句话让
                //   agent 用 Read 自取——读多少缓存多少，与 Claude Code 按需探索同口径。
                sPrompt = QStringLiteral(
                    "【项目指南已更新】.autodev/guide.md 自上次阅读后有变化，"
                    "请先用 Read 工具读取 .autodev/guide.md 同步项目认知，"
                    "再处理用户请求。\\n\\n\\n%1").arg(sPrompt);
            }
        }
    }
    // 需求4：非只读会话强制 agent 自治写 record.md——实际修改源码后必须追加一行，
    //   作为 guide.md 增量同步的触发信号与输入（sTools 已放行 Write/Edit）。
    //   2026.09.07 v2.1.124 防带偏补句（实测：回灌历史里出现过追加 record.md 的操作 +
    //   本规则的具体步骤描述，模型把「追加记录」本身当任务执行、无视真实提问）：
    //   明示本规则是条件性约束不是任务。
    if (a_iPerm != PermReadOnly) {
        sPrompt.append(QStringLiteral(
            "\\n\\n---\\n【以下为附加工程约定（背景约束、不是任务——仅在满足各自条件时遵守，"
            "不要主动执行，更不要以确认语代替处理上面的用户请求）】\\n【强制记录规则】如果你在本次回答中实际修改了工程源码文件，结束前必须向 "
            "`.autodev/record.md` **追加**恰好一行，格式："
            "`[yyyy-MM-dd HH:mm] 助手 | <简述改了哪些文件/做了什么>`（时间用 `date` 命令取当前时刻）。"
            "必须用 Edit 工具在文件末尾追加（先 Read 再 Edit 末尾行），**严禁用 Write 覆盖整个 record.md**。"
            "纯查询/只读操作不要写。此记录是项目指南（guide.md）自动同步的依据。"
            "**本规则是条件性约束、不是本轮任务：仅当你本轮确实改动了源码才适用；"
            "绝不要把「追加记录」本身当作要执行的工作，也不要因历史对话中出现过该操作"
            "而主动执行。**"));
    }
    // 2026-09-11 权限三态指令（下拉框替代原只读勾选）：手工审批/只读轮在提示词层
    //   注入对应强制约束（自动审批不注入任何额外文字，保持零干扰）——手工审批是
    //   唯一的软约束层（deny 名单无法表达「先问再做」）；只读指令同时补齐
    //   dsh/codex/hermes 后端（它们忽略 deny 名单，只认提示词）。
    if (a_iPerm == PermManual) {
        sPrompt.append(QStringLiteral(
            "\n\n---\n【手工审批模式（本轮强制）】你在执行任何会改动工程文件或系统状态的操作"
            "（Edit/Write、创建/删除/重命名文件、非只读的 Bash 命令等）之前，必须先征得用户"
            "明确同意，二选一：\n"
            "① 有 ask_user 工具（Claude Code 后端）→ 直接调用它向用户提问确认；\n"
            "② 其他后端 → 运行以下命令弹出确认对话框（会弹出窗口，用户作答回写到 stdout，"
            "退出码 0=已答复 / 2=取消 / 3=超时）：\n"
            "   bash:  \"$AUTOCODE_ASK_EXE\" --ask 600 \"你要确认的问题\"\n"
            "   cmd:   %AUTOCODE_ASK_EXE% --ask 600 \"你要确认的问题\"\n"
            "答复内容表示拒绝、或退出码非 0 时不得执行，并按用户意见调整方案；"
            "未经同意严禁先执行后汇报；纯查询/只读操作无需审批。"));
    } else if (a_iPerm == PermReadOnly) {
        sPrompt.append(QStringLiteral(
            "\n\n---\n【只读模式（本轮强制）】仅允许查询类操作（Read/Grep/Glob 等）与纯分析，"
            "禁止修改、创建、删除任何文件，禁止执行任何有改动的命令；直接输出分析结论与建议，"
            "不要尝试调用写入类工具。"));
    }
    // 2026.09.09 v2.1.134 任务信息瘦身（用户需求：选中任务也只提示、不注入全文——
    //   长记忆+长规则夹在提示词里会稀释用户请求的权重、带偏模型）：仅一行可选指引，
    //   是否 Read 取用由助手自行决定（与 guide.md 变更重投的“按需自取”同口径）；
    //   非只读轮把经验沉淀闭环压进同一行（条件性一句：改了源码才追加，纯查询不写）。
    //   memory.md 读写链路不变（读=助手按需自取，写=末尾追加一行）。
    if (!a_sTaskId.isEmpty() && m_pPm != nullptr) {
        const bool bHasMemory = QFile::exists(m_pPm->TaskMemoryFile(a_sTaskId));
        if (bHasMemory || a_iPerm != PermReadOnly) {   // 只读轮且无记忆文件：连指引也不加，零干扰
            QString sTaskHint = QStringLiteral(
                "\n\n（可选）当前选中任务 id=%1，任务经验文件为 `.autodev/tasks/%1/memory.md`")
                .arg(a_sTaskId);
            if (bHasMemory) {
                sTaskHint += QStringLiteral("，如对你有帮助可先 Read 再动手");
            }
            if (a_iPerm != PermReadOnly) {
                sTaskHint += QStringLiteral(
                    "；若本轮实际修改了源码或有新发现，结束前向该文件末尾追加一行"
                    " `- [助手] <一条经验>`（先 Read 再 Edit，文件不存在可首次 Write 创建，"
                    "勿整文件覆盖，纯查询不写）");
            }
            sTaskHint += QStringLiteral("。");
            sPrompt.append(sTaskHint);
        }
    }
    // —— 统一备份（方案 §6.1/§6.3）：非只读轮（自动/手工审批均可能改动）投递前快照 + 回退告知注入 ——
    if (a_iPerm != PermReadOnly) {
        const QString sOwner = a_sTaskId.isEmpty() ? QStringLiteral("global") : a_sTaskId;
        int iNoticeTarget = 0;
        QString sNoticeOwner;
        // owner 校验：notice 只由同 owner（任务/global）的下一轮消费，防跨档误吞。
        if (m_pPm->LoadPendingNotice(iNoticeTarget, sNoticeOwner)
            && sNoticeOwner == sOwner) {
            sPrompt.prepend(QStringLiteral(
                "【系统回退通知】工作目录已回退到版本 #%1：此前对话中描述的文件修改已不存在于磁盘。"
                "请以当前磁盘内容为准，不要基于已被回退的修改继续工作。\\n---\\n").arg(iNoticeTarget));
            m_pPm->ClearPendingNotice();
        }
        AsyncAdhocBackup(a_sAsstId, sOwner, a_sText);
    }
    // 2026.09.26 专家助手深度模式（用户需求：ExpertAssistant 开启 + 非只读轮自动执行
    //   深度链，用 token 换质量）：规划Agent 先分析请求制定计划+验收标准 → 执行轮照计划
    //   实施 → 验证Agent 独立编译/测试核验 → 未过自动回炉（上限 MaxIter）→ 回炉耗尽
    //   自动重启整链（上限 [Adhoc] ExpertRestarts）。关闭=纯对话模式（原行为直投）。
    //   g_iAdhocDeepOverride（测试桩）：-1=按配置；0/1=强制关/开。
    //   2026.10.02 查询类请求跳过整链（IsAdhocQueryPrompt）：查询/咨询不需要规划与
    //   验证步骤，直投纯对话行为；深链辅助轮（规划/验证）的 assistant 输出压缩上屏
    //   （CompressDeepAuxText）——结论一眼可见、验证长文不再夹在中间刷屏。
    {
        const int iDeep = (g_iAdhocDeepOverride >= 0)
            ? g_iAdhocDeepOverride
            : ((a_iPerm != PermReadOnly && m_pPm != nullptr
                    && m_pPm->LoadExpertAssistant()) ? 1 : 0);
        if (iDeep == 1 && IsAdhocQueryPrompt(a_sText)) {
            // 2026.10.02 查询类请求跳过深度链（用户需求：查询功能不需要验证步骤）：
            //   规划/验证是改码质量保障，对查询纯属干扰（多两轮 LLM 往返+验证长文
            //   刷屏）；直投原行为（纯对话模式），事件行留痕。
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_skip_query"),
                QStringLiteral("ℹ 查询类请求，跳过专家模式深度链（直接回答）"));
        } else if (iDeep == 1) {
            pS->bDeepActive = true;
            pS->iDeepRetry = 0;
            pS->iDeepRestart = 0;
            pS->sDeepPlan.clear();
            pS->sDeepFailSummary.clear();
            pS->sDeepExecPrompt = sPrompt;
            StartDeepPlan(*pS);
            return;
        }
    }
    pS->bDeepActive = false;
    StartAdhocStage(*pS, sPrompt, /*a_bDropLastUser=*/true);
}

void CTaskOrchestrator::UndoAdhoc(const QString& a_sAsstId) {
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        return;
    }
    // 守卫补崩溃宽限/重试等待（与 Compact/Submit 同口径）：此时 IsRunning()==false
    //   但 runner 仍被占用，放行撤销会在宽限轮询/重试落地时弹栈换基点，
    //   与仍在途的本轮收尾（压栈/事件回放）互踩出孤儿块。
    if (pS->pRunner != nullptr
        && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
            || pS->pRunner->IsRetryPending())) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("该助手运行中，无法撤销"));
        return;
    }
    pS->aPendingInjects.clear();   // 撤销回退链：未发出的排队指令一并作废
    emit sigAdhocPendingDropped(a_sAsstId);   // 2026.09.18 面板暂存框同步作废
    if (pS->aStack.isEmpty()) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("没有可撤销的提问"));
        return;
    }
    pS->aStack.removeLast();
    // 2026.09.02 回灌缓存同步撤销：弹掉末尾的助手行与最后一条用户行（与栈弹一一对应）。
    while (!pS->aTranscript.isEmpty()
           && pS->aTranscript.last().startsWith(QStringLiteral("助手："))) {
        pS->aTranscript.removeLast();
    }
    if (!pS->aTranscript.isEmpty()
        && pS->aTranscript.last().startsWith(QStringLiteral("用户："))) {
        pS->aTranscript.removeLast();
    }
    // 回退到父 sid：栈空则回到无会话态，否则取新栈顶的 sid 作为链基点。
    pS->sSid = pS->aStack.isEmpty() ? QString() : pS->aStack.last().second;
    emit sigAdhocStateChanged(a_sAsstId);   // #1 撤销链变化 → 存盘
    emit sigAdhocUndo(a_sAsstId);
    // 撤销后无法精确还原 fill：栈空显示 0，否则置 -1（面板显示「—」），下次提问刷新。
    emit sigAdhocContext(a_sAsstId, pS->aStack.isEmpty() ? 0 : -1);
}

// —— 助手会话链持久化（#1）：sid + (prompt,sid) 撤销栈的存取 ——
void CTaskOrchestrator::CompactAdhoc(const QString& a_sAsstId) {
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        return;
    }
    // 占用判定含宽限/重试等待（与 SubmitAdhoc 同口径）。
    if (pS->pRunner != nullptr
        && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
            || pS->pRunner->IsRetryPending())) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("该助手运行中，无法压缩"));
        return;
    }
    if (pS->sSid.isEmpty()) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("warn"),
            QStringLiteral("还没有对话，无需压缩"));
        return;
    }
    // /compact 在 headless -p 无效 → 走「摘要→开新会话重播种」两步。
    // 第一步：在当前 sid 的 fork 分支上生成摘要（fork 不污染原 sid），result 作摘要。
    pS->eMode = CompactSummarize;
    emit sigAdhocEvent(a_sAsstId, QStringLiteral("compact_start"),
        QStringLiteral("⏳ 正在总结对话以压缩上下文…"));
    StartAdhocStage(*pS, QStringLiteral(
        "请用约 300 token 精炼总结我们这次对话至今的内容，用于在新会话中恢复上下文。"
        "只输出总结正文，结构为：1) 用户意图与关键决定；2) 已查看/改动的文件；3) 待办与注意事项。"));
}

void CTaskOrchestrator::StopAdhoc(const QString& a_sAsstId) {
    // #4 用户主动停止：先发「已停止」事件（⏹）再 kill 进程，让该助手流留下停止痕迹。
    //   只停指定助手（其余助手并发轮不受影响）。
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS != nullptr && pS->pRunner != nullptr
        && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
            || pS->pRunner->IsRetryPending())) {
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("stopped"),
            QStringLiteral("⏹ 已停止"));
        pS->pRunner->Stop();
    }
    if (pS != nullptr) {
        pS->eMode = Idle;
        pS->sPendingPrompt.clear();
        pS->bDeepActive = false;   // 2026.09.26 深度链中断：停止后不再回炉/重启
    }
    emit sigAdhocFinished(a_sAsstId);
}

void CTaskOrchestrator::StopAllAdhoc() {
    // 2026.09.03 档切换/切工程/关闭前：整组停（⏹ 痕迹各自落自己的流，供旧档落盘）。
    QList<QString> aIds = m_aAdhoc.keys();
    for (const QString& sId : aIds) {
        StopAdhoc(sId);
    }
}

bool CTaskOrchestrator::IsAsstAdhocRunning(const QString& a_sAsstId) const {
    // 2026.08.15 占用判定含宽限/重试等待：此时进程未跑但 runner 仍被占用（UI 停止按钮等依此判定）。
    // 2026.09.03 多助手并发：指定助手看其会话 runner；任一助手遍历会话表。
    if (const SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr)) {
        return pS->pRunner != nullptr
            && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
                || pS->pRunner->IsRetryPending());
    }
    return false;
}

QStringList CTaskOrchestrator::RunningAdhocIds() const {
    // 2026.09.18 退出确认提示用：占用判定与 IsAsstAdhocRunning 同口径
    //   （含宽限/重试等待——进程未跑但 runner 仍被占用也在列）。
    QStringList aIds;
    for (auto it = m_aAdhoc.cbegin(); it != m_aAdhoc.cend(); ++it) {
        const SAdhocSession* pS = it.value();
        if (pS != nullptr && pS->pRunner != nullptr
                && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
                    || pS->pRunner->IsRetryPending())) {
            aIds.append(it.key());
        }
    }
    return aIds;
}

bool CTaskOrchestrator::IsAnyAdhocRunning() const {
    for (auto it = m_aAdhoc.cbegin(); it != m_aAdhoc.cend(); ++it) {
        const SAdhocSession* pS = it.value();
        if (pS->pRunner != nullptr
            && (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
                || pS->pRunner->IsRetryPending())) {
            return true;
        }
    }
    return false;
}

// —— 会话存取（2026.09.03 多助手并发）——
CTaskOrchestrator::SAdhocSession* CTaskOrchestrator::AdhocSess(const QString& a_sAsstId,
                                                               bool a_bCreate) {
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr && a_bCreate && !a_sAsstId.isEmpty()) {
        pS = new SAdhocSession;
        pS->sId = a_sAsstId;
        m_aAdhoc.insert(a_sAsstId, pS);
    }
    return pS;
}

void CTaskOrchestrator::LoadAsstAdhoc(const QString& a_sAsstId, const QString& a_sSid,
                                      const QJsonArray& a_aStack, const QString& a_sGuideHash,
                                      const QString& a_sBackend) {
    // 助手组恢复（不发信号）：直接覆盖该助手会话链；runner 保持懒建（首跑时 Ensure）。
    //   2026.09.20 粘性后端：档内 per-assistant backend 随会话装入（空=未绑定）。
    SAdhocSession* pS = AdhocSess(a_sAsstId, /*a_bCreate=*/true);
    pS->sSid  = a_sSid;
    pS->sGuideHash = a_sGuideHash;
    pS->sBackend = a_sBackend;
    pS->aStack.clear();
    for (const QJsonValue& v : a_aStack) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            pS->aStack.append({ o.value(QStringLiteral("prompt")).toString(),
                                o.value(QStringLiteral("sid")).toString() });
        }
    }
}

void CTaskOrchestrator::RemoveAsstAdhoc(const QString& a_sAsstId) {
    // 删除/丢弃助手：停其在跑 runner（UI 已挡运行中删除，此处兜底）并释放会话。
    SAdhocSession* pS = m_aAdhoc.take(a_sAsstId);
    if (pS == nullptr) {
        return;
    }
    if (pS->pRunner != nullptr) {
        pS->pRunner->Stop();
        pS->pRunner->deleteLater();
    }
    delete pS;
}

void CTaskOrchestrator::ResetAdhocSessions(const QSet<QString>& a_aKeep) {
    // 档切换/切工程：整组停 + 释放（⏹ 痕迹由调用方在落盘前经 StopAllAdhoc 留下；
    //   此处兜底 Stop 防孤儿进程）。新档助手经 LoadAsstAdhoc 重装。
    //   2026-10-02 a_aKeep：跨档幽灵保留集——外档运行中助手的会话/runner 原样保留
    //   （不停、不删，轮次继续；面板以其 id 收流），仅清非保留会话。
    for (auto it = m_aAdhoc.begin(); it != m_aAdhoc.end();) {
        SAdhocSession* pS = it.value();
        if (a_aKeep.contains(it.key())) {
            ++it;
            continue;
        }
        if (pS->pRunner != nullptr) {
            if (pS->pRunner->IsRunning() || pS->pRunner->IsGraceActive()
                || pS->pRunner->IsRetryPending()) {
                pS->pRunner->Stop();
            }
            pS->pRunner->deleteLater();
        }
        delete pS;
        it = m_aAdhoc.erase(it);
    }
}

QString CTaskOrchestrator::MergeQueuedPrompts(const QStringList& a_aPrompts) {
    // 2026.09.20 多条排队指令 → 一次投送封装：定界防短指令被规则噪音稀释（同 v2.1.130
    //   【用户本轮请求】定界思路），按序全部执行完毕再统一收尾（防弱模型只挑部分执行）。
    QString sMerged = QStringLiteral(
        "用户在助手运行期间陆续提交了 %1 条排队指令。请在本轮一次性全部处理："
        "按【指令 1】→【指令 %1】的顺序逐条执行完毕，全部完成后再统一收尾总结；"
        "不要只执行其中一部分，也不要要求用户重发。").arg(a_aPrompts.size());
    for (int i = 0; i < a_aPrompts.size(); ++i) {
        sMerged += QStringLiteral("\n\n【指令 %1】\n%2")
                       .arg(i + 1)
                       .arg(a_aPrompts.at(i));
    }
    return sMerged;
}

bool CTaskOrchestrator::RemoveQueuedAdhoc(const QString& a_sAsstId, const QString& a_sText) {
    // 2026.09.20 删除未发送的排队指令（面板暂存框 ✕/右键）：与 aPendingInjects 入队顺序
    //   同源，按原文本匹配首个即面板暂存框对应行；已被合并消费（轮已起）→ 不命中。
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        return false;
    }
    const int iIdx = pS->aPendingInjects.indexOf(a_sText);
    if (iIdx < 0) {
        return false;
    }
    pS->aPendingInjects.removeAt(iIdx);
    return true;
}

bool CTaskOrchestrator::EditQueuedAdhoc(const QString& a_sAsstId, const QString& a_sOldText,
                                        const QString& a_sNewText) {
    // 2026.09.20 改写未发送的排队指令（面板暂存框编辑）：同文本匹配；新文本空白拒改
    //   （面板侧已拦，双保险）；续发时按改写后的文本投送。
    if (a_sNewText.trimmed().isEmpty()) {
        return false;
    }
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        return false;
    }
    const int iIdx = pS->aPendingInjects.indexOf(a_sOldText);
    if (iIdx < 0) {
        return false;
    }
    pS->aPendingInjects[iIdx] = a_sNewText;
    return true;
}

// 2026.09.20 该助手绑定的后端名（粘性；"" = 未绑定——首次提交时才绑当前全局）。
QString CTaskOrchestrator::AsstAdhocBackend(const QString& a_sAsstId) const {
    const SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    return pS ? pS->sBackend : QString();
}

QString CTaskOrchestrator::AsstAdhocSid(const QString& a_sAsstId) const {
    const SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    return pS ? pS->sSid : QString();
}

QString CTaskOrchestrator::AsstAdhocGuideHash(const QString& a_sAsstId) const {
    const SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    return pS ? pS->sGuideHash : QString();
}

QJsonArray CTaskOrchestrator::AsstAdhocStackToJson(const QString& a_sAsstId) const {
    QJsonArray aArr;
    if (const SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr)) {
        for (const QPair<QString, QString>& pr : pS->aStack) {
            QJsonObject o;
            o[QStringLiteral("prompt")] = pr.first;
            o[QStringLiteral("sid")]    = pr.second;
            aArr.append(o);
        }
    }
    return aArr;
}

void CTaskOrchestrator::OnAdhocInfo(const QString& a_sAsstId, const QString& a_sLine) {
    // 过滤 system 噪音（与任务日志一致），其余原样上抛「助手」Tab（带产出助手 id）。
    if (a_sLine.startsWith(QStringLiteral("[system"))) {
        return;
    }
    emit sigAdhocLog(a_sAsstId, a_sLine);
}

void CTaskOrchestrator::OnAdhocAssistantText(const QString& a_sAsstId, const QString& a_sText) {
    if (SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr)) {
        // 2026.10.02 深链辅助轮（规划/验证）输出压缩上屏（用户需求：验证日志别刷屏、
        //   结论一眼可见）：≤360 字原样，超长保首 300 字+折叠尾注（结论由事件判定行
        //   呈现）；不记 sLastAsstText（纠偏判据只认执行轮终答）；不记 aTranscript
        //   （fork 一次性分支文本入回灌历史会带偏下一轮，同 v2.1.124 连发只留末条
        //   的口径——顺带修复旧版把规划/验证长文也追加进回灌历史的带偏隐患）。
        if (pS->eMode == DeepPlan || pS->eMode == DeepVerify) {
            const QString sCompressed = CompressDeepAuxText(a_sText);
            if (!sCompressed.isEmpty()) {
                emit sigAdhocAssistantText(a_sAsstId, sCompressed);
            }
            return;
        }
        pS->sLastAsstText = a_sText;   // 2026.09.09 v2.1.131 纠偏判据：终答=最后一条
    }
    emit sigAdhocAssistantText(a_sAsstId, a_sText);
    // 2026.09.02 回灌记录：轮内 assistant 输出记入缓存，供每轮独立会话的后端
    //   下一轮回灌（claude/pi 原生续接用不到）；下次外部提交时面板会整体重建
    //   权威记录，此处追加只为覆盖内部续发路径（排队指令/max-turns 续跑）。
    //   单行 4000 字截断与面板同口径。
    if (a_sText.trimmed().isEmpty()) {
        return;
    }
    QString sBody = a_sText;
    if (sBody.size() > 4000) {
        sBody = sBody.left(4000) + QStringLiteral(" …[截断]");
    }
    if (SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr)) {
        AppendAdhocTranscriptLine(*pS, QStringLiteral("助手：") + sBody);
    }
}

// —— 2026.09.02 跨轮上下文回灌：记录缓存构建/追加 ——
QString CTaskOrchestrator::BuildAdhocHistoryBlock(const SAdhocSession& a_S,
                                                  bool a_bDropLastUser) const {
    // 后端原生续接（claude/pi）或尚无会话链（首条提问/Compact 重播种前）→ 不回灌。
    if (a_S.pRunner == nullptr || a_S.pRunner->KeepsContextAcrossTurns()
        || a_S.sSid.isEmpty()) {
        return QString();
    }
    QStringList aLines = a_S.aTranscript;
    if (a_bDropLastUser && !aLines.isEmpty()
        && aLines.last().startsWith(QStringLiteral("用户："))) {
        aLines.removeLast();   // 本轮提问 prompt 尾部已含原文，剔除防重复
    }
    if (aLines.isEmpty()) {
        return QString();
    }
    // 2026.09.07 v2.1.124 回灌结构防带偏（实测 .claude 工程：用户问「你现在用的是什么
    //   模型？」，历史里上一轮的 record.md 追加叙述 + 尾部【强制记录规则】的具体操作
    //   步骤让模型把「追加记录」当成现行任务执行、完全无视提问）：①前置声明强调历史仅
    //   背景、已完成任务不重复执行；②历史后用【最新指令】明确本轮作答对象，把短提问从
    //   噪音里拉出来。实际 prompt = 本块 + "\n\n" + 用户 prompt（含规则尾部）。
    return QStringLiteral(
        "【历史对话回灌】当前后端每轮为独立会话，无法原生延续此前的对话上下文。"
        "以下是你（助手）与本用户最近的真实对话记录（时间升序，个别长条目已截断），"
        "请将其视为你亲历的连续对话。**历史仅作背景参考：其中已执行完毕的任务/操作一律"
        "不要重复执行**。\n---\n%1\n---\n【最新指令】下面才是本轮要处理的用户提问，"
        "请直接围绕它作答；除非提问本身要求延续历史事项，否则不要执行历史中出现过的"
        "任何操作。")
        .arg(aLines.join(QLatin1Char('\n')));
}

void CTaskOrchestrator::AppendAdhocTranscriptLine(SAdhocSession& a_S, const QString& a_sLine) {
    // 预算：总字符 cap 24000（约 6~8k token），超额自头部丢最旧；单行截断在
    //   追加前已完成（面板/调用点各自 1500/4000 字口径）。
    constexpr int k_iMaxTotalChars = 24000;
    a_S.aTranscript.append(a_sLine);
    int iTotal = 0;
    for (const QString& sLine : a_S.aTranscript) {
        iTotal += sLine.size();
    }
    while (a_S.aTranscript.size() > 1 && iTotal > k_iMaxTotalChars) {
        iTotal -= a_S.aTranscript.first().size();
        a_S.aTranscript.removeFirst();
    }
}

void CTaskOrchestrator::OnAdhocToolUse(const QString& a_sAsstId, const QString& a_sTool,
                                       const QString& a_sInput) {
    if (SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr)) {
        pS->bTurnSawTool = true;   // 2026.09.09 v2.1.131 纠偏判据：本轮有真实工具工作
    }
    emit sigAdhocToolUse(a_sAsstId, a_sTool, a_sInput);
}

void CTaskOrchestrator::OnAdhocToolResult(const QString& a_sAsstId, const QString& a_sResult) {
    emit sigAdhocToolResult(a_sAsstId, a_sResult);
}

void CTaskOrchestrator::OnAdhocFinished(const QString& a_sAsstId, bool a_bOk,
                                        const QString& a_sResult,
                                        qint64 a_iIn, qint64 a_iOut,
                                        double a_dCost, const QString& a_sSid) {
    // 2026.09.03 多助手并发：状态机全部走该助手自己的会话（pS）；消耗归集带助手 id。
    //   语义与单会话版逐分支一致（详见各分支原注释）。
    SAdhocSession* pS = m_aAdhoc.value(a_sAsstId, nullptr);
    if (pS == nullptr) {
        // 2026-10-02 修「busy 永卡」（MeloTTS 实例：轮收尾时档已切、会话被删，此分支静默
        //   丢弃 finished——面板 SetAsstRunning(true) 再无人清除：状态图标转不停、Tab 闪点、
        //   输入栏停「追加指令」直到重启）。面板按 id 清运行态（找不到也无害）；进程侧已由
        //   ResetAdhocSessions 的 Stop 兜底，无孤儿风险。
        emit sigAdhocFinished(a_sAsstId);
        return;   // 会话已删（档切换兜底竞态）：丢弃
    }
    if (a_bOk) {
        const qint64 iElapsedMs = pS->dtStart.isValid()
            ? pS->dtStart.msecsTo(QDateTime::currentDateTime()) : 0;
        // 2026.08.27 runner 已逐消息实时上抛（sigUsageDelta）时收尾报 0/0（防任务侧
        //   双计），此处回退用本轮实时累计值归集——助手消耗统计不丢失。
        // 2026.09.05 per-助手：回退取该会话自己的 iLive*（旧全局 m_iLive* 在并发
        //   助手下会把别人的计数算进来——混计修复）。
        qint64 iIn = a_iIn, iOut = a_iOut;
        if (iIn == 0 && iOut == 0 && (pS->iLiveIn > 0 || pS->iLiveOut > 0)) {
            iIn  = pS->iLiveIn;
            iOut = pS->iLiveOut;
        }
        emit sigAdhocUsage(a_sAsstId, iIn, iOut, a_dCost, iElapsedMs,
                           pS->pRunner ? pS->pRunner->LastReqCount() : 0);
    }
    // 归集完成后才清零（顺带 sigAdhocLiveUsage(id,0,0) 收起该助手页内「本轮」显示；
    //   2026.09.05 per-助手：不再 ResetLiveUsage 全局清零——任务侧实时计量不受助手收尾影响）。
    pS->iLiveIn  = 0;
    pS->iLiveOut = 0;
    emit sigAdhocLiveUsage(a_sAsstId, 0, 0);
    pS->dtStart = QDateTime();

    switch (pS->eMode) {
    case AutoNaming: {
        // 2026.08.20 (Agent 起名)：只取结果首行清洗为名称——不动用户会话栈、不推进
        //   pS->sSid（fork 出的分支一次性使用）、不触发 guide 增量同步。
        CTaskInfo* pTask = pS->pAutoNameTask;
        pS->pAutoNameTask = nullptr;
        pS->eMode = Idle;
        pS->sPendingPrompt.clear();
        emit sigAdhocFinished(a_sAsstId);   // 助手面板退出运行态（命名过程会短暂占用输入栏）
        if (pTask == nullptr) { break; }
        if (!a_bOk) {
            StageEvent(pTask, QStringLiteral("auto_name_failed"),
                QStringLiteral("✗ Agent 自动命名失败：%1（任务名保持不变）")
                    .arg(a_sResult.left(120)));
            emit sigAutoNameDone(pTask, false, QString());
            break;
        }
        // 清洗：首行 → CTaskInfo::TrimNamePunct 剥两侧引号/书名号/Markdown 标记与
        //   句尾标点（2026.09.21 补尾冒号：「大模型 Harness网关插件:」实害——旧
        //   reNoise 尾集没有 ：:）→ 截 24 字。
        QString sName = a_sResult.section(QLatin1Char('\n'), 0, 0).trimmed();
        sName = CTaskInfo::TrimNamePunct(sName);
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
    case AsstAutoNaming: {
        // 2026-09-22 助手自动重命名：两行输出——第一行名称（≤10 字，剥标点），其余行
        //   合成一句说明。不动用户会话栈、不推进 pS->sSid（fork 分支一次性使用）。
        const QString sTarget = pS->sAutoNameTargetAsst;
        pS->sAutoNameTargetAsst.clear();
        pS->eMode = Idle;
        pS->sPendingPrompt.clear();
        emit sigAdhocFinished(a_sAsstId);   // 借用的执行会话退出运行态
        if (sTarget.isEmpty()) { break; }
        if (!a_bOk) {
            emit sigAsstAutoNameDone(sTarget, false, QString(),
                a_sResult.left(120));
            break;
        }
        QString sName = a_sResult.section(QLatin1Char('\n'), 0, 0).trimmed();
        sName = CTaskInfo::TrimNamePunct(sName);
        sName = sName.simplified();
        if (sName.size() > 10) { sName = sName.left(10).trimmed(); }
        if (sName.size() < 2) {
            emit sigAsstAutoNameDone(sTarget, false, QString(),
                QStringLiteral("Agent 未给出有效名称"));
            break;
        }
        // 说明：第二行起到结尾合并为一段（模型偶发第三行补充也收编），剥两侧引号/尾标点，截 80 字。
        QString sDesc = a_sResult.section(QLatin1Char('\n'), 1).simplified();
        sDesc = CTaskInfo::TrimNamePunct(sDesc);
        if (sDesc.size() > 80) { sDesc = sDesc.left(80).trimmed(); }
        emit sigAsstAutoNameDone(sTarget, true, sName, sDesc);
        break;
    }
    case Submitting: {
        if (!a_bOk) {
            // ad-hoc max-turns 误报修复（7ce53172 实测）：--max-turns 40 打满时
            //   is_error=true 且 result 为空 → 旧代码显示「✗ 助手调用失败：」空白，且丢弃断点
            //   sid（用户「继续」只能从旧 sid fork，丢掉已跑的轮次）。max-turns 非真错误：
            //   先自动 --resume 续跑（上限 2 次）；耗尽也不判死——压栈保住断点，提示发「继续」接续。
            if (pS->pRunner != nullptr && pS->pRunner->LastWasMaxTurns()
                && !a_sSid.isEmpty()) {
                pS->sSid = a_sSid;   // 链基点推进到断点 sid：后续 fork 从断点续，不丢已完成轮次
                if (pS->iResume < 2) {
                    ++pS->iResume;
                    emit sigAdhocEvent(a_sAsstId, QStringLiteral("resume"),
                        QStringLiteral("⚠ 达到轮次上限，已完成部分已落盘，自动续跑（第 %1/2 次）")
                            .arg(pS->iResume));
                    StartAdhocStage(*pS, QStringLiteral(
                        "继续完成未完成的工作，从上次中断处接着做；不要重复已完成的改动。"));
                    return;   // 保持 Submitting 模式与 pendingPrompt，续跑成功后正常压栈
                }
                // 续跑预算耗尽：深度链 → 重启判定（2026.09.26 用户需求：重试导致停止
                //   时自动重启，重规划携带教训）；纯对话模式 → 压栈断点（本条提问至此
                //   告一段落），交还用户接续。
                if (pS->bDeepActive) {
                    DeepChainRestart(*pS,
                        QStringLiteral("执行轮次上限耗尽（含自动续跑 2 次）"),
                        pS->sLastAsstText.left(600));
                    return;
                }
                pS->aStack.append({ pS->sPendingPrompt, a_sSid });
                emit sigAdhocStateChanged(a_sAsstId);   // 链推进 → 存盘
                pS->eMode = Idle;
                pS->sPendingPrompt.clear();
                emit sigAdhocEvent(a_sAsstId, QStringLiteral("stage_failed"),
                    QStringLiteral("⚠ 达到轮次上限，已自动续跑 2 次仍未收敛；会话已保留，发送「继续」可接着做"));
                emit sigAdhocContext(a_sAsstId, -1);
                emit sigAdhocFinished(a_sAsstId);
                return;
            }
            // 2026.09.09 v2.1.132 上下文超限自愈（实害：pi 会话堆积超窗口，pi 自身
            //   不自动压缩——01:42 实测 ctx=60000 时 66888 token 请求照发 → 400）：
            //   识别 exceeds the available context size / exceed_context_size
            //   → 自动走压缩两步（摘要→重播种）后重发原提问，每条限一次。
            if ((a_sResult.contains(QLatin1String("exceeds the available context size"),
                                   Qt::CaseInsensitive)
                 || a_sResult.contains(QLatin1String("exceed_context_size"),
                                        Qt::CaseInsensitive))
                && !pS->sSid.isEmpty() && !pS->bAutoCompactDone) {
                pS->bAutoCompactDone = true;
                pS->bAutoCompactActive = true;
                emit sigAdhocEvent(a_sAsstId, QStringLiteral("compact_start"),
                    QStringLiteral("⚠ 上下文超出模型窗口，自动压缩对话后重发原请求…"));
                pS->eMode = CompactSummarize;
                // 2026.09.09 v2.1.133 关键：摘要请求发往【全新会话】并内联对话记录
                // （aTranscript 缓存，24k 字）——原会话已超窗，摘要请求在其上必再 400
                // （v2.1.132 实害：01:53「未能生成摘要」即此）。暂存原 sid 供失败回退。
                pS->sCompactBaseSid = pS->sSid;
                pS->sSid.clear();
                const QString sRecap = pS->aTranscript.isEmpty()
                    ? QStringLiteral("(此前对话的文字记录不可用)")
                    : pS->aTranscript.join(QChar::LineFeed).left(20000);
                StartAdhocStage(*pS, QStringLiteral(
                    "以下是与用户此前对话的文字记录（部分截断）。请用约 300 token 精炼总结，"
                    "用于在新会话中恢复上下文。只输出总结正文，结构为：1) 用户意图与关键决定；"
                    "2) 已查看/改动的文件；3) 待办与注意事项。\n---\n%1\n---").arg(sRecap));
                return;
            }
            // 2026.09.26 深度链内执行轮硬失败（上下文超限自愈已在前面 return）：同样进
            //   重启判定（重规划带上错误教训；重启预算耗尽由 DeepChainRestart 收尾）；
            //   纯对话模式保持原「报错即停」。
            if (pS->bDeepActive) {
                DeepChainRestart(*pS, QStringLiteral("执行轮失败"), a_sResult.left(600));
                return;
            }
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("stage_failed"),
                QStringLiteral("✗ 助手调用失败：%1").arg(a_sResult.left(200)));
            // #6 失败（崩溃/异常退出/返回 is_error）时也刷新上下文按钮：发 -1 显示「—」，
            //   明确表示本轮未正常完成、占用未知，而非停留在误导性的「0」。
            emit sigAdhocContext(a_sAsstId, -1);
            pS->eMode = Idle;
            pS->sPendingPrompt.clear();
            emit sigAdhocFinished(a_sAsstId);
            return;
        }
        // 2026.09.09 v2.1.131 确认语应付自动纠偏（实害：本地 spark2.5-4b 收到规则重的
        //   prompt 只回「收到」「我来」零工具零结果——v2.1.130 请求定界后仍复发，模型能力
        //   层短板靠机制兜底）：本轮成功但 ①非只读 ②全程零工具 ③终答 1~6 字 ④本条提问
        //   未纠偏过 → 同会话自动追发执行指令重试一轮（口径仿 max-turns 续跑：推进 sid、
        //   不压栈、保持 Submitting；每用户提问限一次，SubmitAdhoc 重置）。
        const QString sTrimAsst = pS->sLastAsstText.trimmed();
        if (pS->iPerm != PermReadOnly && !pS->bNudgeUsed && !pS->bTurnSawTool
                && !sTrimAsst.isEmpty() && sTrimAsst.size() <= 6) {
            pS->bNudgeUsed = true;
            pS->sSid = a_sSid.isEmpty() ? pS->sSid : a_sSid;   // 链推进到本轮
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("nudge"),
                QStringLiteral("⚠ 助手仅回复了「%1」而没有执行任何工作，自动追发执行指令重试…")
                    .arg(sTrimAsst.left(12)));
            StartAdhocStage(*pS, QStringLiteral(
                "你上一条只回复了确认语而没有做任何事。请立即动手处理用户此前的请求："
                "先用工具读取相关工程文件定位问题，再实际修改并验证，最后说明结果；"
                "不要再以任何确认语结束。"));
            return;
        }
        // 2026.09.26 专家模式深度链：执行轮成功——有实际工作（本轮调用过工具）→ 起
        //   独立验证轮（fork 一次性分支，只验证不修复）；纯问答（零工具）→ 跳过验证
        //   直接收尾。链基点先推进到执行轮 sid（验证/回炉/重启均从它 fork；压栈留待
        //   链尾统一做）。纯对话模式行为不变。
        if (pS->bDeepActive) {
            if (pS->bTurnSawTool && !a_sSid.isEmpty()) {
                pS->sSid = a_sSid;
                StartDeepVerify(*pS);
                return;
            }
            if (!pS->bTurnSawTool) {
                emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_skip_verify"),
                    QStringLiteral("ℹ 本轮未调用工具（未改动工程），跳过验证"));
            }
            FinishAdhocSuccess(*pS, a_sSid);
            return;
        }
        FinishAdhocSuccess(*pS, a_sSid);
        return;
    }
    case CompactSummarize: {
        pS->sSummary = a_sResult.trimmed();
        if (!a_bOk || pS->sSummary.isEmpty()) {
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("compact_fail"),
                QStringLiteral("✗ 未能生成摘要，已取消压缩"));
            // 2026.09.09 v2.1.133 超限自愈路径摘要在新会话做：失败回退暂存的原 sid
            //   （链基点不丢，用户可手动清理后重试）。
            if (!pS->sCompactBaseSid.isEmpty()) {
                pS->sSid = pS->sCompactBaseSid;
                pS->sCompactBaseSid.clear();
            }
            pS->bAutoCompactActive = false;
            pS->eMode = Idle;
            emit sigAdhocFinished(a_sAsstId);
            return;
        }
        // 第二步：临时清空 sid → StartAdhocStage 走新建会话分支投递摘要；
        //   失败时由 CompactReseed 分支从栈顶恢复原 sid（栈在此期间未动）。
        pS->eMode = CompactReseed;
        pS->sSid.clear();
        StartAdhocStage(*pS, pS->sSummary);
        return;
    }
    case CompactReseed: {
        if (!a_bOk || a_sSid.isEmpty()) {
            // 重播种失败：回退到压缩前链基点（栈未动），保持会话连续。
            // 2026.09.09 v2.1.133 超限自愈路径优先回退暂存的原 sid（栈可能为空/陈旧）。
            pS->sSid = !pS->sCompactBaseSid.isEmpty() ? pS->sCompactBaseSid
                       : (pS->aStack.isEmpty() ? QString() : pS->aStack.last().second);
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("compact_fail"),
                QStringLiteral("✗ 压缩失败，已保留原对话"));
        } else {
            // 重置链：仅保留一个「已压缩」哨兵节点，新 sid 为基点。
            pS->aStack.clear();
            pS->aStack.append({ QStringLiteral("[已压缩上下文]"), a_sSid });
            pS->sSid = a_sSid;
            emit sigAdhocStateChanged(a_sAsstId);   // #1 链重置 → 存盘
            emit sigAdhocModel(a_sAsstId, pS->pRunner ? pS->pRunner->LastModel() : QString());
            // 2026.09.26 同上：零 usage 发 -1（「—」）不发 0。
            emit sigAdhocContext(a_sAsstId, pS->pRunner
                ? (pS->pRunner->LastContextFill() > 0 ? pS->pRunner->LastContextFill() : -1) : 0);
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("compact_done"),
                QStringLiteral("✔ 已压缩上下文"));
            // 2026.09.09 v2.1.133 旧对话记录随压缩作废（新会话由摘要起头；
            //   下次外部提交时面板按块重建权威记录）。
            pS->aTranscript.clear();
            pS->sCompactBaseSid.clear();
        }
        pS->eMode = Idle;
        emit sigAdhocFinished(a_sAsstId);
        // 2026.09.09 v2.1.132 超限自愈收尾：压缩完成后自动重发原提问（经完整
        //   SubmitAdhoc 链：定界/规则全新构建）；压缩失败则交还用户手发。
        if (pS->bAutoCompactActive) {
            pS->bAutoCompactActive = false;
            const QString sResend = pS->sPendingPrompt;
            const int iResendPerm = pS->iPerm;
            const QString sResendTask = pS->sLastTaskId;
            if (!sResend.trimmed().isEmpty()) {
                QTimer::singleShot(0, this, [this, a_sAsstId, sResend, iResendPerm, sResendTask]() {
                    SubmitAdhoc(a_sAsstId, sResend, iResendPerm, sResendTask);
                });
            }
        }
        return;
    }
    case DeepPlan: {
        // 2026.09.26 深度链第 1 步收尾：规划产出入会话缓存；失败/空产出 → 回退直投
        //   原执行提示词（纯对话行为，链继续——执行轮仍会进验证）。
        if (!a_bOk || a_sResult.trimmed().isEmpty()) {
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_plan_failed"),
                QStringLiteral("⚠ 规划轮失败（%1），跳过规划直接执行")
                    .arg(a_sResult.left(240)));
            pS->eMode = Submitting;
            StartAdhocStage(*pS, pS->sDeepExecPrompt, /*a_bDropLastUser=*/true);
            return;
        }
        pS->sDeepPlan = a_sResult.trimmed();
        if (pS->sDeepPlan.size() > 6000) {
            pS->sDeepPlan = pS->sDeepPlan.left(6000);
        }
        emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_plan_done"),
            QStringLiteral("✔ 规划完成（含验收标准），开始执行"));
        pS->eMode = Submitting;
        StartAdhocStage(*pS, pS->sDeepExecPrompt + QStringLiteral(
            "\n\n---\n【专家模式·实施计划】以下计划由规划Agent基于你的请求生成，供执行参考；"
            "执行中发现与实际不符时可合理调整，但最终须达成计划中的验收标准：\n%1")
            .arg(pS->sDeepPlan), /*a_bDropLastUser=*/true);
        return;
    }
    case DeepVerify: {
        // 2026.09.26 深度链第 3 步收尾：PASS → 链尾统一成功收尾；FAIL/无判定/验证轮
        //   自身失败 → 回炉重试（上限 MaxIter），耗尽 → DeepChainRestart 重启判定。
        QString sReasons;
        const int iVerdict = ParseDeepVerdict(a_sResult, &sReasons);
        if (iVerdict == 1) {
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_verify_pass"),
                QStringLiteral("✔ 独立验证通过，专家模式闭环完成"));
            FinishAdhocSuccess(*pS, pS->sSid);
            return;
        }
        if (sReasons.isEmpty()) {
            sReasons = (iVerdict == -1 && !a_bOk)
                ? a_sResult.left(600)
                : QStringLiteral("验证结论未按约定格式输出（无 VERIFY:PASS/FAIL 判定行）");
        }
        if (pS->iDeepRetry < MaxIter()) {
            ++pS->iDeepRetry;
            emit sigAdhocEvent(a_sAsstId, QStringLiteral("deep_retry"),
                QStringLiteral("⚠ 验证未通过，自动回炉修复（第 %1/%2 次）：%3")
                    .arg(pS->iDeepRetry).arg(MaxIter()).arg(sReasons.left(160)));
            pS->eMode = Submitting;
            StartAdhocStage(*pS, QStringLiteral(
                "【专家模式·修复轮】独立验证者报告以下问题未通过验收：\n---\n%1\n---\n"
                "请在现有改动基础上修复以上全部问题（会话已保留此前的改动与上下文，"
                "不要重做已通过的部分），对照验收标准逐项处理后，"
                "简要说明每个问题的修复方式。")
                .arg(sReasons.left(1500)));
            return;
        }
        DeepChainRestart(*pS,
            QStringLiteral("验证回炉 %1 次仍未通过").arg(MaxIter()), sReasons);
        return;
    }
    case Idle:
    default: {
        // 不应发生（无模式却收到 finished）：仅恢复空闲、启用输入栏。
        pS->eMode = Idle;
        emit sigAdhocFinished(a_sAsstId);
        return;
    }
    }
}

// —— 2026.09.26 专家助手深度模式（规划→执行→验证闭环 + 回炉/重启）——

// 深度模式运行期覆盖（测试桩，定义见 SubmitAdhoc 前）。
void CTaskOrchestrator::SetAdhocDeepOverride(int a_iMode) {
    g_iAdhocDeepOverride = a_iMode;
}

// 专家模式规划/验证轮的 --max-turns 覆盖：轻量辅助轮（只出计划/只跑验收），无需
// 执行轮的重预算；20 轮足够只读探索大工程 + 编译/测试两三轮。
static constexpr int k_iDeepAuxTurns = 20;

void CTaskOrchestrator::StartDeepPlan(SAdhocSession& a_S) {
    // 深度链第 1 步：规划轮（fork 一次性分支，不动撤销栈/sid 基点）。只读探索，
    //   产出「目标/实施步骤/验收标准」三段计划；重启时携带历史失败教训重规划。
    a_S.eMode = DeepPlan;
    emit sigAdhocEvent(a_S.sId, QStringLiteral("deep_plan"),
        QStringLiteral("⚙ 专家模式：规划Agent 先分析请求并制定实施计划与验收标准…"));
    QString sLessons;
    if (!a_S.sDeepFailSummary.isEmpty()) {
        sLessons = QStringLiteral(
            "\n---\n【历史失败教训】此前的尝试未通过独立验证，原因摘要：\n%1\n"
            "新计划必须规避或正面解决这些问题。").arg(a_S.sDeepFailSummary.left(1500));
    }
    const QString sPrompt = QStringLiteral(
        "【专家模式·规划轮】请分析下面的用户请求并制定实施计划。本轮只输出计划文本，"
        "禁止修改任何文件（可用 Read/Grep/Glob 等只读工具探索工程）。\n"
        "计划格式（Markdown，全文 ≤60 行）：\n"
        "## 目标\n（一两句话）\n"
        "## 实施步骤\n1. …（每步写明要动的文件/位置，尽量具体可执行）\n"
        "## 验收标准\n1. …（每条必须客观可验证：可编译、可运行、测试通过、"
        "文件内容/行为符合预期；禁止「优化」「完善」等主观措辞）\n"
        "---\n【用户请求】\n%1%2")
        .arg(a_S.sPendingPrompt.left(2000), sLessons);
    StartAdhocStage(a_S, sPrompt, /*a_bDropLastUser=*/true,
                    k_iDeepAuxTurns, QStringLiteral("Edit,Write"));
}

void CTaskOrchestrator::StartDeepVerify(SAdhocSession& a_S) {
    // 深度链第 3 步：验证轮（fork 一次性分支）。独立验证者只核验不修复（禁 Edit/Write），
    //   按 guide.md 构建说明实际编译/测试 + 逐项核对验收标准，末行输出 VERIFY:PASS/FAIL。
    a_S.eMode = DeepVerify;
    emit sigAdhocEvent(a_S.sId, QStringLiteral("deep_verify"),
        QStringLiteral("🔍 专家模式：验证Agent 按验收标准实际编译/测试核验…"));
    const QString sCriteria = ExtractDeepCriteria(a_S.sDeepPlan);
    const QString sPrompt = QStringLiteral(
        "【专家模式·验证轮】你是独立验证者。执行Agent 刚完成了用户请求的改动，"
        "请客观验收（只验证、严禁修复、严禁修改任何工程文件）：\n"
        "1) 逐项核对【验收标准】：需看代码的用 Read 核对，需看行为的实际运行验证；\n"
        "2) 编译/测试方法优先按 .autodev/guide.md 的构建说明执行；无说明时用工程惯例"
        "命令（如 cmake --build build、msbuild、pnpm test 等）；\n"
        "3) 正文逐项给出证据（执行的命令与关键输出/检查的文件与关键行）；\n"
        "4) 最后一行必须精确输出总结论（二选一）：\n"
        "   VERIFY:PASS\n"
        "   VERIFY:FAIL: <原因清单，逐条对应未通过的验收标准>\n"
        "---\n【验收标准】\n%1\n---\n【用户原始请求】\n%2")
        .arg(sCriteria, a_S.sPendingPrompt.left(800));
    StartAdhocStage(a_S, sPrompt, /*a_bDropLastUser=*/true,
                    k_iDeepAuxTurns, QStringLiteral("Edit,Write"));
}

void CTaskOrchestrator::DeepChainRestart(SAdhocSession& a_S, const QString& a_sWhy,
                                         const QString& a_sReasons) {
    // 重启判定（2026.09.26 用户需求：重试耗尽导致停止时自动重启）：失败教训累积进
    //   sDeepFailSummary（重规划时注入规避，保尾部 2000 字）→ 未超 [Adhoc]
    //   ExpertRestarts 则重置回炉计数、重新「规划→执行→验证」；超限 → 保留断点
    //   收尾交还用户（改动与会话都在，可人工修复后续问）。
    const int iCap = qBound(0, (m_pPm != nullptr ? m_pPm->LoadExpertRestarts() : 1), 5);
    if (!a_sReasons.trimmed().isEmpty()) {
        const QString sAdd = QStringLiteral("[%1] %2")
            .arg(a_sWhy, a_sReasons.left(600));
        a_S.sDeepFailSummary = a_S.sDeepFailSummary.isEmpty()
            ? sAdd : (a_S.sDeepFailSummary + QLatin1Char('\n') + sAdd);
        if (a_S.sDeepFailSummary.size() > 2000) {   // 保尾部：最新教训优先
            a_S.sDeepFailSummary = a_S.sDeepFailSummary.right(2000);
        }
    }
    if (a_S.iDeepRestart < iCap) {
        ++a_S.iDeepRestart;
        a_S.iDeepRetry = 0;
        emit sigAdhocEvent(a_S.sId, QStringLiteral("deep_restart"),
            QStringLiteral("♻ %1——专家模式自动重启深度链（第 %2/%3 次：重新规划→执行→验证）")
                .arg(a_sWhy).arg(a_S.iDeepRestart).arg(iCap));
        StartDeepPlan(a_S);
        return;
    }
    emit sigAdhocEvent(a_S.sId, QStringLiteral("deep_fail"),
        QStringLiteral("✗ 专家模式未能达成验收（%1；已自动重启 %2 次）。"
                       "会话与改动已保留，可人工检查后发送修复指令")
            .arg(a_sWhy).arg(a_S.iDeepRestart));
    FinishAdhocFailure(a_S);
}

void CTaskOrchestrator::FinishAdhocSuccess(SAdhocSession& a_S, const QString& a_sFinalSid) {
    // 原 Submitting 成功尾段提炼（2026.09.26）：压栈 (prompt, newSid) 并推进链基点
    //   （sid 空=异常不压栈、链不动）→ 模型/上下文上报 → guide 同步节流 → 排队指令
    //   续发（多条合并投送）。深度链 PASS 与纯对话成功统一走这里。
    if (!a_sFinalSid.isEmpty()) {
        a_S.aStack.append({ a_S.sPendingPrompt, a_sFinalSid });
        a_S.sSid = a_sFinalSid;
        emit sigAdhocStateChanged(a_S.sId);   // #1 会话链推进 → 存盘
    }
    a_S.sPendingPrompt.clear();
    a_S.eMode = Idle;
    a_S.bDeepActive = false;   // 深度链终结（成功）
    emit sigAdhocModel(a_S.sId, a_S.pRunner ? a_S.pRunner->LastModel() : QString());
    // 收尾拿不到 usage（≤0）时发 -1 显示「—」而非误导性的 0（pi+网关响应帧 usage 恒 0）。
    emit sigAdhocContext(a_S.sId, a_S.pRunner
        ? (a_S.pRunner->LastContextFill() > 0 ? a_S.pRunner->LastContextFill() : -1) : 0);
    // 需求4：非只读会话完成 → 触发 guide.md 增量同步（空闲 3 分钟节流，见构造函数）。
    if (a_S.iPerm != PermReadOnly && m_pPm != nullptr) {
        m_tGuideSyncIdle.start();
    }
    emit sigAdhocEvent(a_S.sId, QStringLiteral("done"), QStringLiteral("已完成"));
    // 2026.08.27 运行中排队的追加指令自动续发（同会话续跑，在 sigAdhocFinished 之前）；
    //   2026.09.20 v2.4.185 多条排队指令一次性合并投送：全部出队，逐条转正面板暂存
    //   块后合并为一次 SubmitAdhoc；单条行为不变（无包装原文投递）。
    if (!a_S.aPendingInjects.isEmpty()) {
        QStringList aBatch;
        for (const QString& s : a_S.aPendingInjects) {
            if (!s.trimmed().isEmpty()) {
                aBatch << s;
            }
        }
        a_S.aPendingInjects.clear();
        for (const QString& s : aBatch) {
            emit sigAdhocPendingStarted(a_S.sId, s);
        }
        if (aBatch.isEmpty()) {
            // 全空白（防御，正常排队路径已拦空文本）：直接归空闲。
            emit sigAdhocFinished(a_S.sId);
        } else if (aBatch.size() == 1) {
            SubmitAdhoc(a_S.sId, aBatch.first(), a_S.iPerm, a_S.sLastTaskId);
        } else {
            emit sigAdhocEvent(a_S.sId, QStringLiteral("batch"),
                QStringLiteral("已将 %1 条排队指令合并为本轮一次性投送")
                    .arg(aBatch.size()));
            SubmitAdhoc(a_S.sId, MergeQueuedPrompts(aBatch),
                        a_S.iPerm, a_S.sLastTaskId);
        }
        return;
    }
    emit sigAdhocFinished(a_S.sId);
}

void CTaskOrchestrator::FinishAdhocFailure(SAdhocSession& a_S) {
    // 深度链最终失败收尾：保留断点（压栈保会话，用户可发修复指令/「继续」接续）+
    //   上下文 -1（本轮未正常完成）+ 交还输入栏。
    if (!a_S.sSid.isEmpty()) {
        a_S.aStack.append({ a_S.sPendingPrompt, a_S.sSid });
        emit sigAdhocStateChanged(a_S.sId);
    }
    a_S.sPendingPrompt.clear();
    a_S.eMode = Idle;
    a_S.bDeepActive = false;   // 深度链终结（失败）
    emit sigAdhocContext(a_S.sId, -1);
    emit sigAdhocFinished(a_S.sId);
}

int CTaskOrchestrator::ParseDeepVerdict(const QString& a_sResult, QString* a_pReasons) {
    // 纯函数：返回 1=PASS / 0=FAIL（a_pReasons 出该行失败原因） / -1=无判定行。
    //   FAIL 优先判定：防「VERIFY:FAIL: xxx passed」原因文本误命中 PASS。
    if (a_pReasons != nullptr) {
        a_pReasons->clear();
    }
    static const QRegularExpression reFail(
        QStringLiteral("VERIFY\\s*:\\s*FAIL\\b[\uff1a:]?\\s*(.*)$"),
        QRegularExpression::CaseInsensitiveOption
        | QRegularExpression::MultilineOption);
    const auto mFail = reFail.match(a_sResult);
    if (mFail.hasMatch()) {
        if (a_pReasons != nullptr) {
            *a_pReasons = mFail.captured(1).trimmed();
        }
        return 0;
    }
    static const QRegularExpression rePass(
        QStringLiteral("VERIFY\\s*:\\s*PASS\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return rePass.match(a_sResult).hasMatch() ? 1 : -1;
}

QString CTaskOrchestrator::ExtractDeepCriteria(const QString& a_sPlan) {
    // 纯函数：从规划产物提取「## 验收标准」段（到下一个 ## 标题或文末）；段缺失/
    //   段体为空 → 回退整份计划（验证轮仍拿到可用输入）；恒截 2000 字。
    if (a_sPlan.trimmed().isEmpty()) {
        return QStringLiteral("（规划产物缺失——请按用户原始请求的合理结果验收）");
    }
    static const QRegularExpression reSection(
        QStringLiteral("(?m)^#{0,3}\\s*验收标准\\s*$"));
    const auto m = reSection.match(a_sPlan);
    if (!m.hasMatch()) {
        return a_sPlan.trimmed().left(2000);
    }
    const QString sRest = a_sPlan.mid(m.capturedEnd());
    static const QRegularExpression reNext(QStringLiteral("(?m)^#{1,3}\\s+\\S"));
    const auto mNext = reNext.match(sRest);
    QString sBody = mNext.hasMatch() ? sRest.left(mNext.capturedStart()) : sRest;
    sBody = sBody.trimmed();
    if (sBody.isEmpty()) {
        return a_sPlan.trimmed().left(2000);
    }
    return sBody.left(2000);
}

bool CTaskOrchestrator::IsAdhocQueryPrompt(const QString& a_sText) {
    // 纯函数启发式（2026.10.02）：判定用户请求是否为查询/咨询类（不改码）。
    //   判据（须同时满足）：① 无改码动词（出现任一即视为改码请求，优先级最高
    //   ——混合句「解释一下然后修复」按改码处理；「配置/更新」等名词化高频词
    //   不入表——它们作动词的句子通常无查询信号，仍会正确判改码）；② 有查询
    //   信号——问号结尾或含查询动词。无任何信号的中性请求默认 false（保守走深度链，宁多验不漏验）。
    //   注意只判用户原文（SubmitAdhoc 的 a_sText），不判注入定界/规则后的合成
    //   提示词（规则段含「修改」等词必误判）。
    const QString sT = a_sText.trimmed();
    if (sT.isEmpty()) {
        return false;
    }
    static const QStringList aChangeVerbs = {
        QStringLiteral("修改"), QStringLiteral("实现"), QStringLiteral("添加"),
        QStringLiteral("增加"), QStringLiteral("新增"), QStringLiteral("创建"),
        QStringLiteral("新建"), QStringLiteral("删除"), QStringLiteral("移除"),
        QStringLiteral("重构"), QStringLiteral("修复"), QStringLiteral("编写"),
        QStringLiteral("开发"), QStringLiteral("优化"),
        QStringLiteral("安装"), QStringLiteral("升级"),
        QStringLiteral("部署"), QStringLiteral("调整"), QStringLiteral("完善"),
        QStringLiteral("搭建"), QStringLiteral("接入"), QStringLiteral("集成"),
        QStringLiteral("迁移"), QStringLiteral("补充"), QStringLiteral("格式化"),
        QStringLiteral("重命名"), QStringLiteral("生成"), QStringLiteral("写一"),
        QStringLiteral("做一"), QStringLiteral("改一"), QStringLiteral("加一"),
    };
    for (const QString& sV : aChangeVerbs) {
        if (sT.contains(sV)) {
            return false;
        }
    }
    if (sT.endsWith(QLatin1Char('?')) || sT.endsWith(QChar(0xFF1F))) {
        return true;
    }
    static const QStringList aQueryVerbs = {
        QStringLiteral("是什么"), QStringLiteral("什么是"), QStringLiteral("为什么"),
        QStringLiteral("怎么"), QStringLiteral("如何"), QStringLiteral("哪些"),
        QStringLiteral("哪个"), QStringLiteral("有没有"), QStringLiteral("是否"),
        QStringLiteral("查询"), QStringLiteral("查看"), QStringLiteral("看看"),
        QStringLiteral("分析"), QStringLiteral("解释"), QStringLiteral("说明"),
        QStringLiteral("列出"), QStringLiteral("显示"), QStringLiteral("了解"),
        QStringLiteral("介绍"), QStringLiteral("搜索"), QStringLiteral("什么意思"),
        QStringLiteral("区别"), QStringLiteral("作用"), QStringLiteral("用法"),
        QStringLiteral("支持吗"), QStringLiteral("可以吗"), QStringLiteral("能否"),
        QStringLiteral("帮我看"), QStringLiteral("看一下"), QStringLiteral("读一下"),
    };
    for (const QString& sV : aQueryVerbs) {
        if (sT.contains(sV)) {
            return true;
        }
    }
    return false;
}

QString CTaskOrchestrator::CompressDeepAuxText(const QString& a_sText) {
    // 深链辅助轮（规划/验证）输出压缩（2026.10.02）：≤360 字原样；超长保首 300 字
    //   + 尾注折叠说明。结论以事件判定行为准（验证轮末行 VERIFY:PASS/FAIL 由
    //   deep_verify_pass/事件行独立呈现），规划详情完整保留在 sDeepPlan 注入
    //   执行轮——屏幕只留足够上下文不丢结论。
    const QString sT = a_sText.trimmed();
    if (sT.size() <= 360) {
        return sT;
    }
    return sT.left(300) + QStringLiteral(
        "\n\n…（过程输出较长，已折叠 %1 字；结论以下方判定行为准）").arg(sT.size());
}

