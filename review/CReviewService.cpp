// CReviewService.cpp —— 代码审查执行服务实现
//
// 线程模型：专属 QThread（默认 exec 事件循环）。Run 经 invokeMethod
// (QueuedConnection) 投到线程里跑；文件循环内每个 LLM 回调用嵌套 QEventLoop
// 同步等待 runner 的 sigFinished（runner 创建于同线程 → 信号直连、事件派发正常）。
// 停止语义：Stop() 经 worker 的线程亲和投递 StopRun → 置位中止 → 终止当前
// runner（各后端 Stop 均静默不发 sigFinished，故回调里补 loop.quit()）→ 循环
// 在下一文件边界退出。worker 全部信号经 signal→signal 转发到 service（跨线程
// 自动排队）；service 先亡则连接自动断开，worker 空发无害。

#include "CReviewService.h"
#include "CReviewSdkClient.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include "agent/CAgentRunnerFactory.h"
#include "agent/CAgentRunnerBase.h"

// ----------------------------------------------------------------------------
// —— 同步 LLM 调用上下文（LCR 回调与 worker 共享；生命周期=单文件单次调用）——
class CReviewWorkerImpl;

struct SSyncLlmCtx {
    CReviewWorkerImpl* pRelay     = nullptr;   // 诊断信号转发（worker 自有 sigInfo）
    QString            sWorkDir;              // runner 工作目录（工程根）
    int                iTimeoutMs = 0;        // 单文件 LLM 超时
    QByteArray         aResp;                 // 成功响应（UTF-8；借用指针返回给 SDK）
    QString            sErr;                  // 失败原因（借用指针返回给 SDK）
    QEventLoop*        pLoop     = nullptr;   // 嵌套循环（超时/停止路径补 quit）
    CAgentRunnerBase*  pRunner   = nullptr;   // 当前 runner（停止路径终止用）
};

// ----------------------------------------------------------------------------
// —— worker 实现（QObject，moveToThread(m_pTh)；全部逻辑在 worker 线程执行）——
class CReviewWorkerImpl : public QObject {
    Q_OBJECT

public:
    explicit CReviewWorkerImpl() = default;

    bool         m_bAbort  = false;            // 停止标志（StopRun 置位；Run 入口复位）
    SSyncLlmCtx* m_pCurCtx = nullptr;          // 当前调用上下文（停止路径终止 LLM 用）

    void Run(const QString& a_sRootDir, const QStringList& a_aAbsFiles, int a_iTimeoutSec) {
        m_bAbort = false;   // 复位上轮残留的停止请求（Stop 在空转期置位无害化）
        CReviewSdkClient* pSdk = CReviewSdkClient::Get();
        QJsonArray aAll;
        QStringList aErrors;
        int iReviewed = 0, iFindings = 0, iSkipped = 0;
        const int n = static_cast<int>(a_aAbsFiles.size());
        for (int i = 0; i < n; ++i) {
            if (m_bAbort) {
                break;
            }
            const QString sAbs = a_aAbsFiles.at(i);
            const QString sRel = CReviewSdkClient::RelativeToRoot(a_sRootDir, sAbs);
            emit sigFileStart(sRel, i + 1, n);

            QString sFindings, sErr;
            SSyncLlmCtx stCtx;
            stCtx.pRelay     = this;
            stCtx.sWorkDir   = a_sRootDir;
            stCtx.iTimeoutMs = a_iTimeoutSec * 1000;
            m_pCurCtx = &stCtx;
            const int iRc = pSdk->ReviewFile(sAbs, sRel, &ReviewLlmTrampoline, &stCtx,
                                             sFindings, sErr);
            m_pCurCtx = nullptr;

            if (iRc == AUTOCODE_LCR_OK) {
                ++iReviewed;
                // file 字段统一改写为工程相对路径（ReviewFile 传的是绝对路径）。
                QJsonArray aFileFindings =
                    QJsonDocument::fromJson(sFindings.toUtf8()).array();
                for (int k = 0; k < aFileFindings.size(); ++k) {
                    QJsonObject oF = aFileFindings.at(k).toObject();
                    if (oF.value(QStringLiteral("file")).toString().isEmpty()) {
                        oF.insert(QStringLiteral("file"), sRel);
                    }
                    aFileFindings.replace(k, oF);
                    aAll.append(oF);
                }
                iFindings += aFileFindings.size();
                emit sigFileDone(sRel, iRc,
                                 QString::fromUtf8(QJsonDocument(aFileFindings)
                                                       .toJson(QJsonDocument::Compact)),
                                 QString());
            } else if (iRc == AUTOCODE_LCR_ERR_NO_RULES) {
                ++iSkipped;   // 扩展名不在 10 语言内（.json/.md/.txt 等）——正常跳过
                emit sigFileDone(sRel, iRc, QString(), QString());
            } else {
                if (m_bAbort) {
                    // 停止截断：SDK 侧表现为 LLM 失败，统一改报「已停止」。
                    emit sigFileDone(sRel, -1, QString(), QStringLiteral("已停止"));
                    break;
                }
                aErrors.append(QStringLiteral("- %1：%2").arg(sRel, sErr));
                emit sigFileDone(sRel, iRc, QString(), sErr);
            }
        }

        // —— Markdown 报告（落盘留档 + 人工阅读 + 阶段预审注入）——
        QString sMd = QStringLiteral(
            "# 代码审查报告（ReviewSdk）\n\n"
            "- 审查时间：%1\n- 工程根：%2\n- 送审文件：%3 个\n"
            "- 命中语言送审：%4 个；跳过（不支持语言）：%5 个\n- 发现问题：%6 条\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 QDir::toNativeSeparators(a_sRootDir))
            .arg(n)
            .arg(iReviewed)
            .arg(iSkipped)
            .arg(iFindings);
        if (iFindings > 0) {
            sMd += QStringLiteral("\n## 问题清单\n\n");
            for (const auto& v : aAll) {
                const QJsonObject oF = v.toObject();
                sMd += QStringLiteral("- **[%1]** `%2:%3`（%4）%5\n")
                           .arg(oF.value(QStringLiteral("severity")).toString(),
                                oF.value(QStringLiteral("file")).toString(),
                                QString::number(oF.value(QStringLiteral("line")).toInt()),
                                oF.value(QStringLiteral("rule")).toString(),
                                oF.value(QStringLiteral("message")).toString());
                const QString sSug = oF.value(QStringLiteral("suggestion")).toString();
                if (!sSug.isEmpty()) {
                    sMd += QStringLiteral("  - 建议：%1\n").arg(sSug);
                }
            }
        }
        if (!aErrors.isEmpty()) {
            sMd += QStringLiteral("\n## 失败文件\n\n%1\n").arg(aErrors.join(QLatin1Char('\n')));
        }
        if (m_bAbort) {
            sMd += QStringLiteral("\n> 注：本轮审查被提前停止，以上为已完成部分。\n");
        }

        emit sigReviewDone(!m_bAbort, m_bAbort,
                           QString::fromUtf8(QJsonDocument(aAll)
                                                 .toJson(QJsonDocument::Compact)),
                           sMd, iReviewed, iFindings);
    }

public slots:
    void StopRun() {
        m_bAbort = true;
        if (m_pCurCtx != nullptr) {
            if (m_pCurCtx->pRunner != nullptr
                && (m_pCurCtx->pRunner->IsRunning() || m_pCurCtx->pRunner->IsGraceActive()
                    || m_pCurCtx->pRunner->IsRetryPending())) {
                m_pCurCtx->pRunner->Stop();
            }
            m_pCurCtx->sErr = QStringLiteral("已停止");
            if (m_pCurCtx->pLoop != nullptr) {
                m_pCurCtx->pLoop->quit();
            }
        }
    }

signals:
    void sigFileStart(const QString& sRelPath, int iCur, int iTotal);
    void sigFileDone(const QString& sRelPath, int iRc, const QString& sFindingsJson,
                     const QString& sErr);
    void sigInfo(const QString& sLine);
    void sigReviewDone(bool bOk, bool bStopped, const QString& sFindingsJson,
                       const QString& sReportMd, int iFilesReviewed, int iFindings);

private:
    // LCR_LlmInvokeFn 回调（调用线程=worker 线程，SDK 内同步执行恰好一次）。
    static const char* ReviewLlmTrampoline(void* a_pUserCtx, const char* a_szPromptUtf8,
                                           const char** a_ppszErrorUtf8);
};

// LCR 回调实现（file-static，访问 worker 的信号）。
const char* CReviewWorkerImpl::ReviewLlmTrampoline(void* a_pUserCtx,
                                                   const char* a_szPromptUtf8,
                                                   const char** a_ppszErrorUtf8) {
    SSyncLlmCtx* pCtx = static_cast<SSyncLlmCtx*>(a_pUserCtx);
    if (a_szPromptUtf8 == nullptr) {
        if (a_ppszErrorUtf8 != nullptr) {
            *a_ppszErrorUtf8 = "审查 prompt 为空";
        }
        return nullptr;
    }

    QEventLoop oLoop;
    pCtx->pLoop   = &oLoop;
    pCtx->aResp.clear();
    pCtx->sErr.clear();

    // 当前全局后端一次性只读调用：审查 prompt 自含规则+代码全文，模型直接作答；
    // 禁 Bash/Edit/Write 防副作用；轮次上限 6（纯问答，正常 1 轮收敛）。
    CAgentRunnerBase* pRunner = CAgentRunnerFactory::Create();
    pCtx->pRunner = pRunner;
    bool bOk = false;
    QString sResult;
    QObject::connect(pRunner, &CAgentRunnerBase::sigFinished, &oLoop,
                     [&](bool a_bOk, const QString& a_sResult, qint64, qint64, double,
                         const QString&) {
                         bOk     = a_bOk;
                         sResult = a_sResult;
                         oLoop.quit();
                     });
    QObject::connect(pRunner, &CAgentRunnerBase::sigInfo, &oLoop,
                     [pCtx](const QString& a_sLine) {
                         if (pCtx->pRelay != nullptr) {
                             emit pCtx->pRelay->sigInfo(a_sLine);
                         }
                     });
    // 超时：先 Stop（静默终止，不发 sigFinished）再补 quit——否则嵌套循环永远等不到。
    QTimer oTimer;
    oTimer.setSingleShot(true);
    QObject::connect(&oTimer, &QTimer::timeout, &oLoop, [pRunner, pCtx]() {
        if (pCtx->sErr.isEmpty()) {
            pCtx->sErr = QStringLiteral("审查模型调用超时");
        }
        if (pRunner->IsRunning() || pRunner->IsGraceActive() || pRunner->IsRetryPending()) {
            pRunner->Stop();
        }
        if (pCtx->pLoop != nullptr) {
            pCtx->pLoop->quit();
        }
    });

    pRunner->StartStage(pCtx->sWorkDir, QString::fromUtf8(a_szPromptUtf8),
                        QStringLiteral("Bash,Edit,Write"), 6);
    oTimer.start(pCtx->iTimeoutMs);
    oLoop.exec();
    oTimer.stop();
    pCtx->pRunner = nullptr;
    pCtx->pLoop   = nullptr;
    pRunner->deleteLater();
    QCoreApplication::sendPostedEvents(pRunner, QEvent::DeferredDelete);

    if (!bOk) {
        if (pCtx->sErr.isEmpty()) {
            pCtx->sErr = sResult.trimmed().isEmpty()
                             ? QStringLiteral("审查模型调用失败（无错误文本）")
                             : sResult.trimmed().left(600);
        }
        if (a_ppszErrorUtf8 != nullptr) {
            *a_ppszErrorUtf8 = pCtx->sErr.toUtf8().constData();
        }
        return nullptr;
    }
    if (sResult.trimmed().isEmpty()) {
        pCtx->sErr = QStringLiteral("审查模型返回空响应");
        if (a_ppszErrorUtf8 != nullptr) {
            *a_ppszErrorUtf8 = pCtx->sErr.toUtf8().constData();
        }
        return nullptr;
    }
    pCtx->aResp = sResult.toUtf8();
    return pCtx->aResp.constData();   // 借用指针：保活到本次 LCR_Review* 返回即足够
}

#include "CReviewService.moc"

// ----------------------------------------------------------------------------
// —— CReviewService ——
CReviewService::CReviewService(QObject* a_pParent) : QObject(a_pParent) {
    m_pTh = new QThread(this);
    m_pTh->start();
    // 常驻 worker（单实例跨轮复用；m_bAbort 在每轮 Run 入口复位，防上轮 Stop
    // 残留毒化下一轮。销毁只发生在 service 析构（线程已停），无悬垂窗口）。
    CReviewWorkerImpl* pWorker = new CReviewWorkerImpl();
    pWorker->moveToThread(m_pTh);
    m_pWorkerObj = pWorker;
    // worker 自有信号 → service 信号转发（跨线程自动排队；service 先亡自动断开）。
    connect(pWorker, &CReviewWorkerImpl::sigFileStart, this, &CReviewService::sigFileStart);
    connect(pWorker, &CReviewWorkerImpl::sigFileDone, this, &CReviewService::sigFileDone);
    connect(pWorker, &CReviewWorkerImpl::sigInfo, this, &CReviewService::sigInfo);
    connect(pWorker, &CReviewWorkerImpl::sigReviewDone, this, &CReviewService::sigReviewDone);
    connect(pWorker, &CReviewWorkerImpl::sigReviewDone, this, [this]() {
        m_bRunning.store(false);
    });
}

CReviewService::~CReviewService() {
    Stop();
    m_pTh->quit();
    m_pTh->wait(10000);
    delete m_pWorkerObj;   // 线程已停后从创建线程销毁（无事件循环竞争）
    m_pWorkerObj = nullptr;
}

bool CReviewService::Start(const QString& a_sRootDir, const QStringList& a_aAbsFiles,
                           int a_iTimeoutSec) {
    if (m_bRunning.load() || a_sRootDir.isEmpty() || a_aAbsFiles.isEmpty()) {
        return false;
    }
    QString sErr;
    if (!CReviewSdkClient::Get()->EnsureLoaded(&sErr)) {
        emit sigInfo(QStringLiteral("审查引擎不可用：%1").arg(sErr));
        return false;
    }
    m_bRunning.store(true);
    CReviewWorkerImpl* pWorker = static_cast<CReviewWorkerImpl*>(m_pWorkerObj);
    const QString sRoot = a_sRootDir;
    const QStringList aFiles = a_aAbsFiles;
    const int iTimeout = (a_iTimeoutSec > 0) ? a_iTimeoutSec : 600;
    QMetaObject::invokeMethod(pWorker, [pWorker, sRoot, aFiles, iTimeout]() {
        pWorker->Run(sRoot, aFiles, iTimeout);
    }, Qt::QueuedConnection);
    return true;
}

void CReviewService::Stop() {
    if (m_pWorkerObj != nullptr) {
        // 经 worker 线程亲和投递（其操作的 runner/loop 均属 worker 线程）。
        QMetaObject::invokeMethod(m_pWorkerObj, [this]() {
            if (m_pWorkerObj != nullptr) {
                static_cast<CReviewWorkerImpl*>(m_pWorkerObj)->StopRun();
            }
        }, Qt::QueuedConnection);
    }
}
