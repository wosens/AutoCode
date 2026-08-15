#include "CSettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "CProjectManager.h"

CSettingsDialog::CSettingsDialog(CProjectManager* a_pPm, QWidget* a_pParent)
    : QDialog(a_pParent) {
    SetupUi();

    // 构造时回填当前值（a_pPm 为空 → 全默认，仅调试/极端场景）。
    const int iDefMaxIter = (a_pPm != nullptr) ? a_pPm->LoadMaxIter() : 3;
    const int iDefMaxResume = (a_pPm != nullptr) ? a_pPm->LoadMaxResume() : 3;
    const int iDefReviewResume = (a_pPm != nullptr) ? a_pPm->LoadReviewResume() : 1;
    const int iDefReviewPar = (a_pPm != nullptr) ? a_pPm->LoadReviewParallel() : 3;
    const QString sDefCmd = (a_pPm != nullptr) ? a_pPm->LoadClaudeCmd()
                                               : QStringLiteral("claude");
    const int iDefAskTimeout = (a_pPm != nullptr) ? a_pPm->LoadAskTimeoutSec() : 600;
    const bool bDefWeb = (a_pPm != nullptr) ? a_pPm->LoadWebSearch() : true;
    const int iDefPct = (a_pPm != nullptr) ? a_pPm->LoadAutoCompactPct() : 75;
    const qint64 iDefCtx = (a_pPm != nullptr) ? a_pPm->LoadAdhocContextLimit() : 0;
    const QString sDefScan = (a_pPm != nullptr) ? a_pPm->LoadScanExcludeDirs() : QString();
    const QString sDefBackup = (a_pPm != nullptr) ? a_pPm->LoadBackupExcludeDirs() : QString();

    m_pSpinMaxIter->setValue(iDefMaxIter);
    m_pSpinMaxResume->setValue(iDefMaxResume);
    m_pSpinReviewResume->setValue(iDefReviewResume);
    m_pSpinReviewPar->setValue(iDefReviewPar);
    m_pEditClaudeCmd->setText(sDefCmd);
    m_pSpinAskTimeout->setValue(iDefAskTimeout);
    m_pChkWebSearch->setChecked(bDefWeb);
    m_pSpinCompactPct->setValue(iDefPct);
    // 上下文上限以「千 token」展示更友好：内部 0=自动；展示 0 也表示自动（suffix 提示）。
    m_pSpinCtxLimit->setValue(
        static_cast<int>(qBound(qint64(0), iDefCtx / 1000, qint64(4000))));
    m_pEditScanDirs->setText(sDefScan);
    m_pEditBackupDirs->setText(sDefBackup);
}

void CSettingsDialog::SetupUi() {
    setWindowTitle(QStringLiteral("设置"));
    setMinimumWidth(520);

    // —— 运行策略 ——
    auto* pGroupPolicy = new QGroupBox(QStringLiteral("运行策略"), this);
    auto* pFormPolicy = new QFormLayout(pGroupPolicy);
    m_pSpinMaxIter = new QSpinBox(pGroupPolicy);
    m_pSpinMaxIter->setRange(1, 9);
    m_pSpinMaxIter->setToolTip(QStringLiteral(
        "单个阶段被评审驳回后的最大重跑次数，超过则熔断暂停并升级到用户处理（config.ini [Policy] MaxIter）"));
    pFormPolicy->addRow(QStringLiteral("单阶段最大迭代次数："), m_pSpinMaxIter);
    m_pSpinMaxResume = new QSpinBox(pGroupPolicy);
    m_pSpinMaxResume->setRange(1, 9);
    m_pSpinMaxResume->setToolTip(QStringLiteral(
        "阶段命中轮次上限后自动 --resume 续跑的封顶次数（[Policy] MaxResume）"));
    pFormPolicy->addRow(QStringLiteral("阶段续跑上限（次）："), m_pSpinMaxResume);
    m_pSpinReviewResume = new QSpinBox(pGroupPolicy);
    m_pSpinReviewResume->setRange(1, 9);
    m_pSpinReviewResume->setToolTip(QStringLiteral(
        "轻量评审命中轮次上限后催促落盘结论的续跑封顶次数（[Policy] ReviewResume）"));
    pFormPolicy->addRow(QStringLiteral("评审续跑上限（次）："), m_pSpinReviewResume);

    // —— Agent ——
    auto* pGroupAgent = new QGroupBox(QStringLiteral("Agent"), this);
    auto* pFormAgent = new QFormLayout(pGroupAgent);
    m_pEditClaudeCmd = new QLineEdit(pGroupAgent);
    m_pEditClaudeCmd->setPlaceholderText(QStringLiteral("claude"));
    m_pEditClaudeCmd->setToolTip(QStringLiteral(
        "claude 命令；默认 claude（PATH 查找），可填绝对路径指定安装（[Agent] ClaudeCmd）"));
    pFormAgent->addRow(QStringLiteral("claude 命令："), m_pEditClaudeCmd);
    m_pSpinAskTimeout = new QSpinBox(pGroupAgent);
    m_pSpinAskTimeout->setRange(30, 86400);
    m_pSpinAskTimeout->setSuffix(QStringLiteral(" 秒"));
    m_pSpinAskTimeout->setToolTip(QStringLiteral(
        "ask_user 提问等待用户回答的超时（[Agent] AskTimeoutSec，默认 600）"));
    pFormAgent->addRow(QStringLiteral("提问等待超时："), m_pSpinAskTimeout);
    m_pChkWebSearch = new QCheckBox(QStringLiteral("允许阶段联网检索（WebSearch/WebFetch）"), pGroupAgent);
    m_pChkWebSearch->setToolTip(QStringLiteral(
        "关闭后所有阶段的 --disallowedTools 追加 WebSearch、WebFetch（离线/内网环境用；[Agent] WebSearch）"));
    pFormAgent->addRow(QString(), m_pChkWebSearch);

    // —— 评审 / 助手 ——
    auto* pGroupReview = new QGroupBox(QStringLiteral("评审与助手"), this);
    auto* pFormReview = new QFormLayout(pGroupReview);
    m_pSpinReviewPar = new QSpinBox(pGroupReview);
    m_pSpinReviewPar->setRange(1, 8);
    m_pSpinReviewPar->setToolTip(QStringLiteral(
        "每个评审阶段并行拉起的视角数，1=串行回退（[Review] Parallel，默认 3）"));
    pFormReview->addRow(QStringLiteral("评审并行视角数："), m_pSpinReviewPar);
    m_pSpinCompactPct = new QSpinBox(pGroupReview);
    m_pSpinCompactPct->setRange(10, 95);
    m_pSpinCompactPct->setSuffix(QStringLiteral(" %"));
    m_pSpinCompactPct->setToolTip(QStringLiteral(
        "上下文占用达到窗口的该比例时触发自动压缩；阈值 = 窗口 × 比例"
        "（[Adhoc] AutoCompactPct）"));
    pFormReview->addRow(QStringLiteral("自动压缩比例："), m_pSpinCompactPct);
    m_pSpinCtxLimit = new QSpinBox(pGroupReview);
    m_pSpinCtxLimit->setRange(0, 4000);
    m_pSpinCtxLimit->setSuffix(QStringLiteral(" k"));
    m_pSpinCtxLimit->setSpecialValueText(QStringLiteral("自动"));
    m_pSpinCtxLimit->setToolTip(QStringLiteral(
        "助手上下文窗口上限（千 token）；0=按模型自动探测（[Adhoc] ContextLimit）"));
    pFormReview->addRow(QStringLiteral("上下文窗口上限："), m_pSpinCtxLimit);

    // —— 目录排除 ——
    auto* pGroupDirs = new QGroupBox(QStringLiteral("目录排除"), this);
    auto* pFormDirs = new QFormLayout(pGroupDirs);
    m_pEditScanDirs = new QLineEdit(pGroupDirs);
    m_pEditScanDirs->setPlaceholderText(QStringLiteral(
        ".autodev,build,Execute,.git,.vs,node_modules,doc"));
    m_pEditScanDirs->setToolTip(QStringLiteral(
        "guide 指纹扫描排除的目录名，逗号分隔；留空 = 内置默认（[Scan] ExcludeDirs）"));
    pFormDirs->addRow(QStringLiteral("指纹扫描排除："), m_pEditScanDirs);
    m_pEditBackupDirs = new QLineEdit(pGroupDirs);
    m_pEditBackupDirs->setPlaceholderText(QStringLiteral(
        ".autodev,build,Execute,.git,.vs,x64"));
    m_pEditBackupDirs->setToolTip(QStringLiteral(
        "代码备份/恢复排除的目录名，逗号分隔；留空 = 内置默认（[Backup] ExcludeDirs）"));
    pFormDirs->addRow(QStringLiteral("备份恢复排除："), m_pEditBackupDirs);

    // —— 按钮 ——
    auto* pBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    pBtns->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    pBtns->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(pBtns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(pBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* pLayoutMain = new QVBoxLayout(this);
    pLayoutMain->addWidget(pGroupPolicy);
    pLayoutMain->addWidget(pGroupAgent);
    pLayoutMain->addWidget(pGroupReview);
    pLayoutMain->addWidget(pGroupDirs);
    pLayoutMain->addWidget(pBtns);
}

int CSettingsDialog::MaxIter() const {
    return m_pSpinMaxIter->value();
}

int CSettingsDialog::MaxResume() const {
    return m_pSpinMaxResume->value();
}

int CSettingsDialog::ReviewResume() const {
    return m_pSpinReviewResume->value();
}

int CSettingsDialog::ReviewParallel() const {
    return m_pSpinReviewPar->value();
}

QString CSettingsDialog::ClaudeCmd() const {
    return m_pEditClaudeCmd->text().trimmed();
}

int CSettingsDialog::AskTimeoutSec() const {
    return m_pSpinAskTimeout->value();
}

bool CSettingsDialog::WebSearch() const {
    return m_pChkWebSearch->isChecked();
}

int CSettingsDialog::AutoCompactPct() const {
    return m_pSpinCompactPct->value();
}

qint64 CSettingsDialog::ContextLimit() const {
    return static_cast<qint64>(m_pSpinCtxLimit->value()) * 1000;
}

QString CSettingsDialog::ScanExcludeDirs() const {
    return m_pEditScanDirs->text().trimmed();
}

QString CSettingsDialog::BackupExcludeDirs() const {
    return m_pEditBackupDirs->text().trimmed();
}
