#pragma once

// ============================================================================
// CReviewService —— 代码审查执行服务（2026-09-30 ReviewSdk 接入宿主侧编排）
// ----------------------------------------------------------------------------
// 职责（宿主策略层，库刻意不做的部分）：
//   1. 逐文件调用 ReviewSdk.dll 的 LCR_ReviewFile（编码/加密回退由库内读取链处理）；
//   2. 大模型回调（LCR_LlmInvokeFn，同步借用指针）——经 CAgentRunnerFactory 用
//      当前全局后端起一次性只读 runner（禁 Bash/Edit/Write），QEventLoop 同步等
//      sigFinished 取终答，天然兼容全部 5 个后端；
//   3. 全流程跑在专属 QThread（LLM 调用分钟级，绝不阻塞 UI/编排器主线程）；
//   4. 结果聚合：findings JSON 数组（file 字段统一改写为工程相对路径）+ Markdown
//      报告（供落盘与注入 CodeReview 阶段 prompt）。
// 消费方：CTaskOrchestrator（CodeReview 阶段静态预审）与 CCodeReviewDialog
// （资源管理器右键手动审查）各持一个实例（并发各跑各的，互不排队）。
// 线程安全：本对象常驻主线程；信号从 worker 线程自动排队送达。析构先 Stop 再
// 收线程；worker 自有信号经 signal→signal 转发，service 先亡则连接自动断开，
// worker 空发无害（QObject 析构断连接的 Qt 语义保证无悬垂）。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <atomic>

class QThread;

// —— 2026-10-08 手动审查 Tab 改造：单文件送审项 ——
// bIncremental=true 且基线仓可用 → 只审自上次审查后的修改段（库内
// BuildIncrementalScope + ReviewContentEx 增量契约）；否则全量审。
struct SReviewItem {
    QString sAbs;
    bool    bIncremental = false;
};

class CReviewService : public QObject {
    Q_OBJECT

public:
    explicit CReviewService(QObject* a_pParent = nullptr);
    ~CReviewService() override;

    // 启动一轮审查：a_sRootDir 为工程根（相对路径换算基），a_aAbsFiles 为待审
    // 文件绝对路径清单（调用方已过滤/限量）。a_iTimeoutSec 为单文件 LLM 调用
    // 超时。返回 false=正在跑（本实例一轮未完不接新单）或参数空/引擎不可用。
    // （任务阶段预审路径：全量审、不建基线——保持 2026-09-30 原行为。）
    bool Start(const QString& a_sRootDir, const QStringList& a_aAbsFiles, int a_iTimeoutSec);

    // 2026-10-08 手动审查路径：逐文件可增量 + 基线仓（a_sBaselineDir 为
    // review_cache/<目标哈希>.bl；空串=不启用基线）。审查成功的文件基线前移
    // （内容入 blob 仓 + meta 更新），下次「仅审修改」由此判定。
    // 2026-10-08 并发审查：a_iAgents=并发审查 Agent 数（每文件一个一次性 runner，
    // N 个 lane 线程各持队列取件；1=串行原路径）。上限 min(agents, 文件数)。
    bool StartEx(const QString& a_sRootDir, const QList<SReviewItem>& a_aFiles,
                 const QString& a_sBaselineDir, int a_iTimeoutSec, int a_iAgents = 1);

    // 停止当前轮（当前文件的 LLM 调用一并终止；已完成的文件结果保留并照常收尾）。
    void Stop();

    bool IsRunning() const { return m_bRunning.load(); }

signals:
    // 单文件开始/结束（iRc=AUTOCODE_LCR_*；rc=2 表示语言不支持已跳过，
    // sFindingsJson 仅 rc=0 有效）。
    void sigFileStart(const QString& sRelPath, int iCur, int iTotal);
    void sigFileDone(const QString& sRelPath, int iRc, const QString& sFindingsJson,
                     const QString& sErr);
    // 诊断行（runner 原始信息透传，如后端启动失败原因）。
    void sigInfo(const QString& sLine);
    // 整轮结束（bStopped=true 表示被 Stop 截断）。sFindingsJson=合并数组，
    // sReportMd=Markdown 报告，iFilesReviewed=实际送审模型数，iFindings=发现总数。
    void sigReviewDone(bool bOk, bool bStopped, const QString& sFindingsJson,
                       const QString& sReportMd, int iFilesReviewed, int iFindings);

private:
    // worker 实体定义在 .cpp（QObject 子类，单实例常驻 moveToThread）；主线程仅持
    // QObject* 做亲和投递（Stop 经它转到 worker 线程执行）。void* 免头文件泄漏实现类。
    QThread*          m_pTh        = nullptr;
    QObject*          m_pWorkerObj = nullptr;   // 常驻 worker（析构时销毁）
    std::atomic_bool  m_bRunning   = {false};
};
