// CCodeReviewDialog.cpp —— 手动代码审查对话框实现

#include "CCodeReviewDialog.h"
#include "review/CReviewSdkClient.h"
#include "review/CReviewService.h"
#include "agent/CAgentRunnerFactory.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDateTime>
#include <QCryptographicHash>
#include <QCoreApplication>   // 2026-10-02 结果缓存目录（exe 同级 review_cache/）
#include <QPainter>
#include <QTimer>

// 2026-10-02 审查中动态图标：12 段渐变弧转圈（与 CExecutionPanel 的 CBusyIndicator
// 同风格同参数，文件局部类免 moc）；空闲显示静态灰点。无 Q_OBJECT（基类 connect）。
// 注意：定义在全局命名空间（头文件前向声明同域；不进匿名命名空间防链接名不匹配）。
class CReviewSpinner : public QWidget {
public:
    explicit CReviewSpinner(QWidget* a_pParent = nullptr)
        : QWidget(a_pParent), m_iAngle(0) {
        m_pTimer = new QTimer(this);
        m_pTimer->setInterval(70);
        connect(m_pTimer, &QTimer::timeout, this, [this]() {
            m_iAngle = (m_iAngle + 30) % 360;
            update();
        });
        setFixedSize(18, 18);
    }
    void SetBusy(bool a_bBusy) {
        if (a_bBusy) {
            m_pTimer->start();
        } else {
            m_pTimer->stop();
        }
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal dSide = qMin(width(), height());
        const QPointF ptCenter(width() / 2.0, height() / 2.0);
        if (m_pTimer->isActive()) {
            p.translate(ptCenter);
            p.rotate(m_iAngle);
            const QRectF rRect(-dSide / 2.0 + 2, -dSide / 2.0 + 2, dSide - 4, dSide - 4);
            const int iSeg = 12;
            for (int i = 0; i < iSeg; ++i) {
                QColor c(60, 130, 220);
                c.setAlphaF(static_cast<qreal>(i + 1) / iSeg);
                p.setPen(QPen(c, dSide / 7.0, Qt::SolidLine, Qt::RoundCap));
                p.drawArc(rRect, i * (360 / iSeg) * 16, (360 / iSeg - 6) * 16);
            }
        } else {
            p.setBrush(QBrush(QColor(180, 180, 180)));
            p.setPen(Qt::NoPen);
            p.drawEllipse(ptCenter, dSide / 4.0, dSide / 4.0);
        }
    }
private:
    int     m_iAngle = 0;
    QTimer* m_pTimer = nullptr;
};

namespace {
// 目录递归收集上限（防误选巨型目录整轮跑数小时；超限截断并在状态里说明）。
const int s_kiFolderCollectCap = 200;
// 结果缓存目录（exe 同级 review_cache/，与 config.ini 同域；键=目标路径 SHA256）。
QString CacheDirPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/review_cache");
}
QString CacheFilePath(const QString& a_sTarget) {
    const QString sKey = QString::fromLatin1(QCryptographicHash::hash(
        QDir::cleanPath(a_sTarget).normalized(QString::NormalizationForm_C)
            .toLower().toUtf8(),
        QCryptographicHash::Sha256).toHex()).left(24);
    return CacheDirPath() + QStringLiteral("/") + sKey + QStringLiteral(".json");
}
}

CCodeReviewDialog::CCodeReviewDialog(const QString& a_sTarget, QWidget* a_pParent)
    : QDialog(a_pParent), m_sTarget(QDir::cleanPath(a_sTarget)) {
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    BuildUi();
    CollectFiles();
    // 2026-10-02 结果复用：目标代码未修改（送审清单的 mtime+size 指纹一致）→ 直接
    //   展示上次结果不重跑（分钟级 LLM 调用零开销）；已修改/无缓存 → 照旧打开即审。
    if (!TryRestoreCache()) {
        OnStart();
    }
}

CCodeReviewDialog::~CCodeReviewDialog() {
    if (m_pSvc != nullptr) {
        m_pSvc->Stop();
        m_pSvc->deleteLater();
    }
}

void CCodeReviewDialog::BuildUi() {
    setWindowTitle(QStringLiteral("代码审查"));
    setMinimumSize(860, 520);
    setSizeGripEnabled(true);

    auto* pLay = new QVBoxLayout(this);

    m_pLblTarget = new QLabel(this);
    m_pLblTarget->setWordWrap(true);
    QFont fT = m_pLblTarget->font();
    fT.setPointSize(11);
    m_pLblTarget->setFont(fT);
    pLay->addWidget(m_pLblTarget);

    m_pProgress = new QProgressBar(this);
    m_pProgress->setTextVisible(false);
    m_pProgress->setRange(0, 1);
    m_pProgress->setValue(0);
    pLay->addWidget(m_pProgress);

    // 2026-10-02 状态行首动态图标：审查中转圈（12 段弧 70ms/帧），空闲静态灰点。
    auto* pLayStatus = new QHBoxLayout();
    pLayStatus->setContentsMargins(0, 0, 0, 0);
    pLayStatus->setSpacing(6);
    m_pSpin = new CReviewSpinner(this);
    m_pSpin->setObjectName(QStringLiteral("ReviewSpinner"));   // 无 Q_OBJECT，className 不可靠（同 CPromptFlyout 先例）
    m_pSpin->setToolTip(QStringLiteral("审查进行中"));
    pLayStatus->addWidget(m_pSpin);
    pLayStatus->addWidget(m_pLblStatus = new QLabel(QStringLiteral("就绪"), this), 1);
    QFont fS = m_pLblStatus->font();
    fS.setPointSize(11);
    m_pLblStatus->setFont(fS);
    pLay->addLayout(pLayStatus);

    m_pTree = new QTreeWidget(this);
    QFont fTree = m_pTree->font();
    fTree.setPointSize(11);
    m_pTree->setFont(fTree);
    m_pTree->setColumnCount(6);
    m_pTree->setHeaderLabels({QStringLiteral("级别"), QStringLiteral("文件"),
                              QStringLiteral("行"), QStringLiteral("规则"),
                              QStringLiteral("问题"), QStringLiteral("建议")});
    m_pTree->setRootIsDecorated(false);
    m_pTree->setAlternatingRowColors(true);
    m_pTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_pTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_pTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_pTree->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_pTree->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_pTree->header()->resizeSection(1, 260);
    connect(m_pTree, &QTreeWidget::itemDoubleClicked,
            this, &CCodeReviewDialog::OnItemDoubleClicked);
    pLay->addWidget(m_pTree, 1);

    auto* pLayBtn = new QHBoxLayout();
    m_pBtnStart = new QPushButton(QStringLiteral("重新审查"), this);
    m_pBtnStop  = new QPushButton(QStringLiteral("停止"), this);
    // 2026-10-02「按建议修复」：把审查意见交当前助手修复（存在就修复，不存在忽略）。
    m_pBtnFix   = new QPushButton(QStringLiteral("按建议修复"), this);
    m_pBtnSave  = new QPushButton(QStringLiteral("保存报告..."), this);
    m_pBtnClose = new QPushButton(QStringLiteral("关闭"), this);
    for (QPushButton* pB : {m_pBtnStart, m_pBtnStop, m_pBtnFix, m_pBtnSave, m_pBtnClose}) {
        QFont fB = pB->font();
        fB.setPointSize(11);
        pB->setFont(fB);
    }
    m_pBtnSave->setEnabled(false);
    m_pBtnFix->setEnabled(false);
    m_pBtnFix->setToolTip(QStringLiteral("把审查意见注入当前助手的提问框，交大模型修复"
                                         "（存在就修复，不存在忽略），确认后点发送"));
    pLayBtn->addWidget(m_pBtnStart);
    pLayBtn->addWidget(m_pBtnStop);
    pLayBtn->addStretch(1);
    pLayBtn->addWidget(m_pBtnFix);
    pLayBtn->addWidget(m_pBtnSave);
    pLayBtn->addWidget(m_pBtnClose);
    pLay->addLayout(pLayBtn);

    connect(m_pBtnStart, &QPushButton::clicked, this, &CCodeReviewDialog::OnStart);
    connect(m_pBtnStop,  &QPushButton::clicked, this, &CCodeReviewDialog::OnStop);
    connect(m_pBtnFix,   &QPushButton::clicked, this, &CCodeReviewDialog::OnFixToChat);
    connect(m_pBtnSave,  &QPushButton::clicked, this, &CCodeReviewDialog::OnSaveReport);
    connect(m_pBtnClose, &QPushButton::clicked, this, &QWidget::close);

    // 引擎版本角标（未加载显示原因——OnStart 会给出明确提示）。
    QString sErr;
    if (CReviewSdkClient::Get()->EnsureLoaded(&sErr)) {
        m_pLblTarget->setText(QStringLiteral("目标：%1\n引擎：ReviewSdk %2（后端：%3）")
            .arg(QDir::toNativeSeparators(m_sTarget),
                 CReviewSdkClient::Get()->Version(),
                 CAgentRunnerFactory::BackendDisplayName()));
    } else {
        m_pLblTarget->setText(QStringLiteral("目标：%1\n引擎不可用：%2")
            .arg(QDir::toNativeSeparators(m_sTarget), sErr));
    }
}

void CCodeReviewDialog::CollectFiles() {
    m_aAbsFiles.clear();
    CReviewSdkClient* pSdk = CReviewSdkClient::Get();
    const QFileInfo stInfo(m_sTarget);
    if (stInfo.isFile()) {
        m_sRootDir = stInfo.absolutePath();
        // 单文件不挑语言（引擎对不支持的会回报 rc=2 跳过，提示更直观）。
        m_aAbsFiles.append(m_sTarget);
    } else if (stInfo.isDir()) {
        m_sRootDir = m_sTarget;
        // 目录递归：跳过隐藏/.git/.vs/node_modules/build 等噪声目录 + 语言过滤 + 上限。
        QDirIterator it(m_sTarget, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
        static const QStringList s_aSkipDirs = {
            QStringLiteral(".git"), QStringLiteral(".vs"), QStringLiteral(".svn"),
            QStringLiteral("node_modules"), QStringLiteral("build"),
            QStringLiteral("bin"), QStringLiteral("obj"), QStringLiteral(".autodev")};
        while (it.hasNext() && m_aAbsFiles.size() < s_kiFolderCollectCap) {
            const QString sAbs = QDir::cleanPath(it.next());
            const QString sRel = CReviewSdkClient::RelativeToRoot(m_sTarget, sAbs);
            bool bSkip = false;
            for (const QString& sDir : s_aSkipDirs) {
                if (sRel.startsWith(sDir + QLatin1Char('/'), Qt::CaseInsensitive)) {
                    bSkip = true;
                    break;
                }
            }
            if (bSkip) {
                continue;
            }
            if (!pSdk->LanguageForFile(sAbs).isEmpty()) {
                m_aAbsFiles.append(sAbs);
            }
        }
    } else {
        m_sRootDir = stInfo.absolutePath();
    }
}

void CCodeReviewDialog::OnStart() {
    if (m_pSvc != nullptr && m_pSvc->IsRunning()) {
        return;
    }
    if (m_aAbsFiles.isEmpty()) {
        m_pLblStatus->setText(QStringLiteral("没有可审查的代码文件（目录为空或均不受支持）"));
        return;
    }
    QString sErr;
    if (!CReviewSdkClient::Get()->EnsureLoaded(&sErr)) {
        m_pLblStatus->setText(QStringLiteral("审查引擎不可用：%1\n（exe 同目录缺 "
            "ReviewSdk.dll 及其 Qt6Core.dll 依赖）").arg(sErr));
        return;
    }

    if (m_pSvc == nullptr) {
        m_pSvc = new CReviewService(this);
        connect(m_pSvc, &CReviewService::sigFileStart, this,
                &CCodeReviewDialog::OnFileStart);
        connect(m_pSvc, &CReviewService::sigFileDone, this,
                &CCodeReviewDialog::OnFileDone);
        connect(m_pSvc, &CReviewService::sigReviewDone, this,
                &CCodeReviewDialog::OnReviewDone);
    }

    m_pTree->clear();
    m_iDoneFiles  = 0;
    m_iFindings   = 0;
    m_sReportMd.clear();
    m_sCachedTime.clear();
    m_aCacheFiles = QJsonArray();                       // 2026-10-02 新一轮：逐文件结果重新累积
    m_aFp         = ComputeFingerprint();               // 开审前指纹（保存时比对基准）
    m_iTotalFiles = m_aAbsFiles.size();
    m_pProgress->setRange(0, m_iTotalFiles);
    m_pProgress->setValue(0);
    m_pBtnStart->setEnabled(false);
    m_pBtnStop->setEnabled(true);
    m_pBtnSave->setEnabled(false);
    m_pLblStatus->setText(QStringLiteral("开始审查：%1 个文件…")
                              .arg(m_iTotalFiles));
    UpdateCounts();

    if (!m_pSvc->Start(m_sRootDir, m_aAbsFiles, 600)) {
        m_pLblStatus->setText(QStringLiteral("审查启动失败（引擎不可用或正在运行）"));
        m_pBtnStart->setEnabled(true);
        m_pBtnStop->setEnabled(false);
        return;
    }
    m_pSpin->SetBusy(true);   // 2026-10-02 审查中转圈（完成/停止/失败在 OnReviewDone 停）
}

void CCodeReviewDialog::OnStop() {
    if (m_pSvc != nullptr) {
        m_pSvc->Stop();
        m_pLblStatus->setText(QStringLiteral("正在停止…"));
    }
}

void CCodeReviewDialog::OnFileStart(const QString& sRelPath, int iCur, int iTotal) {
    m_pLblStatus->setText(QStringLiteral("[%2/%3] 审查中：%1")
                              .arg(sRelPath)
                              .arg(iCur)
                              .arg(iTotal));
}

void CCodeReviewDialog::OnFileDone(const QString& sRelPath, int iRc,
                                   const QString& sFindingsJson, const QString& sErr) {
    ++m_iDoneFiles;
    m_pProgress->setValue(m_iDoneFiles);
    if (iRc == AUTOCODE_LCR_OK) {
        AddFindings(sRelPath, sFindingsJson);
    } else if (iRc == AUTOCODE_LCR_ERR_NO_RULES) {
        m_pLblStatus->setText(QStringLiteral("跳过（语言不受支持）：%1").arg(sRelPath));
    } else if (iRc != -1) {
        m_pLblStatus->setText(QStringLiteral("失败：%1（%2）").arg(sRelPath, sErr));
    }
    // 2026-10-02 逐文件结果入缓存累积（含跳过/失败，恢复时口径一致）。
    {
        QJsonObject oF;
        oF.insert(QStringLiteral("rel"), sRelPath);
        oF.insert(QStringLiteral("rc"), iRc);
        oF.insert(QStringLiteral("err"), sErr.left(600));
        const QJsonDocument oDoc = QJsonDocument::fromJson(sFindingsJson.toUtf8());
        oF.insert(QStringLiteral("f"), oDoc.isArray() ? oDoc.array() : QJsonArray());
        m_aCacheFiles.append(oF);
    }
    UpdateCounts();
}

void CCodeReviewDialog::OnReviewDone(bool bOk, bool bStopped, const QString& sFindingsJson,
                                     const QString& sReportMd, int iFilesReviewed,
                                     int iFindings) {
    m_sReportMd = sReportMd;
    m_pBtnStart->setEnabled(true);
    m_pBtnStop->setEnabled(false);
    m_pBtnSave->setEnabled(!sReportMd.isEmpty());
    m_pSpin->SetBusy(false);   // 2026-10-02 转圈停（完成/停止/失败统一在此收口）
    m_pLblStatus->setText(QStringLiteral("%1：送审 %2 个文件，发现 %3 条问题。")
                              .arg(bStopped ? QStringLiteral("已停止")
                                            : (bOk ? QStringLiteral("完成")
                                                   : QStringLiteral("结束（含失败文件）")),
                                   QString::number(iFilesReviewed),
                                   QString::number(iFindings)));
    // 2026-10-02 结果落盘：完整跑完（未被停止）且至少一个文件送审成功才缓存；
    //   全失败/被停止不落盘（下次打开重审）。
    if (!bStopped && iFilesReviewed > 0) {
        SaveCache(QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }
}

void CCodeReviewDialog::AddFindings(const QString& sRelPath, const QString& sFindingsJson) {
    const QJsonArray aArr = QJsonDocument::fromJson(sFindingsJson.toUtf8()).array();
    for (const auto& v : aArr) {
        const QJsonObject oF = v.toObject();
        const QString sSev  = oF.value(QStringLiteral("severity")).toString();
        const int iLine     = oF.value(QStringLiteral("line")).toInt();
        auto* pItem = new QTreeWidgetItem(m_pTree);
        pItem->setText(0, sSev);
        pItem->setText(1, oF.value(QStringLiteral("file")).toString(sRelPath));
        pItem->setText(2, iLine > 0 ? QString::number(iLine) : QStringLiteral("-"));
        pItem->setText(3, oF.value(QStringLiteral("rule")).toString());
        pItem->setText(4, oF.value(QStringLiteral("message")).toString());
        pItem->setText(5, oF.value(QStringLiteral("suggestion")).toString());
        pItem->setToolTip(4, oF.value(QStringLiteral("message")).toString());
        pItem->setToolTip(5, oF.value(QStringLiteral("suggestion")).toString());
        // 级别配色：high 红 / medium 橙 / low 灰。
        QColor clrSev(120, 120, 120);
        if (sSev == QStringLiteral("high")) {
            clrSev = QColor(200, 30, 30);
        } else if (sSev == QStringLiteral("medium")) {
            clrSev = QColor(210, 120, 0);
        }
        pItem->setForeground(0, clrSev);
        // 行数据：绝对路径 + 行号（双击打开定位）。
        pItem->setData(0, Qt::UserRole,
                       QDir::isAbsolutePath(pItem->text(1))
                           ? pItem->text(1)
                           : m_sRootDir + QLatin1Char('/') + pItem->text(1));
        pItem->setData(0, Qt::UserRole + 1, iLine);
    }
    m_iFindings += aArr.size();
}

void CCodeReviewDialog::UpdateCounts() {
    setWindowTitle(QStringLiteral("代码审查（%1 条问题 / %2 文件）")
                       .arg(QString::number(m_iFindings),
                            QString::number(m_iTotalFiles)));
    // 2026-10-02 有发现才可「按建议修复」。
    if (m_pBtnFix != nullptr) {
        m_pBtnFix->setEnabled(m_pTree != nullptr && m_pTree->topLevelItemCount() > 0);
    }
}

// 2026-10-02「按建议修复」：按用户指定模板拼修复提示词（审查意见=当前问题清单），
//   注入当前助手提问框（确认后手动发送；同「添加到聊天框」链路，不自动起 Agent）。
void CCodeReviewDialog::OnFixToChat() {
    if (m_pTree == nullptr || m_pTree->topLevelItemCount() == 0) {
        return;
    }
    QString sList;
    for (int i = 0; i < m_pTree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = m_pTree->topLevelItem(i);
        sList += QStringLiteral("- [%1] %2:%3（%4）%5\n")
                     .arg(pItem->text(0), pItem->text(1), pItem->text(2),
                          pItem->text(3), pItem->text(4));
        if (!pItem->text(5).isEmpty()) {
            sList += QStringLiteral("  建议：%1\n").arg(pItem->text(5));
        }
    }
    const QString sPrompt = QStringLiteral(
        "请根据审查意见修复代码（存在就修复，不存在忽略）：\n\n%1").arg(sList.trimmed());
    emit sigFixToChat(sPrompt);
}

void CCodeReviewDialog::OnItemDoubleClicked(QTreeWidgetItem* pItem, int) {
    if (pItem == nullptr) {
        return;
    }
    const QString sAbs = pItem->data(0, Qt::UserRole).toString();
    const int iLine    = pItem->data(0, Qt::UserRole + 1).toInt();
    if (!sAbs.isEmpty() && QFile::exists(sAbs)) {
        emit sigOpenFile(sAbs, iLine);
    }
}

void CCodeReviewDialog::OnSaveReport() {
    if (m_sReportMd.isEmpty()) {
        return;
    }
    const QString sDef = QStringLiteral("代码审查报告_%1.md")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd_HHmmss")));
    const QString sPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存审查报告"), sDef,
        QStringLiteral("Markdown (*.md)"));
    if (sPath.isEmpty()) {
        return;
    }
    QFile stFile(sPath);
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入 %1").arg(sPath));
        return;
    }
    stFile.write(m_sReportMd.toUtf8());
    stFile.close();
}

// ----------------------------------------------------------------------------
// —— 2026-10-02 结果缓存复用：进入审查页时代码未修改则直接展示上次结果 ——
QJsonArray CCodeReviewDialog::ComputeFingerprint() const {
    // mtime+size 指纹（秒级千文件也仅几十毫秒；agent 重写/重存必然改变 mtime）。
    QJsonArray aFp;
    for (const QString& sAbs : m_aAbsFiles) {
        const QFileInfo stInfo(sAbs);
        QJsonObject o;
        o.insert(QStringLiteral("rel"),
                 CReviewSdkClient::RelativeToRoot(m_sRootDir, sAbs));
        o.insert(QStringLiteral("mt"),
                 static_cast<qint64>(stInfo.lastModified().toMSecsSinceEpoch()));
        o.insert(QStringLiteral("sz"), static_cast<qint64>(stInfo.size()));
        aFp.append(o);
    }
    return aFp;
}

bool CCodeReviewDialog::TryRestoreCache() {
    if (m_aAbsFiles.isEmpty()) {
        return false;
    }
    QFile stFile(CacheFilePath(m_sTarget));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(stFile.readAll());
    stFile.close();
    if (!oDoc.isObject()) {
        return false;
    }
    const QJsonObject oRoot = oDoc.object();
    if (oRoot.value(QStringLiteral("version")).toInt() != 1
        || QDir::cleanPath(oRoot.value(QStringLiteral("target")).toString())
               != QDir::cleanPath(m_sTarget)) {
        return false;   // 旧版本/目标不一致：不命中
    }
    const QJsonArray aFiles = oRoot.value(QStringLiteral("files")).toArray();
    if (aFiles.isEmpty()) {
        return false;
    }
    // 指纹比对：送审清单的 mtime+size 任一变化 → 视为代码已修改，重新开审。
    const QString sFpNew = QString::fromUtf8(QJsonDocument(ComputeFingerprint())
                                                 .toJson(QJsonDocument::Compact));
    const QString sFpOld = QString::fromUtf8(
        QJsonDocument(oRoot.value(QStringLiteral("fp")).toArray())
            .toJson(QJsonDocument::Compact));
    if (sFpNew != sFpOld) {
        return false;
    }

    // 命中：回填表格/报告/进度（口径与实时审查一致），不自动起 LLM。
    m_sCachedTime = oRoot.value(QStringLiteral("time")).toString();
    m_sReportMd   = oRoot.value(QStringLiteral("md")).toString();
    m_iTotalFiles = aFiles.size();
    m_iDoneFiles  = aFiles.size();
    m_iFindings   = 0;
    m_pProgress->setRange(0, m_iTotalFiles);
    m_pProgress->setValue(m_iTotalFiles);
    int iFailed = 0;
    for (const auto& v : aFiles) {
        const QJsonObject oF = v.toObject();
        const int iRc = oF.value(QStringLiteral("rc")).toInt();
        if (iRc == AUTOCODE_LCR_OK) {
            AddFindings(oF.value(QStringLiteral("rel")).toString(),
                        QString::fromUtf8(QJsonDocument(oF.value(QStringLiteral("f")).toArray())
                                              .toJson(QJsonDocument::Compact)));
        } else if (iRc != AUTOCODE_LCR_ERR_NO_RULES && iRc != -1) {
            ++iFailed;
        }
    }
    m_pBtnStart->setEnabled(true);
    m_pBtnStop->setEnabled(false);
    m_pBtnSave->setEnabled(!m_sReportMd.isEmpty());
    m_pLblStatus->setText(QStringLiteral(
        "代码未修改，已显示上次审查结果（%1；送审 %2 个文件，发现 %3 条问题%4）。"
        "点「重新审查」强制重审。")
        .arg(m_sCachedTime, QString::number(m_iTotalFiles),
             QString::number(m_iFindings),
             iFailed > 0 ? QStringLiteral("，含 %1 个失败文件").arg(iFailed)
                         : QString()));
    UpdateCounts();
    return true;
}

void CCodeReviewDialog::SaveCache(const QString& a_sTime) {
    QDir().mkpath(CacheDirPath());
    QJsonObject oRoot;
    oRoot.insert(QStringLiteral("version"), 1);
    oRoot.insert(QStringLiteral("target"), m_sTarget);
    oRoot.insert(QStringLiteral("time"), a_sTime);
    oRoot.insert(QStringLiteral("md"), m_sReportMd);
    oRoot.insert(QStringLiteral("fp"), m_aFp);
    oRoot.insert(QStringLiteral("files"), m_aCacheFiles);
    QFile stFile(CacheFilePath(m_sTarget));
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;   // 缓存写失败静默（仅丢失复用能力，不影响本次结果展示）
    }
    stFile.write(QJsonDocument(oRoot).toJson(QJsonDocument::Compact));
    stFile.close();
}
