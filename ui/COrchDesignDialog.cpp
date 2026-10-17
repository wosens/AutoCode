// COrchDesignDialog —— 任务编排设计窗口（见头文件说明）。
#include "COrchDesignDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "core/CFrwDevice.h"
#include "core/StageDef.h"
#include "ui/CMenuIcon.h"

// —— 小工具：读提示词文件（CFrwDevice 统一入口，UTF-8；BOM 有无均兼容）——
static QString ReadPromptFile(const QString& a_sPath) {
    CFrwDevice f(a_sPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    return ts.readAll();
}

// —— 小工具：写提示词文件（UTF-8 无 BOM——与既有 prompts/*.md 同口径；原子写）——
static bool WritePromptFile(const QString& a_sPath, const QString& a_sText) {
    QSaveFile f(a_sPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << a_sText;
    return f.commit();
}

// —— 小工具：新阶段提示词骨架模板 ——
static QString StagePromptSkeleton(const QString& a_sId, const QString& a_sName) {
    return QStringLiteral(
        "# 阶段：%1（%2）\n\n"
        "## 职责\n（描述本阶段要完成什么）\n\n"
        "## 输入\n- （上游产物文件，如 plan.md）\n\n"
        "## 输出\n- （本阶段产物文件名；无落盘产物则写「直接修改业务代码」）\n\n"
        "## 要求\n1. \n").arg(a_sName, a_sId);
}

COrchDesignDialog::COrchDesignDialog(QWidget* a_pParent)
    : QDialog(a_pParent), m_pOrch(COrchConfig::Instance()) {
    // 2026-10-14 全窗口 11 号字体（用户需求）：在 SetupUi 之前设，所有子控件
    //   （三列表/表格/编辑器/工具栏）创建时即继承；子编辑对话框/右键菜单以
    //   this 为父同样继承。表头、子对话框内表格再显式补设一道保险。
    QFont f11 = font();
    f11.setPointSize(11);
    setFont(f11);
    SetupUi();
    RefreshAll();
    // 默认选中首个阶段 → 右侧即见要求内容
    if (m_pTblStages->rowCount() > 0) {
        m_pTblStages->selectRow(0);
        OnStageSelectionChanged();
    }
}

// ============================================================================
// 界面搭建
// ============================================================================
void COrchDesignDialog::SetupUi() {
    setWindowTitle(QStringLiteral("任务编排设计"));
    setMinimumSize(1000, 640);
    resize(1180, 760);

    auto* pSplit = new QSplitter(Qt::Horizontal, this);
    pSplit->addWidget([&]() -> QWidget* {
        auto* pLeft = new QWidget(this);
        auto* pVL = new QVBoxLayout(pLeft);
        pVL->setContentsMargins(0, 0, 0, 0);
        pVL->addWidget(BuildTypeGroup(), 1);
        pVL->addWidget(BuildTierGroup(), 1);
        pVL->addWidget(BuildStageGroup(), 2);
        return pLeft;
    }());
    pSplit->addWidget(BuildEditorGroup());
    pSplit->setStretchFactor(0, 0);
    pSplit->setStretchFactor(1, 1);
    pSplit->setSizes({420, 760});

    auto* pMain = new QVBoxLayout(this);
    pMain->addWidget(pSplit);
}

QWidget* COrchDesignDialog::BuildTypeGroup() {
    auto* pGrp = new QWidget(this);
    auto* pV = new QVBoxLayout(pGrp);
    pV->setContentsMargins(4, 4, 4, 4);

    auto* pLbl = new QLabel(QStringLiteral("任务类型"), pGrp);
    pLbl->setStyleSheet(QStringLiteral("font-weight:bold;"));
    pV->addWidget(pLbl);

    m_pLstTypes = new QListWidget(pGrp);
    m_pLstTypes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pLstTypes->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        QFont f11 = m_pLstTypes->font();
        f11.setPointSize(11);
        m_pLstTypes->setFont(f11);   // 显式 11 号（不依赖继承）
    }
    m_pLstTypes->setSpacing(2);       // 行间距：避免行高紧凑致字体视觉偏小
    m_pLstTypes->setUniformItemSizes(true);   // 等高项可预算（无图标/单行文本）
    m_pLstTypes->setToolTip(QStringLiteral(
        "任务类型清单（多选可批量删除）；双击 = 编辑。\n"
        "每个类型含有序阶段流，新建任务按此显示与执行"));
    pV->addWidget(m_pLstTypes, 1);

    auto* pH = new QHBoxLayout();
    auto* pBtnAdd = new QPushButton(QStringLiteral("添加"), pGrp);
    auto* pBtnEdit = new QPushButton(QStringLiteral("编辑"), pGrp);
    auto* pBtnDel = new QPushButton(QStringLiteral("删除"), pGrp);
    pH->addWidget(pBtnAdd);
    pH->addWidget(pBtnEdit);
    pH->addWidget(pBtnDel);
    pH->addStretch(1);
    pV->addLayout(pH);

    connect(pBtnAdd, &QPushButton::clicked, this, [this]() { OnAddEntity(true); });
    connect(pBtnEdit, &QPushButton::clicked, this, [this]() { OnEditEntity(true); });
    connect(pBtnDel, &QPushButton::clicked, this, [this]() { OnDeleteEntity(true); });
    connect(m_pLstTypes, &QListWidget::itemDoubleClicked, this, [this]() { OnEditEntity(true); });
    // 右键菜单：添加/编辑/删除（同按钮）
    connect(m_pLstTypes, &QWidget::customContextMenuRequested, this, [this](const QPoint& a_stPos) {
        QMenu m(this);
        m.addAction(MenuIcon(QStringLiteral("ow-rootadd")), QStringLiteral("添加"),
                   this, [this]() { OnAddEntity(true); });
        m.addAction(MenuIcon(QStringLiteral("ow-rename")), QStringLiteral("编辑"),
                   this, [this]() { OnEditEntity(true); });
        m.addAction(MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除"),
                   this, [this]() { OnDeleteEntity(true); });
        m.exec(m_pLstTypes->mapToGlobal(a_stPos));
    });
    return pGrp;
}

QWidget* COrchDesignDialog::BuildTierGroup() {
    auto* pGrp = new QWidget(this);
    auto* pV = new QVBoxLayout(pGrp);
    pV->setContentsMargins(4, 4, 4, 4);

    auto* pLbl = new QLabel(QStringLiteral("流程档位"), pGrp);
    pLbl->setStyleSheet(QStringLiteral("font-weight:bold;"));
    pV->addWidget(pLbl);

    m_pLstTiers = new QListWidget(pGrp);
    m_pLstTiers->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pLstTiers->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        QFont f11 = m_pLstTiers->font();
        f11.setPointSize(11);
        m_pLstTiers->setFont(f11);
    }
    m_pLstTiers->setSpacing(2);
    m_pLstTiers->setUniformItemSizes(true);
    m_pLstTiers->setToolTip(QStringLiteral(
        "流程档位清单（多选可批量删除）；双击 = 编辑。\n"
        "档位决定新建任务时阶段的默认勾选集与评审策略"));
    pV->addWidget(m_pLstTiers, 1);

    auto* pH = new QHBoxLayout();
    auto* pBtnAdd = new QPushButton(QStringLiteral("添加"), pGrp);
    auto* pBtnEdit = new QPushButton(QStringLiteral("编辑"), pGrp);
    auto* pBtnDel = new QPushButton(QStringLiteral("删除"), pGrp);
    pH->addWidget(pBtnAdd);
    pH->addWidget(pBtnEdit);
    pH->addWidget(pBtnDel);
    pH->addStretch(1);
    pV->addLayout(pH);

    connect(pBtnAdd, &QPushButton::clicked, this, [this]() { OnAddEntity(false); });
    connect(pBtnEdit, &QPushButton::clicked, this, [this]() { OnEditEntity(false); });
    connect(pBtnDel, &QPushButton::clicked, this, [this]() { OnDeleteEntity(false); });
    connect(m_pLstTiers, &QListWidget::itemDoubleClicked, this, [this]() { OnEditEntity(false); });
    connect(m_pLstTiers, &QWidget::customContextMenuRequested, this, [this](const QPoint& a_stPos) {
        QMenu m(this);
        m.addAction(MenuIcon(QStringLiteral("ow-rootadd")), QStringLiteral("添加"),
                   this, [this]() { OnAddEntity(false); });
        m.addAction(MenuIcon(QStringLiteral("ow-rename")), QStringLiteral("编辑"),
                   this, [this]() { OnEditEntity(false); });
        m.addAction(MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除"),
                   this, [this]() { OnDeleteEntity(false); });
        m.exec(m_pLstTiers->mapToGlobal(a_stPos));
    });
    return pGrp;
}

QWidget* COrchDesignDialog::BuildStageGroup() {
    auto* pGrp = new QWidget(this);
    auto* pV = new QVBoxLayout(pGrp);
    pV->setContentsMargins(4, 4, 4, 4);

    auto* pLbl = new QLabel(QStringLiteral("执行阶段"), pGrp);
    pLbl->setStyleSheet(QStringLiteral("font-weight:bold;"));
    pV->addWidget(pLbl);

    m_pTblStages = new QTableWidget(0, 2, pGrp);
    {
        QFont f11 = m_pTblStages->font();
        f11.setPointSize(11);
        m_pTblStages->setFont(f11);   // 显式 11 号（不依赖继承）
    }
    // 2026-10-14 行高 28px：11 号字舒适行高（同 CVersionDialog 口径，防视觉偏小拥挤）
    m_pTblStages->verticalHeader()->setDefaultSectionSize(28);
    m_pTblStages->setHorizontalHeaderLabels({QStringLiteral("阶段名称"),
                                             QStringLiteral("缺省选择")});
    m_pTblStages->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTblStages->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTblStages->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTblStages->setContextMenuPolicy(Qt::CustomContextMenu);
    m_pTblStages->verticalHeader()->setVisible(false);
    m_pTblStages->horizontalHeader()->setStretchLastSection(false);
    m_pTblStages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pTblStages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    // 2026-10-14 表头 11 号字体（视图字体继承不覆盖 QHeaderView 默认样式的场景，显式补设）
    {
        QFont fHdr = m_pTblStages->horizontalHeader()->font();
        fHdr.setPointSize(11);
        m_pTblStages->horizontalHeader()->setFont(fHdr);
    }
    m_pTblStages->setToolTip(QStringLiteral(
        "全部执行阶段（表序即主序）；点击行 → 右侧编辑该阶段要求。\n"
        "「缺省选择」= 新建任务默认勾选；内置阶段不可删除、提示词文件名固定"));
    pV->addWidget(m_pTblStages, 1);

    auto* pH = new QHBoxLayout();
    auto* pBtnAdd = new QPushButton(QStringLiteral("添加"), pGrp);
    auto* pBtnEdit = new QPushButton(QStringLiteral("编辑"), pGrp);
    auto* pBtnDel = new QPushButton(QStringLiteral("删除"), pGrp);
    auto* pBtnDef = new QPushButton(QStringLiteral("设置为缺省"), pGrp);
    pH->addWidget(pBtnAdd);
    pH->addWidget(pBtnEdit);
    pH->addWidget(pBtnDel);
    pH->addWidget(pBtnDef);
    pH->addStretch(1);
    pV->addLayout(pH);

    connect(pBtnAdd, &QPushButton::clicked, this, &COrchDesignDialog::OnAddStage);
    connect(pBtnEdit, &QPushButton::clicked, this, &COrchDesignDialog::OnEditStage);
    connect(pBtnDel, &QPushButton::clicked, this, &COrchDesignDialog::OnDeleteStage);
    connect(pBtnDef, &QPushButton::clicked, this, &COrchDesignDialog::OnToggleStageDefault);
    connect(m_pTblStages, &QTableWidget::cellDoubleClicked, this, [this]() { OnEditStage(); });
    connect(m_pTblStages, &QTableWidget::itemSelectionChanged,
            this, &COrchDesignDialog::OnStageSelectionChanged);
    connect(m_pTblStages, &QWidget::customContextMenuRequested, this, [this](const QPoint& a_stPos) {
        QMenu m(this);
        m.addAction(MenuIcon(QStringLiteral("ow-rootadd")), QStringLiteral("添加"),
                   this, &COrchDesignDialog::OnAddStage);
        m.addAction(MenuIcon(QStringLiteral("ow-rename")), QStringLiteral("编辑"),
                   this, &COrchDesignDialog::OnEditStage);
        m.addAction(MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除"),
                   this, &COrchDesignDialog::OnDeleteStage);
        m.addSeparator();
        m.addAction(MenuIcon(QStringLiteral("ow-autoname")), QStringLiteral("设置为缺省(切换)"),
                   this, &COrchDesignDialog::OnToggleStageDefault);
        m.exec(m_pTblStages->viewport()->mapToGlobal(a_stPos));
    });
    return pGrp;
}

QWidget* COrchDesignDialog::BuildEditorGroup() {
    auto* pGrp = new QWidget(this);
    auto* pV = new QVBoxLayout(pGrp);
    pV->setContentsMargins(4, 4, 4, 4);

    // 工具栏：保存 / 查找 / 替换
    auto* pTb = new QHBoxLayout();
    m_pBtnSave = new QToolButton(pGrp);
    m_pBtnSave->setText(QStringLiteral("保存"));
    m_pBtnSave->setToolTip(QStringLiteral("保存当前阶段要求到 prompts 目录（Ctrl+S）"));
    m_pBtnFind = new QToolButton(pGrp);
    m_pBtnFind->setText(QStringLiteral("查找"));
    m_pBtnFind->setIcon(MenuIcon(QStringLiteral("ow-search")));
    m_pBtnFind->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_pBtnFind->setToolTip(QStringLiteral("打开查找栏（Ctrl+F）"));
    m_pBtnReplace = new QToolButton(pGrp);
    m_pBtnReplace->setText(QStringLiteral("替换"));
    m_pBtnReplace->setToolTip(QStringLiteral("打开替换栏（Ctrl+H）"));
    pTb->addWidget(m_pBtnSave);
    pTb->addWidget(m_pBtnFind);
    pTb->addWidget(m_pBtnReplace);
    pTb->addStretch(1);
    pV->addLayout(pTb);

    m_pLblPrompt = new QLabel(QStringLiteral("（未选择阶段）"), pGrp);
    m_pLblPrompt->setStyleSheet(QStringLiteral("color:#4b5563;"));
    pV->addWidget(m_pLblPrompt);

    m_pEdit = new QPlainTextEdit(pGrp);
    m_pEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_pEdit->setPlaceholderText(QStringLiteral(
        "点击左侧执行阶段列表中的行，编辑该阶段的要求（提示词模板）"));
    connect(m_pEdit, &QPlainTextEdit::modificationChanged, this,
            [this](bool a_b) { m_bDirty = a_b; });
    pV->addWidget(m_pEdit, 1);

    BuildFindBar();
    BuildReplaceBar();
    pV->addWidget(m_pFindBar);
    pV->addWidget(m_pRepBar);

    connect(m_pBtnSave, &QToolButton::clicked, this, [this]() { OnSaveClicked(); });
    connect(m_pBtnFind, &QToolButton::clicked, this, &COrchDesignDialog::OnFindOpen);
    connect(m_pBtnReplace, &QToolButton::clicked, this, &COrchDesignDialog::OnReplaceOpen);

    // Ctrl+S 保存 / Ctrl+F 查找
    auto* pScSave = new QShortcut(QKeySequence::Save, this);
    connect(pScSave, &QShortcut::activated, this, [this]() { OnSaveClicked(); });
    m_pScFind = new QShortcut(QKeySequence::Find, this);
    connect(m_pScFind, &QShortcut::activated, this, &COrchDesignDialog::OnFindOpen);
    return pGrp;
}

void COrchDesignDialog::BuildFindBar() {
    m_pFindBar = new QWidget(this);
    auto* pH = new QHBoxLayout(m_pFindBar);
    pH->setContentsMargins(0, 0, 0, 0);
    pH->addWidget(new QLabel(QStringLiteral("查找:"), m_pFindBar));
    m_pEdFind = new QLineEdit(m_pFindBar);
    pH->addWidget(m_pEdFind, 1);
    auto* pBtnPrev = new QPushButton(QStringLiteral("上一个"), m_pFindBar);
    auto* pBtnNext = new QPushButton(QStringLiteral("下一个"), m_pFindBar);
    m_pChkCase = new QCheckBox(QStringLiteral("区分大小写"), m_pFindBar);
    m_pChkWrap = new QCheckBox(QStringLiteral("循环"), m_pFindBar);
    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), m_pFindBar);
    pH->addWidget(pBtnPrev);
    pH->addWidget(pBtnNext);
    pH->addWidget(m_pChkCase);
    pH->addWidget(m_pChkWrap);
    pH->addWidget(pBtnClose);

    auto fnFind = [this](bool a_bBack) {
        if (!FindInEditor(a_bBack) && !m_pChkWrap->isChecked()) {
            QMessageBox::information(this, QStringLiteral("查找"),
                QStringLiteral("未找到：%1").arg(m_pEdFind->text()));
        }
    };
    connect(pBtnNext, &QPushButton::clicked, this, [fnFind]() { fnFind(false); });
    connect(pBtnPrev, &QPushButton::clicked, this, [fnFind]() { fnFind(true); });
    connect(m_pEdFind, &QLineEdit::returnPressed, this, [fnFind]() { fnFind(false); });
    connect(pBtnClose, &QPushButton::clicked, this, [this]() { m_pFindBar->hide(); });
    m_pFindBar->hide();
}

void COrchDesignDialog::BuildReplaceBar() {
    m_pRepBar = new QWidget(this);
    auto* pH = new QHBoxLayout(m_pRepBar);
    pH->setContentsMargins(0, 0, 0, 0);
    pH->addWidget(new QLabel(QStringLiteral("查找:"), m_pRepBar));
    m_pEdRepFind = new QLineEdit(m_pRepBar);
    pH->addWidget(m_pEdRepFind, 1);
    pH->addWidget(new QLabel(QStringLiteral("替换为:"), m_pRepBar));
    m_pEdRepWith = new QLineEdit(m_pRepBar);
    pH->addWidget(m_pEdRepWith, 1);
    auto* pBtnOne = new QPushButton(QStringLiteral("替换"), m_pRepBar);
    auto* pBtnAll = new QPushButton(QStringLiteral("全部替换"), m_pRepBar);
    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), m_pRepBar);
    pH->addWidget(pBtnOne);
    pH->addWidget(pBtnAll);
    pH->addWidget(pBtnClose);

    connect(pBtnOne, &QPushButton::clicked, this, &COrchDesignDialog::ReplaceOne);
    connect(pBtnAll, &QPushButton::clicked, this, &COrchDesignDialog::ReplaceAll);
    connect(pBtnClose, &QPushButton::clicked, this, [this]() { m_pRepBar->hide(); });
    m_pRepBar->hide();
}

// ============================================================================
// 列表刷新
// ============================================================================
void COrchDesignDialog::RefreshTypes() {
    const QString sCur = (m_pLstTypes->currentItem() != nullptr)
                             ? m_pLstTypes->currentItem()->data(Qt::UserRole).toString()
                             : QString();
    m_pLstTypes->blockSignals(true);
    m_pLstTypes->clear();
    int iRestore = -1;
    for (int i = 0; i < m_pOrch->aTypes.size(); ++i) {
        const SOrchType& t = m_pOrch->aTypes.at(i);
        auto* pItem = new QListWidgetItem(
            QStringLiteral("%1（%2 · %3 阶段）").arg(t.sName, t.sId)
                .arg(t.aStages.size()), m_pLstTypes);
        pItem->setFont(m_pLstTypes->font());   // 项字体显式 11 号（不随样式/委托浮动）
        pItem->setData(Qt::UserRole, t.sId);
        pItem->setToolTip(QStringLiteral("基础类型：%1\n阶段流：%2")
                              .arg(t.sBase, t.aStages.join(QStringLiteral(" → "))));
        m_pLstTypes->addItem(pItem);
        if (t.sId == sCur) {
            iRestore = i;
        }
    }
    if (iRestore >= 0) {
        m_pLstTypes->setCurrentRow(iRestore);
    }
    m_pLstTypes->blockSignals(false);
}

void COrchDesignDialog::RefreshTiers() {
    const QString sCur = (m_pLstTiers->currentItem() != nullptr)
                             ? m_pLstTiers->currentItem()->data(Qt::UserRole).toString()
                             : QString();
    m_pLstTiers->blockSignals(true);
    m_pLstTiers->clear();
    int iRestore = -1;
    for (int i = 0; i < m_pOrch->aTiers.size(); ++i) {
        const SOrchTier& t = m_pOrch->aTiers.at(i);
        auto* pItem = new QListWidgetItem(
            QStringLiteral("%1（%2）").arg(t.sName, t.sId), m_pLstTiers);
        pItem->setFont(m_pLstTiers->font());   // 项字体显式 11 号
        pItem->setData(Qt::UserRole, t.sId);
        pItem->setToolTip(QStringLiteral("基础档位：%1\n默认勾选阶段：%2")
                              .arg(t.sBase,
                                   t.aStages.isEmpty()
                                       ? QStringLiteral("（按阶段「缺省选择」）")
                                       : t.aStages.join(QStringLiteral(", "))));
        m_pLstTiers->addItem(pItem);
        if (t.sId == sCur) {
            iRestore = i;
        }
    }
    if (iRestore >= 0) {
        m_pLstTiers->setCurrentRow(iRestore);
    }
    m_pLstTiers->blockSignals(false);
}

void COrchDesignDialog::RefreshStages() {
    // 记住当前行阶段 id，刷新后尽量还原（结构操作不打断编辑位置）
    QString sCur;
    if (m_iCurStageRow >= 0 && m_iCurStageRow < m_pTblStages->rowCount()) {
        sCur = m_pTblStages->item(m_iCurStageRow, 0)->data(Qt::UserRole).toString();
    }
    m_pTblStages->blockSignals(true);
    m_pTblStages->setRowCount(0);
    int iRestore = -1;
    for (int i = 0; i < m_pOrch->aStages.size(); ++i) {
        const SOrchStage& s = m_pOrch->aStages.at(i);
        m_pTblStages->insertRow(i);
        auto* pItemName = new QTableWidgetItem(
            s.bBuiltin ? s.sName : QStringLiteral("%1 *").arg(s.sName));
        pItemName->setData(Qt::UserRole, s.sId);
        pItemName->setFont(m_pTblStages->font());   // 项字体显式 11 号
        pItemName->setToolTip(s.bBuiltin
            ? QStringLiteral("内置阶段（id：%1，提示词：%2）").arg(s.sId, s.sPromptFile)
            : QStringLiteral("自定义阶段（id：%1，提示词：%2，输出：%3）")
                  .arg(s.sId, s.sPromptFile, s.sOutput.isEmpty()
                        ? QStringLiteral("无") : s.sOutput));
        // 缺省选择列：只读复选样式（切换走按钮/右键，避免多选态下点选歧义）
        auto* pItemDef = new QTableWidgetItem();
        pItemDef->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pItemDef->setCheckState(s.bDefault ? Qt::Checked : Qt::Unchecked);
        pItemDef->setTextAlignment(Qt::AlignCenter);
        m_pTblStages->setItem(i, 0, pItemName);
        m_pTblStages->setItem(i, 1, pItemDef);
        if (s.sId == sCur) {
            iRestore = i;
        }
    }
    if (iRestore >= 0) {
        m_pTblStages->selectRow(iRestore);
        m_iCurStageRow = iRestore;
    } else {
        m_iCurStageRow = -1;   // 当前行没了（被删）：编辑器留空待选
    }
    m_pTblStages->blockSignals(false);
}

void COrchDesignDialog::RefreshAll() {
    RefreshTypes();
    RefreshTiers();
    RefreshStages();
}

// ============================================================================
// 任务类型 / 流程档位 增删改
// ============================================================================
void COrchDesignDialog::OnAddEntity(bool a_bType) {
    if (a_bType) {
        SOrchType t;
        t.sBase = QStringLiteral("feature");
        if (EditTypeDlg(t, true)) {
            m_pOrch->aTypes.append(t);
            CommitStructure();
        }
    } else {
        SOrchTier t;
        t.sBase = QStringLiteral("standard");
        if (EditTierDlg(t, true)) {
            m_pOrch->aTiers.append(t);
            CommitStructure();
        }
    }
}

void COrchDesignDialog::OnEditEntity(bool a_bType) {
    QListWidget* pLst = a_bType ? m_pLstTypes : m_pLstTiers;
    QListWidgetItem* pCur = pLst->currentItem();
    if (pCur == nullptr) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先选择要编辑的%1").arg(a_bType ? "任务类型" : "流程档位"));
        return;
    }
    const QString sId = pCur->data(Qt::UserRole).toString();
    if (a_bType) {
        SOrchType* pT = m_pOrch->FindTypeCfg(sId);
        if (pT == nullptr) {
            return;
        }
        SOrchType stCopy = *pT;   // 取消编辑不污染实例
        if (EditTypeDlg(stCopy, false)) {
            *pT = stCopy;
            CommitStructure();
        }
    } else {
        SOrchTier* pT = m_pOrch->FindTierCfg(sId);
        if (pT == nullptr) {
            return;
        }
        SOrchTier stCopy = *pT;
        if (EditTierDlg(stCopy, false)) {
            *pT = stCopy;
            CommitStructure();
        }
    }
}

void COrchDesignDialog::OnDeleteEntity(bool a_bType) {
    QListWidget* pLst = a_bType ? m_pLstTypes : m_pLstTiers;
    const QList<QListWidgetItem*> aSel = pLst->selectedItems();
    if (aSel.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先选择要删除的%1（支持多选批量删除）")
                .arg(a_bType ? "任务类型" : "流程档位"));
        return;
    }
    QStringList aIds;
    for (QListWidgetItem* pItem : aSel) {
        aIds.append(pItem->data(Qt::UserRole).toString());
    }
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
            QStringLiteral("确定删除以下%1？\n%2\n（已创建任务的编排不受影响，仅影响新建任务）")
                .arg(a_bType ? "任务类型" : "流程档位", aIds.join(QStringLiteral(", "))))
        != QMessageBox::Yes) {
        return;
    }
    for (const QString& sId : aIds) {
        if (a_bType) {
            for (int i = m_pOrch->aTypes.size() - 1; i >= 0; --i) {
                if (m_pOrch->aTypes.at(i).sId == sId) {
                    m_pOrch->aTypes.removeAt(i);
                }
            }
        } else {
            for (int i = m_pOrch->aTiers.size() - 1; i >= 0; --i) {
                if (m_pOrch->aTiers.at(i).sId == sId) {
                    m_pOrch->aTiers.removeAt(i);
                }
            }
        }
    }
    CommitStructure();
}

// ============================================================================
// 执行阶段 增删改 / 缺省切换
// ============================================================================
void COrchDesignDialog::OnAddStage() {
    SOrchStage s;
    s.bBuiltin = false;
    s.bDefault = true;
    if (EditStageDlg(s, true)) {
        // 提示词文件缺失 → 用骨架模板创建（仍存 prompts 目录）
        const QString sPath = GetPromptsDir() + QStringLiteral("/") + s.sPromptFile;
        if (!QFile::exists(sPath)) {
            WritePromptFile(sPath, StagePromptSkeleton(s.sId, s.sName));
        }
        s.iEnum = m_pOrch->NextCustomEnum();
        m_pOrch->aStages.append(s);
        CommitStructure();
    }
}

void COrchDesignDialog::OnEditStage() {
    const int iRow = m_pTblStages->currentRow();
    if (iRow < 0 || iRow >= m_pOrch->aStages.size()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先选择要编辑的执行阶段"));
        return;
    }
    SOrchStage stCopy = m_pOrch->aStages.at(iRow);
    const QString sOldPrompt = stCopy.sPromptFile;
    if (EditStageDlg(stCopy, false)) {
        m_pOrch->aStages[iRow] = stCopy;
        // 提示词文件名变了：旧文件保留，新文件缺失则用旧内容/骨架补建
        if (stCopy.sPromptFile != sOldPrompt) {
            const QString sNewPath = GetPromptsDir() + QStringLiteral("/") + stCopy.sPromptFile;
            if (!QFile::exists(sNewPath)) {
                const QString sOldPath = GetPromptsDir() + QStringLiteral("/") + sOldPrompt;
                const QString sContent = QFile::exists(sOldPath)
                    ? ReadPromptFile(sOldPath)
                    : StagePromptSkeleton(stCopy.sId, stCopy.sName);
                WritePromptFile(sNewPath, sContent);
            }
        }
        CommitStructure();
    }
}

void COrchDesignDialog::OnDeleteStage() {
    const QList<int> aRows = [this]() {
        QList<int> a;
        for (QTableWidgetItem* pItem : m_pTblStages->selectedItems()) {
            if (!a.contains(pItem->row())) {
                a.append(pItem->row());
            }
        }
        std::sort(a.begin(), a.end());
        return a;
    }();
    if (aRows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先选择要删除的执行阶段（支持多选批量删除）"));
        return;
    }
    // 内置阶段不可删除；收集可删的自定义阶段
    QStringList aIds;
    QList<int> aDelRows;
    QStringList aBuiltin;
    for (int iRow : aRows) {
        const SOrchStage& s = m_pOrch->aStages.at(iRow);
        if (s.bBuiltin) {
            aBuiltin.append(s.sName);
        } else {
            aIds.append(s.sId);
            aDelRows.append(iRow);
        }
    }
    if (aDelRows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("内置阶段不可删除：%1").arg(aBuiltin.join(QStringLiteral(", "))));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
            QStringLiteral("确定删除自定义阶段：%1？\n"
                           "（提示词文件保留在 prompts 目录；引用它的类型/档位将同步移除引用；"
                           "已创建任务的编排不受影响）%2")
                .arg(aIds.join(QStringLiteral(", ")),
                     aBuiltin.isEmpty() ? QString()
                        : QStringLiteral("\n（内置阶段已跳过：%1）")
                              .arg(aBuiltin.join(QStringLiteral(", ")))))
        != QMessageBox::Yes) {
        return;
    }
    // 倒序删行（防索引位移）
    std::sort(aDelRows.begin(), aDelRows.end(), std::greater<int>());
    for (int iRow : aDelRows) {
        m_pOrch->aStages.removeAt(iRow);
    }
    // 类型/档位引用同步清理
    for (SOrchType& t : m_pOrch->aTypes) {
        t.aStages.erase(std::remove_if(t.aStages.begin(), t.aStages.end(),
                        [&aIds](const QString& s) { return aIds.contains(s); }),
                        t.aStages.end());
    }
    for (SOrchTier& t : m_pOrch->aTiers) {
        t.aStages.erase(std::remove_if(t.aStages.begin(), t.aStages.end(),
                        [&aIds](const QString& s) { return aIds.contains(s); }),
                        t.aStages.end());
    }
    CommitStructure();
}

void COrchDesignDialog::OnToggleStageDefault() {
    const QList<int> aRows = [this]() {
        QList<int> a;
        for (QTableWidgetItem* pItem : m_pTblStages->selectedItems()) {
            if (!a.contains(pItem->row())) {
                a.append(pItem->row());
            }
        }
        return a;
    }();
    if (aRows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先选择要设置的阶段（支持多选）"));
        return;
    }
    // 切换语义：按首个选中行当前态取反（已缺省 → 全部变为不缺省）
    const bool bNew = !m_pOrch->aStages.at(aRows.first()).bDefault;
    for (int iRow : aRows) {
        m_pOrch->aStages[iRow].bDefault = bNew;
    }
    CommitStructure();
}

void COrchDesignDialog::OnStageSelectionChanged() {
    const int iRow = m_pTblStages->currentRow();
    if (iRow < 0 || iRow >= m_pOrch->aStages.size()) {
        return;
    }
    // 脏内容拦截：取消 = 回选旧行（不加载新阶段）
    if (m_bDirty && iRow != m_iCurStageRow) {
        const auto eBtn = QMessageBox::question(this, QStringLiteral("未保存的修改"),
            QStringLiteral("当前阶段要求有未保存的修改，是否保存后切换？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (eBtn == QMessageBox::Save) {
            if (!SaveStagePrompt(false)) {
                // 保存失败（文件名非法等）：留在原行
                m_pTblStages->blockSignals(true);
                if (m_iCurStageRow >= 0) {
                    m_pTblStages->selectRow(m_iCurStageRow);
                }
                m_pTblStages->blockSignals(false);
                return;
            }
        } else if (eBtn == QMessageBox::Cancel) {
            m_pTblStages->blockSignals(true);
            if (m_iCurStageRow >= 0) {
                m_pTblStages->selectRow(m_iCurStageRow);
            }
            m_pTblStages->blockSignals(false);
            return;
        }
    }
    LoadStagePrompt(iRow);
}

// ============================================================================
// 阶段要求编辑器
// ============================================================================
void COrchDesignDialog::LoadStagePrompt(int a_iRow) {
    if (a_iRow < 0 || a_iRow >= m_pOrch->aStages.size()) {
        return;
    }
    const SOrchStage& s = m_pOrch->aStages.at(a_iRow);
    m_iCurStageRow = a_iRow;
    m_sCurPromptPath = GetPromptsDir() + QStringLiteral("/") + s.sPromptFile;
    const QString sText = ReadPromptFile(m_sCurPromptPath);
    m_pEdit->blockSignals(true);
    m_pEdit->setPlainText(QFile::exists(m_sCurPromptPath)
                              ? sText
                              : QStringLiteral("# （提示词文件 %1 不存在，保存时创建）\n")
                                    .arg(s.sPromptFile));
    m_pEdit->document()->setModified(false);
    m_pEdit->blockSignals(false);
    m_bDirty = false;
    m_pLblPrompt->setText(QStringLiteral("阶段：%1　文件：prompts/%2%3")
                              .arg(s.sName, s.sPromptFile,
                                   s.bBuiltin ? QStringLiteral("　[内置]")
                                              : QString()));
}

bool COrchDesignDialog::SaveStagePrompt(bool a_bSilent) {
    if (m_iCurStageRow < 0 || m_sCurPromptPath.isEmpty()) {
        if (!a_bSilent) {
            QMessageBox::information(this, QStringLiteral("提示"),
                QStringLiteral("请先选择一个执行阶段"));
        }
        return false;
    }
    const SOrchStage& s = m_pOrch->aStages.at(m_iCurStageRow);
    // 内置阶段提示词文件名固定，但内容允许编辑（这正是编排设计的目的）
    if (!QDir().mkpath(GetPromptsDir()) ||
        !WritePromptFile(m_sCurPromptPath, m_pEdit->toPlainText())) {
        if (!a_bSilent) {
            QMessageBox::warning(this, QStringLiteral("保存失败"),
                QStringLiteral("无法写入：%1").arg(m_sCurPromptPath));
        }
        return false;
    }
    m_pEdit->document()->setModified(false);
    m_bDirty = false;
    m_pLblPrompt->setText(QStringLiteral("阶段：%1　文件：prompts/%2　[已保存 %3]")
                              .arg(s.sName, s.sPromptFile,
                                   QDateTime::currentDateTime()
                                       .toString(QStringLiteral("HH:mm:ss"))));
    return true;
}

bool COrchDesignDialog::ConfirmDiscard() {
    if (!m_bDirty) {
        return true;
    }
    const auto eBtn = QMessageBox::question(this, QStringLiteral("未保存的修改"),
        QStringLiteral("当前阶段要求有未保存的修改，是否保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (eBtn == QMessageBox::Save) {
        return SaveStagePrompt(false);
    }
    return eBtn == QMessageBox::Discard;
}

void COrchDesignDialog::OnSaveClicked() {
    SaveStagePrompt(false);   // 状态反馈在标签上（无状态栏可借）
}

void COrchDesignDialog::OnFindOpen() {
    m_pRepBar->hide();
    m_pFindBar->show();
    m_pEdFind->setFocus();
    m_pEdFind->selectAll();
}

void COrchDesignDialog::OnReplaceOpen() {
    m_pFindBar->hide();
    m_pRepBar->show();
    m_pEdRepFind->setFocus();
    m_pEdRepFind->selectAll();
}

bool COrchDesignDialog::FindInEditor(bool a_bBackward) {
    QTextDocument::FindFlags fl;
    if (a_bBackward) {
        fl |= QTextDocument::FindBackward;
    }
    if (m_pChkCase->isChecked()) {
        fl |= QTextDocument::FindCaseSensitively;
    }
    const QString sText = m_pEdFind->text();
    if (sText.isEmpty()) {
        return true;
    }
    if (m_pEdit->find(sText, fl)) {
        return true;
    }
    if (m_pChkWrap->isChecked()) {
        // 循环：回头/回尾再找一次
        if (a_bBackward) {
            QTextCursor c = m_pEdit->textCursor();
            c.movePosition(QTextCursor::End);
            m_pEdit->setTextCursor(c);
        } else {
            QTextCursor c = m_pEdit->textCursor();
            c.movePosition(QTextCursor::Start);
            m_pEdit->setTextCursor(c);
        }
        return m_pEdit->find(sText, fl);
    }
    return false;
}

void COrchDesignDialog::ReplaceOne() {
    const QString sFind = m_pEdRepFind->text();
    if (sFind.isEmpty()) {
        return;
    }
    // 先替换当前选中命中（若是查找命中），再定位下一个
    QTextCursor c = m_pEdit->textCursor();
    if (c.hasSelection() && c.selectedText() == sFind) {
        c.insertText(m_pEdRepWith->text());
    } else {
        m_pEdit->find(sFind);
    }
}

void COrchDesignDialog::ReplaceAll() {
    const QString sFind = m_pEdRepFind->text();
    if (sFind.isEmpty()) {
        return;
    }
    QString sAll = m_pEdit->toPlainText();
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;   // 与查找栏独立，简单口径
    int iCount = 0;
    // 逐段替换（QString::replace 自带计数无，手动 count）
    iCount = sAll.count(sFind, cs);
    sAll.replace(sFind, m_pEdRepWith->text(), cs);
    m_pEdit->setPlainText(sAll);
    m_pEdit->document()->setModified(true);
    m_bDirty = true;
    QMessageBox::information(this, QStringLiteral("替换完成"),
        QStringLiteral("已替换 %1 处").arg(iCount));
}

// ============================================================================
// 子对话框
// ============================================================================
bool COrchDesignDialog::EditTypeDlg(SOrchType& a_stType, bool a_bNew) {
    QDialog dlg(this);
    {
        QFont f11 = dlg.font();
        f11.setPointSize(11);
        dlg.setFont(f11);   // 子对话框同样 11 号（控件创建前设，全部继承）
    }
    dlg.setWindowTitle(a_bNew ? QStringLiteral("添加任务类型")
                              : QStringLiteral("编辑任务类型"));
    dlg.setMinimumSize(520, 560);

    auto* pEdId = new QLineEdit(a_stType.sId, &dlg);
    pEdId->setEnabled(a_bNew);
    pEdId->setPlaceholderText(QStringLiteral("英文标识，如 refactor（创建后不可改）"));
    auto* pEdName = new QLineEdit(a_stType.sName, &dlg);
    auto* pCmbBase = new QComboBox(&dlg);
    pCmbBase->addItem(QStringLiteral("feature（新功能：驳回路由走设计/计划链）"),
                      QStringLiteral("feature"));
    pCmbBase->addItem(QStringLiteral("bugfix（修缺陷：驳回路由走根因分析链）"),
                      QStringLiteral("bugfix"));
    pCmbBase->setCurrentIndex(a_stType.sBase == QStringLiteral("bugfix") ? 1 : 0);

    // 阶段表：全阶段 + 勾选包含 + 上移/下移调序（表序即执行序；Intake 强制首位包含）
    auto* pTbl = new QTableWidget(0, 2, &dlg);
    {
        QFont f11 = pTbl->font();
        f11.setPointSize(11);
        pTbl->setFont(f11);   // 显式 11 号（不依赖继承）
    }
    pTbl->verticalHeader()->setDefaultSectionSize(28);   // 舒适行高
    pTbl->setHorizontalHeaderLabels({QStringLiteral("包含"), QStringLiteral("阶段")});
    pTbl->verticalHeader()->setVisible(false);
    pTbl->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    {
        QFont fHdr = pTbl->horizontalHeader()->font();
        fHdr.setPointSize(11);
        pTbl->horizontalHeader()->setFont(fHdr);
    }
    {
        QFont fHdr = pTbl->horizontalHeader()->font();
        fHdr.setPointSize(11);
        pTbl->horizontalHeader()->setFont(fHdr);
    }
    pTbl->setToolTip(QStringLiteral("勾选该类型包含的阶段；上下箭头调整执行顺序"));
    for (int i = 0; i < m_pOrch->aStages.size(); ++i) {
        const SOrchStage& s = m_pOrch->aStages.at(i);
        pTbl->insertRow(i);
        auto* pItemChk = new QTableWidgetItem();
        pItemChk->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        pItemChk->setCheckState(a_stType.aStages.contains(s.sId) ? Qt::Checked : Qt::Unchecked);
        if (s.sId == QLatin1String("intake")) {
            pItemChk->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);   // 入口强制包含
            pItemChk->setCheckState(Qt::Checked);
            pItemChk->setToolTip(QStringLiteral("入口阶段，强制包含"));
        }
        pTbl->setItem(i, 0, pItemChk);
        auto* pItemName = new QTableWidgetItem(
            QStringLiteral("%1（%2）").arg(s.sName, s.sId));
        pItemName->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pTbl->setItem(i, 1, pItemName);
    }
    auto* pBtnUp = new QPushButton(QStringLiteral("上移"), &dlg);
    auto* pBtnDown = new QPushButton(QStringLiteral("下移"), &dlg);
    auto* pRowBtns = new QHBoxLayout();
    pRowBtns->addWidget(pBtnUp);
    pRowBtns->addWidget(pBtnDown);
    pRowBtns->addStretch(1);
    connect(pBtnUp, &QPushButton::clicked, this, [pTbl]() {
        const int i = pTbl->currentRow();
        if (i > 0) {
            pTbl->insertRow(i - 1);
            for (int c = 0; c < 2; ++c) {
                pTbl->setItem(i - 1, c, pTbl->takeItem(i + 1, c));
            }
            pTbl->removeRow(i + 1);
            pTbl->selectRow(i - 1);
        }
    });
    connect(pBtnDown, &QPushButton::clicked, this, [pTbl]() {
        const int i = pTbl->currentRow();
        if (i >= 0 && i < pTbl->rowCount() - 1) {
            pTbl->insertRow(i + 2);
            for (int c = 0; c < 2; ++c) {
                pTbl->setItem(i + 2, c, pTbl->takeItem(i, c));
            }
            pTbl->removeRow(i);
            pTbl->selectRow(i + 1);
        }
    });

    auto* pBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(pBtns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(pBtns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* pForm = new QFormLayout();
    pForm->addRow(QStringLiteral("标识(id)"), pEdId);
    pForm->addRow(QStringLiteral("显示名"), pEdName);
    pForm->addRow(QStringLiteral("基础类型"), pCmbBase);
    auto* pV = new QVBoxLayout(&dlg);
    pV->addLayout(pForm);
    pV->addWidget(new QLabel(QStringLiteral("阶段流（表序即执行顺序）："), &dlg), 0);
    pV->addWidget(pTbl, 1);
    pV->addLayout(pRowBtns);
    pV->addWidget(pBtns);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    const QString sId = pEdId->text().trimmed().toLower();
    if (sId.isEmpty() || !sId.contains(QRegularExpression(QStringLiteral("^[a-z0-9_]+$")))) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("类型 id 只能为小写字母/数字/下划线"));
        return false;
    }
    if (a_bNew && m_pOrch->FindTypeCfg(sId) != nullptr) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("类型 id 已存在：%1").arg(sId));
        return false;
    }
    a_stType.sId = sId;
    a_stType.sName = pEdName->text().trimmed().isEmpty() ? sId : pEdName->text().trimmed();
    a_stType.sBase = pCmbBase->currentData().toString();
    a_stType.aStages.clear();
    for (int i = 0; i < pTbl->rowCount(); ++i) {
        if (pTbl->item(i, 0)->checkState() == Qt::Checked) {
            const QString sStageId = m_pOrch->aStages.at(i).sId;
            if (!a_stType.aStages.contains(sStageId)) {
                a_stType.aStages.append(sStageId);
            }
        }
    }
    if (a_stType.aStages.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("至少勾选一个阶段"));
        return false;
    }
    return true;
}

bool COrchDesignDialog::EditTierDlg(SOrchTier& a_stTier, bool a_bNew) {
    QDialog dlg(this);
    {
        QFont f11 = dlg.font();
        f11.setPointSize(11);
        dlg.setFont(f11);
    }
    dlg.setWindowTitle(a_bNew ? QStringLiteral("添加流程档位")
                              : QStringLiteral("编辑流程档位"));
    dlg.setMinimumSize(520, 560);

    auto* pEdId = new QLineEdit(a_stTier.sId, &dlg);
    pEdId->setEnabled(a_bNew);
    pEdId->setPlaceholderText(QStringLiteral("英文标识，如 quick（创建后不可改）"));
    auto* pEdName = new QLineEdit(a_stTier.sName, &dlg);
    auto* pCmbBase = new QComboBox(&dlg);
    pCmbBase->addItem(QStringLiteral("simple（评审单视角/多数决，轮次最少）"),
                      QStringLiteral("simple"));
    pCmbBase->addItem(QStringLiteral("standard（评审多数决）"), QStringLiteral("standard"));
    pCmbBase->addItem(QStringLiteral("strict（评审一票否决）"), QStringLiteral("strict"));
    {
        const int iBase = pCmbBase->findData(a_stTier.sBase);
        pCmbBase->setCurrentIndex(iBase >= 0 ? iBase : 1);
    }

    auto* pLst = new QListWidget(&dlg);
    {
        QFont f11 = pLst->font();
        f11.setPointSize(11);
        pLst->setFont(f11);   // 显式 11 号（不依赖继承）
    }
    pLst->setSpacing(2);
    pLst->setToolTip(QStringLiteral(
        "勾选该档位下新建任务的默认阶段集（与任务类型阶段流取交集生效）"));
    for (int i = 0; i < m_pOrch->aStages.size(); ++i) {
        const SOrchStage& s = m_pOrch->aStages.at(i);
        auto* pItem = new QListWidgetItem(
            QStringLiteral("%1（%2）").arg(s.sName, s.sId), pLst);
        pItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        pItem->setCheckState(a_stTier.aStages.contains(s.sId) ? Qt::Checked : Qt::Unchecked);
    }

    auto* pBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(pBtns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(pBtns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* pForm = new QFormLayout();
    pForm->addRow(QStringLiteral("标识(id)"), pEdId);
    pForm->addRow(QStringLiteral("显示名"), pEdName);
    pForm->addRow(QStringLiteral("基础档位"), pCmbBase);
    auto* pV = new QVBoxLayout(&dlg);
    pV->addLayout(pForm);
    pV->addWidget(new QLabel(QStringLiteral("默认勾选阶段："), &dlg), 0);
    pV->addWidget(pLst, 1);
    pV->addWidget(pBtns);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    const QString sId = pEdId->text().trimmed().toLower();
    if (sId.isEmpty() || !sId.contains(QRegularExpression(QStringLiteral("^[a-z0-9_]+$")))) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("档位 id 只能为小写字母/数字/下划线"));
        return false;
    }
    if (a_bNew && m_pOrch->FindTierCfg(sId) != nullptr) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("档位 id 已存在：%1").arg(sId));
        return false;
    }
    a_stTier.sId = sId;
    a_stTier.sName = pEdName->text().trimmed().isEmpty() ? sId : pEdName->text().trimmed();
    a_stTier.sBase = pCmbBase->currentData().toString();
    a_stTier.aStages.clear();
    for (int i = 0; i < pLst->count(); ++i) {
        if (pLst->item(i)->checkState() == Qt::Checked) {
            a_stTier.aStages.append(m_pOrch->aStages.at(i).sId);
        }
    }
    return true;
}

bool COrchDesignDialog::EditStageDlg(SOrchStage& a_stStage, bool a_bNew) {
    QDialog dlg(this);
    {
        QFont f11 = dlg.font();
        f11.setPointSize(11);
        dlg.setFont(f11);
    }
    dlg.setWindowTitle(a_bNew ? QStringLiteral("添加执行阶段")
                              : QStringLiteral("编辑执行阶段"));
    dlg.setMinimumWidth(560);

    auto* pEdId = new QLineEdit(a_stStage.sId, &dlg);
    pEdId->setEnabled(a_bNew);
    pEdId->setPlaceholderText(QStringLiteral("英文标识，如 deploy（创建后不可改）"));
    auto* pEdName = new QLineEdit(a_stStage.sName, &dlg);
    auto* pEdPrompt = new QLineEdit(a_stStage.sPromptFile, &dlg);
    pEdPrompt->setEnabled(!a_stStage.bBuiltin);   // 内置阶段提示词文件名固定（引擎表引用）
    pEdPrompt->setPlaceholderText(QStringLiteral("prompts 目录下文件名，如 15_deploy.md"));
    if (a_bNew) {
        // 自动建议：下一个可用编号_<id>.md
        int iMax = 0;
        for (const SOrchStage& s : m_pOrch->aStages) {
            const int iNo = s.sPromptFile.left(2).toInt();
            if (iNo > iMax) {
                iMax = iNo;
            }
        }
        pEdPrompt->setText(QStringLiteral("%1_%2.md").arg(iMax + 1, 2, 10, QLatin1Char('0'))
                               .arg(pEdId->text()));
        connect(pEdId, &QLineEdit::textChanged, this, [pEdId, pEdPrompt, iMax]() {
            const QString sId = pEdId->text().trimmed().toLower();
            if (!sId.isEmpty()) {
                pEdPrompt->setText(QStringLiteral("%1_%2.md")
                                       .arg(iMax + 1, 2, 10, QLatin1Char('0')).arg(sId));
            }
        });
    }

    // 引擎字段：内置只读（静态表为准）；自定义可编辑
    auto* pEdTools = new QLineEdit(a_stStage.sTools, &dlg);
    auto* pEdInputs = new QLineEdit(a_stStage.aInputs.join(QStringLiteral(";")), &dlg);
    auto* pEdOutput = new QLineEdit(a_stStage.sOutput, &dlg);
    if (a_stStage.bBuiltin) {
        const SStageDef* pDef = FindStageById(a_stStage.sId);
        if (pDef != nullptr) {
            pEdTools->setText(pDef->sTools);
            pEdInputs->setText(pDef->aInputs.join(QStringLiteral(";")));
            pEdOutput->setText(pDef->sOutput);
        }
        for (QLineEdit* pEd : {pEdTools, pEdInputs, pEdOutput}) {
            pEd->setReadOnly(true);
        }
    }
    pEdTools->setToolTip(QStringLiteral(
        "禁用工具黑名单（逗号分隔，如 Bash,Edit；空=全功能）。内置阶段只读"));
    pEdInputs->setToolTip(QStringLiteral(
        "输入文件（相对任务私有目录，分号分隔，如 plan.md;design.md）。内置阶段只读"));
    pEdOutput->setToolTip(QStringLiteral(
        "输出产物文件名（如 deploy.md；空=直接修改业务代码）。内置阶段只读"));

    auto* pBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(pBtns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(pBtns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* pForm = new QFormLayout(&dlg);
    pForm->addRow(QStringLiteral("标识(id)"), pEdId);
    pForm->addRow(QStringLiteral("显示名"), pEdName);
    pForm->addRow(QStringLiteral("提示词文件"), pEdPrompt);
    pForm->addRow(QStringLiteral("禁用工具"), pEdTools);
    pForm->addRow(QStringLiteral("输入文件"), pEdInputs);
    pForm->addRow(QStringLiteral("输出产物"), pEdOutput);
    pForm->addWidget(pBtns);

    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    const QString sId = pEdId->text().trimmed().toLower();
    if (sId.isEmpty() || !sId.contains(QRegularExpression(QStringLiteral("^[a-z0-9_]+$")))) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("阶段 id 只能为小写字母/数字/下划线"));
        return false;
    }
    if (a_bNew && m_pOrch->FindStageCfg(sId) != nullptr) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("阶段 id 已存在：%1").arg(sId));
        return false;
    }
    QString sPrompt = pEdPrompt->text().trimmed();
    if (sPrompt.isEmpty() || sPrompt.contains(QLatin1Char('/'))
        || sPrompt.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("提示词文件名非法（仅文件名，不含路径）"));
        return false;
    }
    if (!sPrompt.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) {
        sPrompt += QStringLiteral(".md");
    }
    if (!a_stStage.bBuiltin) {
        for (const SOrchStage& s : m_pOrch->aStages) {
            if (s.sPromptFile == sPrompt && s.sId != a_stStage.sId) {
                QMessageBox::warning(this, QStringLiteral("提示"),
                    QStringLiteral("提示词文件已被子阶段使用：%1（%2）").arg(sPrompt, s.sId));
                return false;
            }
        }
    }
    a_stStage.sId = sId;
    a_stStage.sName = pEdName->text().trimmed().isEmpty() ? sId : pEdName->text().trimmed();
    a_stStage.sPromptFile = sPrompt;
    if (!a_stStage.bBuiltin) {
        a_stStage.sTools = pEdTools->text().trimmed();
        a_stStage.aInputs.clear();
        for (const QString& s : pEdInputs->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            a_stStage.aInputs.append(s.trimmed());
        }
        a_stStage.sOutput = pEdOutput->text().trimmed();
    }
    return true;
}

// ============================================================================
// 收口
// ============================================================================
void COrchDesignDialog::CommitStructure() {
    m_pOrch->Save();
    m_pOrch->ApplyToRuntime();   // 自定义阶段增删改后即时注册（FindStage 立即可见）
    RefreshAll();
}

void COrchDesignDialog::closeEvent(QCloseEvent* a_pEvent) {
    if (ConfirmDiscard()) {
        a_pEvent->accept();
    } else {
        a_pEvent->ignore();
    }
}
