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
#include "CReviewBaseline.h"

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
#include <QFileInfo>

#include "agent/CAgentRunnerFactory.h"
#include "agent/CAgentRunnerBase.h"
#include "core/CFrwDevice.h"   // 2026-10-08 增量路径自读原文（统一 fileread.dll 入口）

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // GB18030 严格校验（MultiByteToWideChar 54936）
#endif
#include <QStringDecoder>

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

    // 2026-10-08 编码自适应解码（与 CFilePreviewWidget::DetectAndDecode 同口径的
    // 审查侧副本）：BOM(UTF-16LE/BE、UTF-8) → 严格 UTF-8 → GB18030(Windows 严格
    // 校验) → 二进制判定（返回 false，该文件报失败）。增量/全量自读路径共用。
    static bool DecodeText(const QByteArray& a_byBytes, QString& a_sText) {
        if (a_byBytes.size() >= 2) {
            if (uchar(a_byBytes[0]) == 0xFF && uchar(a_byBytes[1]) == 0xFE) {
                QStringDecoder d(QStringLiteral("UTF-16LE"));
                a_sText = d.decode(QByteArrayView(a_byBytes).sliced(2));
                return true;
            }
            if (uchar(a_byBytes[0]) == 0xFE && uchar(a_byBytes[1]) == 0xFF) {
                QStringDecoder d(QStringLiteral("UTF-16BE"));
                a_sText = d.decode(QByteArrayView(a_byBytes).sliced(2));
                return true;
            }
        }
        if (a_byBytes.size() >= 3 && uchar(a_byBytes[0]) == 0xEF
            && uchar(a_byBytes[1]) == 0xBB && uchar(a_byBytes[2]) == 0xBF) {
            QStringDecoder d(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
            a_sText = d.decode(QByteArrayView(a_byBytes).sliced(3));
            return true;
        }
        {
            QStringDecoder d(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
            QString s = d.decode(a_byBytes);
            if (!d.hasError()) {
                a_sText = std::move(s);
                return true;
            }
        }
#ifdef Q_OS_WIN
        if (!a_byBytes.contains('\0')) {
            const int nLen = int(a_byBytes.size());
            const int nW = MultiByteToWideChar(54936 /*GB18030*/, MB_ERR_INVALID_CHARS,
                                               a_byBytes.constData(), nLen, nullptr, 0);
            if (nW > 0 || (nW == 0 && nLen == 0)) {
                std::wstring sBuf(nW, L'\0');
                if (nW > 0) {
                    MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS,
                                        a_byBytes.constData(), nLen, sBuf.data(), nW);
                }
                a_sText = QString::fromStdWString(sBuf);
                return true;
            }
        }
#endif
        // 未知格式：空字节密集判二进制（审查无意义）
        int iZero = 0;
        for (char c : a_byBytes) {
            if (c == '\0') {
                ++iZero;
            }
        }
        if (!a_byBytes.isEmpty() && iZero * 10 > a_byBytes.size()) {
            return false;
        }
        a_sText = QString::fromLatin1(a_byBytes);
        return true;
    }

    // 2026-10-08 统一入口：逐文件可选增量 + 基线前移（bUseBaseline=false 时
    // 走 2026-09-30 原路径：SDK ReviewFile 库内读取链，任务阶段预审不变）。
    void Run(const QString& a_sRootDir, const QList<SReviewItem>& a_aFiles,
             const QString& a_sBaselineDir, int a_iTimeoutSec) {
        m_bAbort = false;   // 复位上轮残留的停止请求（Stop 在空转期置位无害化）
        CReviewSdkClient* pSdk = CReviewSdkClient::Get();
        const bool bUseBaseline = !a_sBaselineDir.isEmpty();
        CReviewBaseline::TMap mapBase = bUseBaseline
            ? CReviewBaseline::LoadMeta(a_sBaselineDir)
            : CReviewBaseline::TMap();
        QJsonArray aAll;
        QStringList aErrors;
        int iReviewed = 0, iFindings = 0, iSkipped = 0, iNoChange = 0, iIncr = 0;
        const int n = static_cast<int>(a_aFiles.size());
        for (int i = 0; i < n; ++i) {
            if (m_bAbort) {
                break;
            }
            const SReviewItem stItem = a_aFiles.at(i);
            const QString sAbs = stItem.sAbs;
            const QString sRel = CReviewSdkClient::RelativeToRoot(a_sRootDir, sAbs);
            emit sigFileStart(sRel, i + 1, n);

            int iRc = AUTOCODE_LCR_ERR_ARGS;
            QString sFindings, sErr;
            QByteArray byRaw;
            bool bCallSdk = false;   // false=读盘/解码已失败，跳过 SDK 直落错误分支

            // LLM 调用上下文（停止路径终止 runner 用；两路共用）。
            SSyncLlmCtx stCtx;
            stCtx.pRelay     = this;
            stCtx.sWorkDir   = a_sRootDir;
            stCtx.iTimeoutMs = a_iTimeoutSec * 1000;
            m_pCurCtx = &stCtx;

            if (!bUseBaseline) {
                // —— 原路径：库内读取链（编码/透明加密回退都在 SDK 侧）——
                bCallSdk = true;
            } else {
                // —— 基线路径：自读原文（基线需要原始字节）→ 增量裁决 → ContentEx ——
                bool bReadOk = false;
                {
                    CFrwDevice stFile(sAbs);
                    if (stFile.open(QIODevice::ReadOnly)) {
                        byRaw = stFile.readAll();
                        stFile.close();
                        bReadOk = true;
                    }
                }
                QString sText;
                if (!bReadOk) {
                    iRc = AUTOCODE_LCR_ERR_READ;
                    sErr = QStringLiteral("文件读取失败");
                } else if (!DecodeText(byRaw, sText)) {
                    iRc = AUTOCODE_LCR_ERR_READ;
                    sErr = QStringLiteral("二进制/未知编码文件，无法审查");
                } else {
                    bCallSdk = true;
                    // 基线快照取读取时刻盘上 mtime/size（审查成功即入基线；期间
                    // 外部再改 → 下次「修改过」检测按 mtime/size 判回，不漏网）。
                    const QFileInfo stInfo(sAbs);
                    const qint64 iMt = stInfo.lastModified().toMSecsSinceEpoch();
                    const qint64 iSz = stInfo.size();

                    QString sScope = sText;
                    bool bInc = false;
                    if (stItem.bIncremental) {
                        const auto itBase = mapBase.find(CReviewBaseline::NormKey(sRel));
                        QByteArray byOld;
                        QString sOld;
                        if (itBase != mapBase.end()
                            && CReviewBaseline::ReadBlob(a_sBaselineDir,
                                                         itBase.value().sSha1, byOld)
                            && DecodeText(byOld, sOld)) {
                            const QString sSc = pSdk->BuildIncrementalScope(sOld, sText);
                            if (sSc.isEmpty()) {
                                // 与上次审查版本零差异：零模型调用收尾，基线照常前移
                                // （防「内容未变但被 touch」误判修改）。
                                const QString sSha = CReviewBaseline::Sha1Of(byRaw);
                                CReviewBaseline::WriteBlob(a_sBaselineDir, sSha, byRaw);
                                itBase.value() = {iMt, iSz, sSha};
                                CReviewBaseline::SaveMeta(a_sBaselineDir, mapBase);
                                ++iNoChange;
                                m_pCurCtx = nullptr;
                                emit sigInfo(QStringLiteral("无修改，跳过模型调用：%1").arg(sRel));
                                emit sigFileDone(sRel, AUTOCODE_LCR_OK,
                                                 QStringLiteral("[]"), QString());
                                continue;
                            }
                            sScope = sSc;
                            bInc = true;
                            ++iIncr;
                            emit sigInfo(QStringLiteral("增量审查（仅修改部分）：%1").arg(sRel));
                        } else {
                            emit sigInfo(QStringLiteral("无上次审查基线，本次全量审查：%1"
                                                        "（首次审查或上次未成功）").arg(sRel));
                        }
                    }
                    iRc = pSdk->ReviewContentEx(pSdk->LanguageForFile(sAbs), sRel,
                                                sScope, bInc,
                                                &ReviewLlmTrampoline, &stCtx,
                                                sFindings, sErr);
                    if (iRc == AUTOCODE_LCR_OK) {
                        // 审查成功 → 基线前移（内容寻址 blob + meta 原子替换）。
                        const QString sSha = CReviewBaseline::Sha1Of(byRaw);
                        CReviewBaseline::WriteBlob(a_sBaselineDir, sSha, byRaw);
                        mapBase.insert(CReviewBaseline::NormKey(sRel), {iMt, iSz, sSha});
                        CReviewBaseline::SaveMeta(a_sBaselineDir, mapBase);
                    }
                    bCallSdk = false;   // 已调过，下方不再重调
                }
            }
            if (bCallSdk) {
                iRc = pSdk->ReviewFile(sAbs, sRel, &ReviewLlmTrampoline, &stCtx,
                                       sFindings, sErr);
            }
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
        if (bUseBaseline) {
            sMd += QStringLiteral("- 增量审查（仅修改部分）：%1 个；无修改跳过：%2 个\n")
                       .arg(QString::number(iIncr), QString::number(iNoChange));
        }
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
    // 全量、无基线（任务阶段预审路径保持 2026-09-30 原行为）。
    QList<SReviewItem> aItems;
    aItems.reserve(a_aAbsFiles.size());
    for (const QString& s : a_aAbsFiles) {
        aItems.append({s, false});
    }
    return StartEx(a_sRootDir, aItems, QString(), a_iTimeoutSec);
}

bool CReviewService::StartEx(const QString& a_sRootDir, const QList<SReviewItem>& a_aFiles,
                             const QString& a_sBaselineDir, int a_iTimeoutSec) {
    if (m_bRunning.load() || a_sRootDir.isEmpty() || a_aFiles.isEmpty()) {
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
    const QList<SReviewItem> aFiles = a_aFiles;
    const QString sBaseline = a_sBaselineDir;
    const int iTimeout = (a_iTimeoutSec > 0) ? a_iTimeoutSec : 600;
    QMetaObject::invokeMethod(pWorker, [pWorker, sRoot, aFiles, sBaseline, iTimeout]() {
        pWorker->Run(sRoot, aFiles, sBaseline, iTimeout);
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
