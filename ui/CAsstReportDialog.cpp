// CAsstReportDialog.cpp —— 助手分析报告页（2026-11-04 用户需求）。
// 语义与生命周期见头注。正文渲染复用 CArtifactView；导出恒 UTF-8 with BOM
// （遵循全局编码规则）。

#include "ui/CAsstReportDialog.h"

#include "ui/CArtifactView.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// 11pt 字号（与主程序统一口径；对话框根设一次，子控件继承）。
void SetPt11(QWidget* a_pW) {
    QFont stFont = a_pW->font();
    stFont.setPointSize(11);
    a_pW->setFont(stFont);
}

} // namespace

CAsstReportDialog::CAsstReportDialog(QWidget* a_pParent)
    : QDialog(a_pParent)
{
    setWindowTitle(QStringLiteral("分析报告"));
    setAttribute(Qt::WA_DeleteOnClose);   // 宿主面板持 QPointer，关闭即销毁
    setModal(false);
    SetPt11(this);
    resize(780, 640);

    auto* pLayout = new QVBoxLayout(this);
    // 标题行：助手名 + 生成时间/说明；右侧「导出报告…」。
    auto* pHeadRow = new QHBoxLayout();
    m_pHead = new QLabel(this);
    m_pHead->setWordWrap(true);
    pHeadRow->addWidget(m_pHead, 1);
    m_pBtnExport = new QPushButton(QStringLiteral("导出报告…"), this);
    m_pBtnExport->setToolTip(QStringLiteral(
        "把当前报告另存为 Markdown 文件（UTF-8 编码）"));
    m_pBtnExport->setEnabled(false);   // 无正文不可导出（ShowReport 置位）
    connect(m_pBtnExport, &QPushButton::clicked, this, &CAsstReportDialog::OnExport);
    pHeadRow->addWidget(m_pBtnExport);
    pLayout->addLayout(pHeadRow);

    // 状态横幅：后台总结进行中/静态提示/导出结果（默认隐藏）。
    m_pBanner = new QLabel(this);
    m_pBanner->setWordWrap(true);
    m_pBanner->setStyleSheet(
        QStringLiteral("QLabel{background:#EFF6FF;color:#1D4ED8;"
                       "border:1px solid #BFDBFE;border-radius:4px;"
                       "padding:6px 8px;}"));
    m_pBanner->hide();
    pLayout->addWidget(m_pBanner);

    // 正文：Markdown 渲染（复用阶段产物查看器）。
    m_pView = new CArtifactView(this);
    m_pView->setPlaceholderText(QStringLiteral(
        "该助手还没有分析报告。\n\n右键助手 Tab 打开「查看分析报告」后，"
        "系统会根据对话日志在后台自动总结生成。"));
    m_pView->setOpenExternalLinks(false);   // 报告内不期望外链；保守不弹浏览器
    pLayout->addWidget(m_pView, 1);
}

void CAsstReportDialog::ApplyBanner() {
    if (m_bGenerating) {
        m_pBanner->setText(m_sBanner.isEmpty()
            ? QStringLiteral("⚙ 正在后台总结对话日志，完成后自动更新报告…")
            : QStringLiteral("⚙ %1").arg(m_sBanner));
        m_pBanner->show();
    } else if (!m_sBanner.isEmpty()) {
        m_pBanner->setText(m_sBanner);
        m_pBanner->show();
    } else {
        m_pBanner->hide();
    }
}

void CAsstReportDialog::ShowReport(const QString& a_sAsstId, const QString& a_sAsstName,
                                   const QString& a_sReport, const QString& a_sMetaNote,
                                   bool a_bStale) {
    m_sAsstId = a_sAsstId;
    m_sAsstName = a_sAsstName;
    m_sReport = a_sReport;
    setWindowTitle(QStringLiteral("分析报告 — %1").arg(a_sAsstName));
    m_pHead->setText(a_sMetaNote.isEmpty()
        ? QStringLiteral("「%1」的对话分析报告").arg(a_sAsstName)
        : QStringLiteral("「%1」的对话分析报告　·　%2").arg(a_sAsstName, a_sMetaNote));
    m_pView->ShowMarkdownText(a_sReport);
    m_pBtnExport->setEnabled(!a_sReport.trimmed().isEmpty());
    // 旧报告照常显示，横幅提示后台更新中（完成后 ApplyAsstReport 再整体换新）。
    if (a_bStale) {
        m_sBanner = QStringLiteral("对话日志有新增，正在后台重新总结，完成后自动替换…");
    } else {
        m_sBanner.clear();
    }
    m_bGenerating = a_bStale;
    ApplyBanner();
    show();
    raise();
    activateWindow();
}

void CAsstReportDialog::SetGenerating(const QString& a_sAsstId, bool a_bOn,
                                      const QString& a_sHint) {
    // 只影响当前展示的助手；切到其他助手的报告时旧请求的完成信号不再动新内容。
    if (m_sAsstId != a_sAsstId) {
        return;
    }
    m_bGenerating = a_bOn;
    m_sBanner = a_bOn ? a_sHint : QString();
    ApplyBanner();
}

void CAsstReportDialog::SetHint(const QString& a_sAsstId, const QString& a_sAsstName,
                                const QString& a_sHint) {
    m_sAsstId = a_sAsstId;
    m_sAsstName = a_sAsstName;
    m_sReport.clear();
    setWindowTitle(QStringLiteral("分析报告 — %1").arg(a_sAsstName));
    m_pHead->setText(QStringLiteral("「%1」的对话分析报告").arg(a_sAsstName));
    m_pView->Clear();
    m_pBtnExport->setEnabled(false);
    m_sBanner = a_sHint;
    m_bGenerating = false;
    ApplyBanner();
    show();
    raise();
    activateWindow();
}

void CAsstReportDialog::OnExport() {
    // 导出当前报告原始 Markdown；UTF-8 with BOM（中文 Windows 下记事本/编辑器
    //   兼容最佳）。失败弹窗提示，成功横幅显示导出路径。
    if (m_sReport.trimmed().isEmpty()) {
        return;
    }
    const QString sDef = QStringLiteral("%1-分析报告-%2.md")
        .arg(m_sAsstName.isEmpty() ? QStringLiteral("助手")
                                   : m_sAsstName,
             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString sPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出分析报告"), sDef,
        QStringLiteral("Markdown (*.md);;文本文件 (*.txt);;所有文件 (*.*)"));
    if (sPath.isEmpty()) {
        return;   // 用户取消
    }
    QFile stFile(sPath);
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
            QStringLiteral("无法写入文件：\n%1").arg(sPath));
        return;
    }
    const QByteArray aBody = QByteArray("\xEF\xBB\xBF") + m_sReport.toUtf8();
    if (stFile.write(aBody) != aBody.size()) {
        stFile.close();
        QMessageBox::warning(this, QStringLiteral("导出失败"),
            QStringLiteral("写入未完成（磁盘已满或路径不可写）：\n%1").arg(sPath));
        return;
    }
    stFile.close();
    m_sBanner = QStringLiteral("✓ 已导出到：%1").arg(sPath);
    m_bGenerating = false;
    ApplyBanner();
}
