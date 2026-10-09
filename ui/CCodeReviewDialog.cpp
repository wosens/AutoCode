// CCodeReviewDialog.cpp —— 手动代码审查对话框实现
// 2026-10-08 Tab 化改造：选择文件（过滤+复选清单）/ 审查（发现表+复选）两页；
//   顶部「全量审查」开关（缺省不勾=仅自上次审查后修改/新增的文件且逐文件增量审，
//   基线=exe 同级 review_cache/<目标哈希>.bl 内容快照仓）。

#include "CCodeReviewDialog.h"
#include "review/CReviewSdkClient.h"
#include "review/CReviewService.h"
#include "agent/CAgentRunnerFactory.h"
#include "CTipStyle.h"   // 2026-10-08 条目 tooltip 自动换行（同 CExecutionPanel 条目 tip 口径）

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
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
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QTableWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // 过滤档案 GetPrivateProfileStringW/WritePrivateProfileStringW
#endif

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
QString KeyOfTarget(const QString& a_sTarget) {
    return QString::fromLatin1(QCryptographicHash::hash(
        QDir::cleanPath(a_sTarget).normalized(QString::NormalizationForm_C)
            .toLower().toUtf8(),
        QCryptographicHash::Sha256).toHex()).left(24);
}
QString CacheFilePath(const QString& a_sTarget) {
    return CacheDirPath() + QStringLiteral("/") + KeyOfTarget(a_sTarget)
         + QStringLiteral(".json");
}

// exe 同目录 config.ini（与 CProjectManager::ConfigFilePath 同口径：GetModuleFileName
// 取 exe 全路径——此处用 applicationDirPath 等价，禁用 GetCurrentDirectory）。
QString GlobalConfigIni() {
    return QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/config.ini"));
}

// 大小人性化（清单「大小」列）。
QString HumanSize(qint64 a_iBytes) {
    if (a_iBytes < 1024) {
        return QStringLiteral("%1 B").arg(a_iBytes);
    }
    if (a_iBytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(a_iBytes / 1024);
    }
    return QString::number(a_iBytes / 1024.0 / 1024.0, 'f', 1) + QStringLiteral(" MB");
}

// 2026-10-08 缺省过滤档案（与 ReviewSdk 10 语言中用户指定的 8 项对齐；
//   JavaScript/TypeScript 可经「编辑过滤…」自行添加）。
QList<SReviewFilter> DefaultFilterProfiles() {
    return {
        {QStringLiteral("C/C++"),        QStringLiteral("cpp;cc;cxx;c;h;hh;hpp;hxx;inl")},
        {QStringLiteral("Python"),       QStringLiteral("py;pyw")},
        {QStringLiteral("C#"),           QStringLiteral("cs")},
        {QStringLiteral("Delphi"),       QStringLiteral("pas;dpr;dpk;inc")},
        {QStringLiteral("Visual Basic"), QStringLiteral("bas;vb;vbs")},
        {QStringLiteral("Rust"),         QStringLiteral("rs")},
        {QStringLiteral("Go"),           QStringLiteral("go")},
        {QStringLiteral("Java"),         QStringLiteral("java")},
    };
}
}   // namespace

// ----------------------------------------------------------------------------
// —— 2026-10-08 过滤档案编辑对话框（文件局部类；无自定义信号 → 免 Q_OBJECT/moc，
//    按钮全部 lambda 连接；表格两列：名称 / 扩展名（分号分隔））——
class CReviewFilterEditDialog : public QDialog {
public:
    explicit CReviewFilterEditDialog(const QList<SReviewFilter>& a_aCur, QWidget* a_pParent)
        : QDialog(a_pParent) {
        setWindowTitle(QStringLiteral("编辑过滤条件"));
        setMinimumSize(420, 320);
        auto* pLay = new QVBoxLayout(this);

        auto* pLblHint = new QLabel(
            QStringLiteral("每行一个过滤项：名称 + 扩展名（分号分隔，不含点，如 cpp;h;hpp）。"
                           "「选择文件」页下拉框按此列出，可在各过滤项间切换。"), this);
        pLblHint->setWordWrap(true);
        QFont fH = pLblHint->font();
        fH.setPointSize(11);
        pLblHint->setFont(fH);
        pLay->addWidget(pLblHint);

        m_pTable = new QTableWidget(static_cast<int>(a_aCur.size()), 2, this);
        m_pTable->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("扩展名")});
        m_pTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_pTable->verticalHeader()->setVisible(false);
        QFont fT = m_pTable->font();
        fT.setPointSize(11);
        m_pTable->setFont(fT);
        for (int i = 0; i < a_aCur.size(); ++i) {
            m_pTable->setItem(i, 0, new QTableWidgetItem(a_aCur.at(i).sName));
            m_pTable->setItem(i, 1, new QTableWidgetItem(a_aCur.at(i).sExts));
        }
        pLay->addWidget(m_pTable, 1);

        auto* pLayRow = new QHBoxLayout();
        auto* pBtnAdd = new QPushButton(QStringLiteral("添加"), this);
        auto* pBtnDel = new QPushButton(QStringLiteral("删除选中行"), this);
        auto* pBtnDef = new QPushButton(QStringLiteral("恢复缺省"), this);
        for (QPushButton* pB : {pBtnAdd, pBtnDel, pBtnDef}) {
            QFont fB = pB->font();
            fB.setPointSize(11);
            pB->setFont(fB);
        }
        pLayRow->addWidget(pBtnAdd);
        pLayRow->addWidget(pBtnDel);
        pLayRow->addStretch(1);
        pLayRow->addWidget(pBtnDef);
        pLay->addLayout(pLayRow);

        auto* pLayBtn = new QHBoxLayout();
        auto* pBtnOk  = new QPushButton(QStringLiteral("确定"), this);
        auto* pBtnCancel = new QPushButton(QStringLiteral("取消"), this);
        for (QPushButton* pB : {pBtnOk, pBtnCancel}) {
            QFont fB = pB->font();
            fB.setPointSize(11);
            pB->setFont(fB);
        }
        pLayBtn->addStretch(1);
        pLayBtn->addWidget(pBtnOk);
        pLayBtn->addWidget(pBtnCancel);
        pLay->addLayout(pLayBtn);

        connect(pBtnAdd, &QPushButton::clicked, this, [this]() {
            const int iRow = m_pTable->rowCount();
            m_pTable->insertRow(iRow);
            m_pTable->setItem(iRow, 0, new QTableWidgetItem(QStringLiteral("新过滤项")));
            m_pTable->setItem(iRow, 1, new QTableWidgetItem(QStringLiteral("ext")));
            m_pTable->editItem(m_pTable->item(iRow, 0));
        });
        connect(pBtnDel, &QPushButton::clicked, this, [this]() {
            const int iRow = m_pTable->currentRow();
            if (iRow >= 0) {
                m_pTable->removeRow(iRow);
            }
        });
        connect(pBtnDef, &QPushButton::clicked, this, [this]() {
            const QList<SReviewFilter> aDef = DefaultFilterProfiles();
            m_pTable->setRowCount(static_cast<int>(aDef.size()));
            for (int i = 0; i < aDef.size(); ++i) {
                m_pTable->setItem(i, 0, new QTableWidgetItem(aDef.at(i).sName));
                m_pTable->setItem(i, 1, new QTableWidgetItem(aDef.at(i).sExts));
            }
        });
        connect(pBtnOk, &QPushButton::clicked, this, &QDialog::accept);
        connect(pBtnCancel, &QPushButton::clicked, this, &QDialog::reject);
    }

    // 汇总表格内容（空名称/空扩展名行丢弃；扩展名规整为小写去点）。
    QList<SReviewFilter> Result() const {
        QList<SReviewFilter> aOut;
        for (int i = 0; i < m_pTable->rowCount(); ++i) {
            const QTableWidgetItem* pN = m_pTable->item(i, 0);
            const QTableWidgetItem* pE = m_pTable->item(i, 1);
            if (pN == nullptr || pE == nullptr) {
                continue;
            }
            QStringList aExts;
            for (const QString& s : pE->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                QString sExt = s.trimmed().toLower();
                while (sExt.startsWith(QLatin1Char('.'))) {
                    sExt.remove(0, 1);
                }
                if (!sExt.isEmpty()) {
                    aExts.append(sExt);
                }
            }
            if (!pN->text().trimmed().isEmpty() && !aExts.isEmpty()) {
                aOut.append({pN->text().trimmed(), aExts.join(QLatin1Char(';'))});
            }
        }
        return aOut;
    }

private:
    QTableWidget* m_pTable = nullptr;
};

// ----------------------------------------------------------------------------
CCodeReviewDialog::CCodeReviewDialog(const QString& a_sTarget, QWidget* a_pParent)
    : QDialog(a_pParent), m_sTarget(QDir::cleanPath(a_sTarget)) {
    // 多窗口并存时靠标题区分目标（目录名/文件名；盘根回退全路径）。
    m_sTargetName = QFileInfo(m_sTarget).fileName();
    if (m_sTargetName.isEmpty()) {
        m_sTargetName = m_sTarget;
    }
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    BuildUi();
    m_aFilters = LoadFilters();
    FillFilterCombo();   // 2026-10-08 「全部」+ 过滤档案项（缺省 8 项，见 DefaultFilterProfiles）
    CollectFiles();
    PopulateFileList();
    // 2026-10-08 起不自动开审：用户在「选择文件」页勾选后右键/按钮开始；
    //   代码未修改（全候选清单指纹与上次一致）→ 直接回填上次结果并停在审查页。
    if (!TryRestoreCache()) {
        m_pLblStatus->setText(QStringLiteral(
            "就绪：在「选择文件」页勾选待审文件后开始审查"
            "（未勾「全量审查」时只列/只审自上次审查后修改的部分）。"));
    }
}

CCodeReviewDialog::~CCodeReviewDialog() {
    if (m_pSvc != nullptr) {
        m_pSvc->Stop();
        m_pSvc->deleteLater();
    }
}

// 2026-10-08 运行态查询（主窗口退出确认枚举在跑的审查窗口）。
bool CCodeReviewDialog::IsReviewRunning() const {
    return (m_pSvc != nullptr && m_pSvc->IsRunning()) || m_bReviewRunning;
}

// 2026-10-08 审查进行中关窗确认（用户需求）：缺省/Esc=继续审查（误触保护）；
//   确认关闭 → 析构链 Stop 中止本轮——已完成文件的发现已入表、基线已逐文件前移，
//   被停止的轮次结果缓存不落盘（口径与「停止」按钮一致）。
void CCodeReviewDialog::closeEvent(QCloseEvent* a_pEvent) {
    if (!IsReviewRunning()) {
        QDialog::closeEvent(a_pEvent);
        return;
    }
    QMessageBox oBox(this);
    oBox.setIcon(QMessageBox::Warning);
    oBox.setWindowTitle(QStringLiteral("关闭代码审查窗口"));
    oBox.setText(QStringLiteral(
        "「%1」的审查正在进行（进度 %2/%3），关闭窗口将中止本轮审查。"
        "\n已完成文件的结果保留，未审完的文件下次可重新勾选审查。\n\n确定关闭吗？")
        .arg(m_sTargetName,
             QString::number(m_iDoneFiles), QString::number(m_iTotalFiles)));
    QPushButton* pBtnClose = oBox.addButton(
        QStringLiteral("关闭并中止"), QMessageBox::YesRole);
    QPushButton* pBtnGoOn = oBox.addButton(
        QStringLiteral("继续审查"), QMessageBox::NoRole);
    oBox.setDefaultButton(pBtnGoOn);   // 缺省选择：继续（不关）
    oBox.setEscapeButton(pBtnGoOn);    // Esc 同继续（误触保护）
    oBox.exec();
    if (oBox.clickedButton() == pBtnClose) {
        QDialog::closeEvent(a_pEvent);   // 确认 → 走正常关闭（析构链 Stop 中止）
    } else {
        a_pEvent->ignore();
    }
}

void CCodeReviewDialog::BuildUi() {
    setWindowTitle(QStringLiteral("代码审查：%1").arg(m_sTargetName));
    setMinimumSize(900, 560);
    setSizeGripEnabled(true);

    auto* pLay = new QVBoxLayout(this);

    m_pLblTarget = new QLabel(this);
    m_pLblTarget->setWordWrap(true);
    QFont fT = m_pLblTarget->font();
    fT.setPointSize(11);
    m_pLblTarget->setFont(fT);
    pLay->addWidget(m_pLblTarget);

    // —— 2026-10-08 顶部「全量审查」开关（缺省不勾）——
    m_pChkFull = new QCheckBox(QStringLiteral("全量审查"), this);
    QFont fC = m_pChkFull->font();
    fC.setPointSize(11);
    m_pChkFull->setFont(fC);
    m_pChkFull->setChecked(false);
    m_pChkFull->setToolTip(QStringLiteral(
        "不勾选（缺省）：文件清单只列自上次审查后修改/新增的文件，审查只送修改部分"
        "（增量，首次审查自动全量）。\n勾选：列出全部文件并完整审查。"));
    connect(m_pChkFull, &QCheckBox::toggled, this, &CCodeReviewDialog::OnFullToggled);
    pLay->addWidget(m_pChkFull);

    // —— 2026-10-08 双 Tab：选择文件 / 审查 ——
    m_pTabs = new QTabWidget(this);
    QFont fTab = m_pTabs->font();
    fTab.setPointSize(11);
    m_pTabs->setFont(fTab);
    pLay->addWidget(m_pTabs, 1);

    // ▍选择文件页
    auto* pPageFiles = new QWidget(this);
    auto* pLayFiles = new QVBoxLayout(pPageFiles);
    pLayFiles->setContentsMargins(6, 6, 6, 6);

    auto* pLayFilter = new QHBoxLayout();
    m_pCboFilter = new QComboBox(this);
    QFont fCb = m_pCboFilter->font();
    fCb.setPointSize(11);
    m_pCboFilter->setFont(fCb);
    m_pCboFilter->addItem(QStringLiteral("全部"), QStringLiteral("*"));
    m_pBtnEditFit = new QPushButton(QStringLiteral("编辑过滤…"), this);
    QFont fE = m_pBtnEditFit->font();
    fE.setPointSize(11);
    m_pBtnEditFit->setFont(fE);
    m_pBtnEditFit->setToolTip(QStringLiteral(
        "编辑各过滤项的扩展名条件，或添加新的过滤项（存 config.ini [Review] Filters）"));
    m_pLblCount = new QLabel(this);
    QFont fN = m_pLblCount->font();
    fN.setPointSize(11);
    m_pLblCount->setFont(fN);
    pLayFilter->addWidget(m_pCboFilter);
    pLayFilter->addWidget(m_pBtnEditFit);
    pLayFilter->addStretch(1);
    pLayFilter->addWidget(m_pLblCount);
    pLayFiles->addLayout(pLayFilter);

    m_pTreeFiles = new QTreeWidget(this);
    QFont fTreeF = m_pTreeFiles->font();
    fTreeF.setPointSize(11);
    m_pTreeFiles->setFont(fTreeF);
    m_pTreeFiles->setColumnCount(5);
    m_pTreeFiles->setHeaderLabels({QStringLiteral("选"), QStringLiteral("文件"),
                                   QStringLiteral("状态"), QStringLiteral("大小"),
                                   QStringLiteral("修改时间")});
    m_pTreeFiles->setRootIsDecorated(false);
    m_pTreeFiles->setAlternatingRowColors(true);
    m_pTreeFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTreeFiles->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTreeFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTreeFiles->header()->setSectionResizeMode(QHeaderView::Interactive);   // 2026-10-08 全列允许手工拖宽
    m_pTreeFiles->header()->setStretchLastSection(true);
    m_pTreeFiles->header()->resizeSection(0, 40);
    m_pTreeFiles->header()->resizeSection(1, 380);
    m_pTreeFiles->header()->resizeSection(2, 76);
    m_pTreeFiles->header()->resizeSection(3, 84);
    m_pTreeFiles->header()->resizeSection(4, 132);
    m_pTreeFiles->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTreeFiles, &QTreeWidget::customContextMenuRequested,
            this, &CCodeReviewDialog::OnFilesContextMenu);
    connect(m_pTreeFiles, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int a_iCol) {
        if (a_iCol == 0 && !m_bPopulating) {
            RefreshCountLabel();
        }
    });
    pLayFiles->addWidget(m_pTreeFiles, 1);
    m_pTabs->addTab(pPageFiles, QStringLiteral("选择文件"));

    // ▍审查页
    auto* pPageReview = new QWidget(this);
    auto* pLayReview = new QVBoxLayout(pPageReview);
    pLayReview->setContentsMargins(6, 6, 6, 6);

    m_pProgress = new QProgressBar(this);
    m_pProgress->setTextVisible(false);
    m_pProgress->setRange(0, 1);
    m_pProgress->setValue(0);
    pLayReview->addWidget(m_pProgress);

    // 2026-10-02 状态行首动态图标：审查中转圈（12 段弧 70ms/帧），空闲静态灰点。
    auto* pLayStatus = new QHBoxLayout();
    pLayStatus->setContentsMargins(0, 0, 0, 0);
    pLayStatus->setSpacing(6);
    m_pSpin = new CReviewSpinner(this);
    m_pSpin->setObjectName(QStringLiteral("ReviewSpinner"));   // 无 Q_OBJECT，className 不可靠（同 CPromptFlyout 先例）
    m_pSpin->setToolTip(QStringLiteral("审查进行中"));
    pLayStatus->addWidget(m_pSpin);
    m_pLblStatus = new QLabel(QStringLiteral("就绪"), this);
    m_pLblStatus->setWordWrap(true);
    QFont fS = m_pLblStatus->font();
    fS.setPointSize(11);
    m_pLblStatus->setFont(fS);
    pLayStatus->addWidget(m_pLblStatus, 1);
    pLayReview->addLayout(pLayStatus);

    // 2026-10-08 发现表：行首加复选框列（缺省全勾；按建议修复只带走勾选行）。
    m_pTree = new QTreeWidget(this);
    QFont fTree = m_pTree->font();
    fTree.setPointSize(11);
    m_pTree->setFont(fTree);
    m_pTree->setColumnCount(7);
    m_pTree->setHeaderLabels({QStringLiteral("选"), QStringLiteral("级别"),
                              QStringLiteral("文件"), QStringLiteral("行"),
                              QStringLiteral("规则"), QStringLiteral("问题"),
                              QStringLiteral("建议")});
    m_pTree->setRootIsDecorated(false);
    m_pTree->setAlternatingRowColors(true);
    m_pTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTree->header()->setSectionResizeMode(QHeaderView::Interactive);   // 2026-10-08 全列允许手工拖宽（尾列吃余量）
    m_pTree->header()->setStretchLastSection(true);
    m_pTree->header()->resizeSection(0, 40);
    m_pTree->header()->resizeSection(1, 68);
    m_pTree->header()->resizeSection(2, 240);
    m_pTree->header()->resizeSection(3, 48);
    m_pTree->header()->resizeSection(4, 88);
    m_pTree->header()->resizeSection(5, 380);
    m_pTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTree, &QTreeWidget::customContextMenuRequested,
            this, &CCodeReviewDialog::OnFindingsContextMenu);
    connect(m_pTree, &QTreeWidget::itemDoubleClicked,
            this, &CCodeReviewDialog::OnItemDoubleClicked);
    connect(m_pTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int a_iCol) {
        if (a_iCol == 0 && !m_bPopulating) {
            UpdateCounts();   // 「按建议修复」可用性随勾选变化
        }
    });
    pLayReview->addWidget(m_pTree, 1);
    m_pTabs->addTab(pPageReview, QStringLiteral("审查"));

    // —— 底部按钮（不变 + 开始文案随新流程调整）——
    auto* pLayBtn = new QHBoxLayout();
    m_pBtnStart = new QPushButton(QStringLiteral("开始审查"), this);
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
    m_pBtnStop->setEnabled(false);
    m_pBtnSave->setEnabled(false);
    m_pBtnFix->setEnabled(false);
    m_pBtnFix->setToolTip(QStringLiteral("把**勾选**的审查意见注入当前助手的提问框，交大模型修复"
                                         "（存在就修复，不存在忽略），确认后点发送"));
    m_pBtnStart->setToolTip(QStringLiteral("审查「选择文件」页勾选的文件"
                                           "（未勾「全量审查」时仅送修改部分）"));
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
    connect(m_pCboFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CCodeReviewDialog::OnFilterChanged);
    connect(m_pBtnEditFit, &QPushButton::clicked, this, &CCodeReviewDialog::OnEditFilters);

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

// ----------------------------------------------------------------------------
// —— 2026-10-08 过滤档案 ——
QList<SReviewFilter> CCodeReviewDialog::LoadFilters() const {
    QList<SReviewFilter> aOut = DefaultFilterProfiles();
#ifdef Q_OS_WIN
    const QString sIni = GlobalConfigIni();
    wchar_t wszBuf[4096] = {0};
    const DWORD dwLen = ::GetPrivateProfileStringW(L"Review", L"Filters", L"",
                                                   wszBuf, 4096,
                                                   sIni.toStdWString().c_str());
    const QString sJson = QString::fromWCharArray(wszBuf, static_cast<int>(dwLen)).trimmed();
    if (!sJson.isEmpty()) {
        const QJsonArray aArr = QJsonDocument::fromJson(sJson.toUtf8()).array();
        if (!aArr.isEmpty()) {
            aOut.clear();
            for (const auto& v : aArr) {
                const QJsonObject o = v.toObject();
                const QString sName = o.value(QStringLiteral("n")).toString().trimmed();
                const QString sExts = o.value(QStringLiteral("e")).toString().trimmed();
                if (!sName.isEmpty() && !sExts.isEmpty()) {
                    aOut.append({sName, sExts.toLower()});
                }
            }
            if (aOut.isEmpty()) {
                aOut = DefaultFilterProfiles();   // 全非法（手改坏）→ 回缺省
            }
        }
    }
#endif
    return aOut;
}

void CCodeReviewDialog::SaveFilters(const QList<SReviewFilter>& a_aFilters) {
#ifdef Q_OS_WIN
    QJsonArray aArr;
    for (const SReviewFilter& stF : a_aFilters) {
        QJsonObject o;
        o.insert(QStringLiteral("n"), stF.sName);
        o.insert(QStringLiteral("e"), stF.sExts);
        aArr.append(o);
    }
    const QString sJson = QString::fromUtf8(
        QJsonDocument(aArr).toJson(QJsonDocument::Compact));
    const std::wstring wsIni = GlobalConfigIni().toStdWString();
    ::WritePrivateProfileStringW(L"Review", L"Filters", sJson.toStdWString().c_str(),
                                 wsIni.c_str());
#endif
}

QList<QByteArray> CCodeReviewDialog::ExtsOfProfile(const SReviewFilter& a_stProfile) {
    QList<QByteArray> aOut;
    for (const QString& s : a_stProfile.sExts.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        aOut.append(s.trimmed().toLower().toUtf8());
    }
    return aOut;
}

QList<QByteArray> CCodeReviewDialog::AllFilterExts() const {
    QList<QByteArray> aAll;
    for (const SReviewFilter& stF : m_aFilters) {
        aAll.append(ExtsOfProfile(stF));
    }
    return aAll;
}

// 「全部」+ 各过滤档案项；保名：编辑前后同名项选中态保持。
void CCodeReviewDialog::FillFilterCombo() {
    const QString sKeep = m_pCboFilter->currentData().toString();
    m_pCboFilter->blockSignals(true);
    m_pCboFilter->clear();
    m_pCboFilter->addItem(QStringLiteral("全部"), QStringLiteral("*"));
    int iKeep = 0;
    for (int i = 0; i < m_aFilters.size(); ++i) {
        m_pCboFilter->addItem(m_aFilters.at(i).sName, m_aFilters.at(i).sName);
        if (sKeep == m_aFilters.at(i).sName) {
            iKeep = i + 1;
        }
    }
    m_pCboFilter->setCurrentIndex(iKeep);
    m_pCboFilter->blockSignals(false);
}

void CCodeReviewDialog::OnEditFilters() {
    CReviewFilterEditDialog dlg(m_aFilters, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    QList<SReviewFilter> aNew = dlg.Result();
    if (aNew.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("过滤条件"),
                                 QStringLiteral("未保留任何有效过滤项，维持原过滤条件不变。"));
        return;
    }
    m_aFilters = aNew;
    SaveFilters(m_aFilters);
    FillFilterCombo();
    PopulateFileList();
}

void CCodeReviewDialog::OnFilterChanged(int) {
    PopulateFileList();
}

void CCodeReviewDialog::OnFullToggled(bool) {
    PopulateFileList();
}

// ----------------------------------------------------------------------------
// —— 基线/修改判定 ——
QString CCodeReviewDialog::BaselineDirPath() const {
    return CacheDirPath() + QStringLiteral("/") + KeyOfTarget(m_sTarget)
         + QStringLiteral(".bl");
}

CCodeReviewDialog::EModState CCodeReviewDialog::ModStateOf(const QString& a_sRel,
                                                           const QFileInfo& a_stInfo) const {
    const auto it = m_mapBase.find(CReviewBaseline::NormKey(a_sRel));
    if (it == m_mapBase.end()) {
        return eModNew;
    }
    return (it.value().iMtimeMs == a_stInfo.lastModified().toMSecsSinceEpoch()
            && it.value().iSize == a_stInfo.size())
               ? eModSame
               : eModChanged;
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
        // 2026-10-08 候选口径 = 过滤档案扩展名并集 ∪ 引擎已知语言（档案被改窄时
        //   已知类型仍可见于「全部」）；目录递归跳过生成目录噪声。
        const QList<QByteArray> aExts = AllFilterExts();
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
            const QByteArray byExt = QFileInfo(sAbs).suffix().toLower().toUtf8();
            if (aExts.contains(byExt) || !pSdk->LanguageForFile(sAbs).isEmpty()) {
                m_aAbsFiles.append(sAbs);
            }
        }
    } else {
        m_sRootDir = stInfo.absolutePath();
    }
}

void CCodeReviewDialog::PopulateFileList() {
    if (m_pTreeFiles == nullptr) {
        return;
    }
    m_bPopulating = true;
    m_mapBase = CReviewBaseline::LoadMeta(BaselineDirPath());
    m_pTreeFiles->clear();

    const bool bFull = m_pChkFull->isChecked();
    // 当前过滤项扩展名（「全部」= 空表 → 全候选）。
    QList<QByteArray> aSelExts;
    const QString sSel = m_pCboFilter->currentData().toString();
    if (sSel != QStringLiteral("*") && !sSel.isEmpty()) {
        for (const SReviewFilter& stF : m_aFilters) {
            if (stF.sName == sSel) {
                aSelExts = ExtsOfProfile(stF);
                break;
            }
        }
    }
    int iShown = 0, iChanged = 0, iNew = 0, iChecked = 0;
    for (const QString& sAbs : m_aAbsFiles) {
        const QFileInfo stInfo(sAbs);
        const QString sRel = CReviewSdkClient::RelativeToRoot(m_sRootDir, sAbs);
        if (!aSelExts.isEmpty()
            && !aSelExts.contains(stInfo.suffix().toLower().toUtf8())) {
            continue;
        }
        const EModState eMod = ModStateOf(sRel, stInfo);
        if (!bFull && eMod == eModSame) {
            continue;   // 未勾全量：只列修改/新增
        }
        auto* pItem = new QTreeWidgetItem(m_pTreeFiles);
        pItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pItem->setCheckState(0, Qt::Checked);   // 缺省全勾
        pItem->setText(1, sRel);
        pItem->setTextAlignment(0, Qt::AlignCenter);
        QColor clrState(150, 150, 150);
        if (eMod == eModNew) {
            pItem->setText(2, QStringLiteral("新增"));
            clrState = QColor(30, 110, 210);
            ++iNew;
        } else if (eMod == eModChanged) {
            pItem->setText(2, QStringLiteral("修改"));
            clrState = QColor(200, 30, 30);
            ++iChanged;
        } else {
            pItem->setText(2, QStringLiteral("无变化"));
        }
        pItem->setForeground(2, clrState);
        pItem->setText(3, HumanSize(stInfo.size()));
        pItem->setText(4, stInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        pItem->setToolTip(1, CTipStyle::WrapText(QDir::toNativeSeparators(sAbs)));   // 2026-10-08 长路径同样换行
        pItem->setData(0, Qt::UserRole, sAbs);
        ++iShown;
        ++iChecked;
    }
    m_pTreeFiles->sortByColumn(1, Qt::AscendingOrder);
    m_bPopulating = false;

    QString sTip = QStringLiteral("共 %1 个，已勾选 %2").arg(
        QString::number(iShown), QString::number(iChecked));
    if (!bFull) {
        sTip += QStringLiteral("（修改 %1，新增 %2）")
                    .arg(QString::number(iChanged), QString::number(iNew));
    } else {
        sTip += QStringLiteral("（修改 %1，新增 %2，无变化 %3）")
                    .arg(QString::number(iChanged), QString::number(iNew),
                         QString::number(iShown - iChanged - iNew));
    }
    m_pLblCount->setText(sTip);
}

void CCodeReviewDialog::RefreshCountLabel() {
    if (m_pTreeFiles == nullptr || m_pLblCount == nullptr) {
        return;
    }
    int iShown = 0, iChecked = 0;
    for (int i = 0; i < m_pTreeFiles->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = m_pTreeFiles->topLevelItem(i);
        ++iShown;
        if (pItem->checkState(0) == Qt::Checked) {
            ++iChecked;
        }
    }
    m_pLblCount->setText(QStringLiteral("共 %1 个，已勾选 %2")
                             .arg(QString::number(iShown), QString::number(iChecked)));
}

// ----------------------------------------------------------------------------
// —— 右键菜单 ——
void CCodeReviewDialog::OnFilesContextMenu(const QPoint& a_pt) {
    auto* pMenu = new QMenu(this);
    QAction* pActSel = pMenu->addAction(QStringLiteral("选择"));
    QAction* pActUnsel = pMenu->addAction(QStringLiteral("取消选择"));
    pMenu->addSeparator();
    QAction* pActRun = pMenu->addAction(QStringLiteral("开始审查"));
    const QPoint ptGlobal = m_pTreeFiles->viewport()->mapToGlobal(a_pt);
    QAction* pAct = pMenu->exec(ptGlobal);
    if (pAct == nullptr) {
        return;
    }
    // 作用域：选中行（无选中=全部可见行）。
    QList<QTreeWidgetItem*> aRows = m_pTreeFiles->selectedItems();
    if (aRows.isEmpty()) {
        for (int i = 0; i < m_pTreeFiles->topLevelItemCount(); ++i) {
            aRows.append(m_pTreeFiles->topLevelItem(i));
        }
    }
    if (pAct == pActSel || pAct == pActUnsel) {
        const Qt::CheckState eState = (pAct == pActSel) ? Qt::Checked : Qt::Unchecked;
        m_bPopulating = true;
        for (QTreeWidgetItem* pItem : aRows) {
            pItem->setCheckState(0, eState);
        }
        m_bPopulating = false;
        RefreshCountLabel();
    } else if (pAct == pActRun) {
        OnStart();
    }
}

void CCodeReviewDialog::OnFindingsContextMenu(const QPoint& a_pt) {
    auto* pMenu = new QMenu(this);
    QAction* pActAll = pMenu->addAction(QStringLiteral("全选"));
    QAction* pActNone = pMenu->addAction(QStringLiteral("取消全选"));
    const QPoint ptGlobal = m_pTree->viewport()->mapToGlobal(a_pt);
    QAction* pAct = pMenu->exec(ptGlobal);
    if (pAct == nullptr) {
        return;
    }
    m_bPopulating = true;
    for (int i = 0; i < m_pTree->topLevelItemCount(); ++i) {
        m_pTree->topLevelItem(i)->setCheckState(
            0, (pAct == pActAll) ? Qt::Checked : Qt::Unchecked);
    }
    m_bPopulating = false;
    UpdateCounts();
}

// ----------------------------------------------------------------------------
// —— 审查执行 ——
void CCodeReviewDialog::OnStart() {
    if (m_pSvc != nullptr && m_pSvc->IsRunning()) {
        return;
    }
    // 勾选文件清单 + 逐文件增量裁决（未勾全量且有基线 → 只审修改部分）。
    const bool bFull = m_pChkFull->isChecked();
    QList<SReviewItem> aItems;
    for (int i = 0; i < m_pTreeFiles->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = m_pTreeFiles->topLevelItem(i);
        if (pItem->checkState(0) != Qt::Checked) {
            continue;
        }
        const QString sAbs = pItem->data(0, Qt::UserRole).toString();
        const QFileInfo stInfo(sAbs);
        const QString sRel = CReviewSdkClient::RelativeToRoot(m_sRootDir, sAbs);
        const bool bInc = !bFull && ModStateOf(sRel, stInfo) == eModChanged;
        aItems.append({sAbs, bInc});
    }
    if (aItems.isEmpty()) {
        m_pTabs->setCurrentWidget(m_pTreeFiles->parentWidget());
        QMessageBox::information(this, QStringLiteral("开始审查"),
            QStringLiteral("「选择文件」页没有勾选任何文件——请先勾选（或右键「选择」）"
                           "后再开始审查。"));
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
        connect(m_pSvc, &CReviewService::sigInfo, this, [this](const QString& a_sLine) {
            m_pLblStatus->setText(a_sLine);   // 增量/跳过/回退等逐文件说明
        });
    }

    m_bPopulating = true;
    m_pTree->clear();
    m_bPopulating = false;
    m_iDoneFiles  = 0;
    m_iFindings   = 0;
    m_sReportMd.clear();
    m_sCachedTime.clear();
    m_aCacheFiles = QJsonArray();                       // 2026-10-02 新一轮：逐文件结果重新累积
    m_aFp         = ComputeFingerprint();               // 开审前指纹（保存时比对基准）
    m_iTotalFiles = aItems.size();
    m_pProgress->setRange(0, m_iTotalFiles);
    m_pProgress->setValue(0);
    m_pBtnStart->setEnabled(false);
    m_pBtnStop->setEnabled(true);
    m_pBtnSave->setEnabled(false);
    m_pLblStatus->setText(QStringLiteral("开始审查：%1 个文件（%2）…")
                              .arg(QString::number(m_iTotalFiles),
                                   bFull ? QStringLiteral("全量")
                                         : QStringLiteral("仅修改部分")));
    UpdateCounts();
    m_pTabs->setCurrentIndex(1);   // 审查时自动切到审查页

    if (!m_pSvc->StartEx(m_sRootDir, aItems, BaselineDirPath(), 600)) {
        m_pLblStatus->setText(QStringLiteral("审查启动失败（引擎不可用或正在运行）"));
        m_pBtnStart->setEnabled(true);
        m_pBtnStop->setEnabled(false);
        return;
    }
    m_bReviewRunning = true;
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
    m_bReviewRunning = false;
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
    // 2026-10-08 基线已随逐文件成功前移：重建清单反映最新「修改/新增」状态
    //   （未勾全量 + 全部审完 → 清单归空属预期语义「自上次审查无修改」）。
    PopulateFileList();
}

void CCodeReviewDialog::AddFindings(const QString& sRelPath, const QString& sFindingsJson) {
    const QJsonArray aArr = QJsonDocument::fromJson(sFindingsJson.toUtf8()).array();
    for (const auto& v : aArr) {
        const QJsonObject oF = v.toObject();
        const QString sSev  = oF.value(QStringLiteral("severity")).toString();
        const int iLine     = oF.value(QStringLiteral("line")).toInt();
        m_bPopulating = true;
        auto* pItem = new QTreeWidgetItem(m_pTree);
        pItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pItem->setCheckState(0, Qt::Checked);   // 2026-10-08 缺省全勾
        pItem->setText(1, sSev);
        pItem->setText(2, oF.value(QStringLiteral("file")).toString(sRelPath));
        pItem->setText(3, iLine > 0 ? QString::number(iLine) : QStringLiteral("-"));
        pItem->setText(4, oF.value(QStringLiteral("rule")).toString());
        pItem->setText(5, oF.value(QStringLiteral("message")).toString());
        pItem->setText(6, oF.value(QStringLiteral("suggestion")).toString());
        // 2026-10-08 条目 tooltip 自动换行：全局 CTipFilter 只截获 QWidget::setToolTip，
        //   QTreeWidgetItem 走模型数据不经该事件 → 此处显式 WrapText（富文本自动按宽换行）。
        const QString sMsg = oF.value(QStringLiteral("message")).toString();
        const QString sSug = oF.value(QStringLiteral("suggestion")).toString();
        if (!sMsg.isEmpty()) {
            pItem->setToolTip(5, CTipStyle::WrapText(sMsg));
        }
        if (!sSug.isEmpty()) {
            pItem->setToolTip(6, CTipStyle::WrapText(sSug));
        }
        // 级别配色：high 红 / medium 橙 / low 灰。
        QColor clrSev(120, 120, 120);
        if (sSev == QStringLiteral("high")) {
            clrSev = QColor(200, 30, 30);
        } else if (sSev == QStringLiteral("medium")) {
            clrSev = QColor(210, 120, 0);
        }
        pItem->setForeground(1, clrSev);
        // 行数据：绝对路径 + 行号（双击打开定位）。
        const QString sFile = pItem->text(2);
        pItem->setData(0, Qt::UserRole,
                       QDir::isAbsolutePath(sFile)
                           ? sFile
                           : m_sRootDir + QLatin1Char('/') + sFile);
        pItem->setData(0, Qt::UserRole + 1, iLine);
        m_bPopulating = false;
    }
    m_iFindings += aArr.size();
}

void CCodeReviewDialog::UpdateCounts() {
    int iChecked = 0;
    int iTotalRows = 0;
    if (m_pTree != nullptr) {
        for (int i = 0; i < m_pTree->topLevelItemCount(); ++i) {
            ++iTotalRows;
            if (m_pTree->topLevelItem(i)->checkState(0) == Qt::Checked) {
                ++iChecked;
            }
        }
    }
    setWindowTitle(QStringLiteral("代码审查：%1（发现 %2 条，勾选 %3 条 / 送审 %4 文件）")
                       .arg(m_sTargetName,
                            QString::number(m_iFindings),
                            QString::number(iTotalRows > 0 ? iChecked : 0),
                            QString::number(m_iTotalFiles)));
    // 2026-10-08 有勾选发现才可「按建议修复」（空勾选=空提示词无意义）。
    if (m_pBtnFix != nullptr) {
        m_pBtnFix->setEnabled(iChecked > 0);
    }
}

// 2026-10-02「按建议修复」：按用户指定模板拼修复提示词（审查意见=勾选的问题清单），
//   注入当前助手提问框（确认后手动发送；同「添加到聊天框」链路，不自动起 Agent）。
void CCodeReviewDialog::OnFixToChat() {
    if (m_pTree == nullptr || m_pTree->topLevelItemCount() == 0) {
        return;
    }
    QString sList;
    for (int i = 0; i < m_pTree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = m_pTree->topLevelItem(i);
        if (pItem->checkState(0) != Qt::Checked) {
            continue;   // 2026-10-08 只带走勾选行
        }
        sList += QStringLiteral("- [%1] %2:%3（%4）%5\n")
                     .arg(pItem->text(1), pItem->text(2), pItem->text(3),
                          pItem->text(4), pItem->text(5));
        if (!pItem->text(6).isEmpty()) {
            sList += QStringLiteral("  建议：%1\n").arg(pItem->text(6));
        }
    }
    if (sList.isEmpty()) {
        return;
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
    // 指纹比对：全候选清单的 mtime+size 任一变化 → 视为代码已修改，走正常选文件流程。
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
        "在「选择文件」页重新勾选可强制重审。")
        .arg(m_sCachedTime, QString::number(m_iTotalFiles),
             QString::number(m_iFindings),
             iFailed > 0 ? QStringLiteral("，含 %1 个失败文件").arg(iFailed)
                         : QString()));
    UpdateCounts();
    m_pTabs->setCurrentIndex(1);   // 无修改可选 → 直接停在审查页看上次结果
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
