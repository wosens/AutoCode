// CFindReplaceDialog.cpp —— 编辑菜单「查找/替换」共用对话框（2026-11-07；同日
//   增强：查找串历史下拉/右键管理 + 选项持久化注入回读）。
#include "CFindReplaceDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QAbstractItemView>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "core/CFileSearcher.h"   // RegexError 预检

CFindReplaceDialog::CFindReplaceDialog(bool a_bReplaceMode,
                                       const QStringList& a_aHistory,
                                       QWidget* a_pParent)
    : QDialog(a_pParent), m_aHistory(a_aHistory) {
    setWindowTitle(a_bReplaceMode ? QStringLiteral("在文件中替换")
                                  : QStringLiteral("在文件中查找"));
    QFont stFont = font();
    stFont.setPointSize(11);   // 界面缺省 11pt（2026 规则）
    setFont(stFont);

    // —— 顶部：模式单选（调用参数决定初值，用户可手切）——
    m_pRadioFind = new QRadioButton(QStringLiteral("在文件中查找"), this);
    m_pRadioFind->setObjectName(QStringLiteral("radioFind"));   // 测试观察口
    m_pRadioReplace = new QRadioButton(QStringLiteral("在文件中替换"), this);
    m_pRadioReplace->setObjectName(QStringLiteral("radioReplace"));   // 测试观察口
    if (a_bReplaceMode) {
        m_pRadioReplace->setChecked(true);
    } else {
        m_pRadioFind->setChecked(true);
    }
    connect(m_pRadioFind, &QRadioButton::toggled, this, [this]() { OnModeToggled(); });
    connect(m_pRadioReplace, &QRadioButton::toggled, this, [this]() { OnModeToggled(); });
    auto* pRowMode = new QHBoxLayout();
    pRowMode->addWidget(m_pRadioFind);
    pRowMode->addWidget(m_pRadioReplace);
    pRowMode->addStretch(1);

    // —— 查找串（可编辑下拉：点击输入框自动列出历史）+ 替换串 ——
    m_pCmbFind = new QComboBox(this);
    m_pCmbFind->setEditable(true);
    m_pCmbFind->addItems(m_aHistory);
    m_pCmbFind->setInsertPolicy(QComboBox::NoInsert);   // 历史由主窗口统一登记
    QLineEdit* pEditFind = m_pCmbFind->lineEdit();
    pEditFind->setPlaceholderText(QStringLiteral("要查找的内容"));
    pEditFind->setClearButtonEnabled(true);
    // 需求 3：点击（获得焦点）自动列出最近查找内容（时间倒序=插入序）。
    pEditFind->installEventFilter(this);
    // 下拉列表右键：删除（当前条目）/清空（全部）。view() 惰性——首次访问即建，
    //   装事件过滤器接 ContextMenu。
    m_pCmbFind->setView(new QListView(m_pCmbFind));   // 显式建 view（默认同款）
    QAbstractItemView* pPopup = m_pCmbFind->view();
    pPopup->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pPopup, &QAbstractItemView::customContextMenuRequested,
            this, [this, pPopup](const QPoint& a_stPos) {
        if (m_pCmbFind->count() < 1) {
            return;
        }
        // 右键所在行高亮为“当前条目”（未命中行则保持现选中）。
        const QModelIndex stIdx = pPopup->indexAt(a_stPos);
        if (stIdx.isValid()) {
            pPopup->setCurrentIndex(stIdx);
        }
        QMenu stMenu(this);
        QAction* pActDel = stMenu.addAction(QStringLiteral("删除"));
        QAction* pActClr = stMenu.addAction(QStringLiteral("清空"));
        QAction* pAct = stMenu.exec(pPopup->viewport()->mapToGlobal(a_stPos));
        if (pAct == pActDel) {
            RemoveHistoryItem(pPopup->currentIndex().row());
        } else if (pAct == pActClr) {
            ClearHistory();
        }
    });
    m_pEditReplace = new QLineEdit(this);
    m_pEditReplace->setPlaceholderText(QStringLiteral("替换为（空=删除匹配内容）"));
    m_pEditReplace->setClearButtonEnabled(true);
    m_pLblReplace = new QLabel(QStringLiteral("替换为："), this);
    auto* pForm = new QFormLayout();
    pForm->setLabelAlignment(Qt::AlignRight);
    pForm->addRow(QStringLiteral("查找内容："), m_pCmbFind);
    pForm->addRow(m_pLblReplace, m_pEditReplace);

    // —— 三复选（一行横排）——
    m_pChkCase = new QCheckBox(QStringLiteral("区分大小写"), this);
    m_pChkWord = new QCheckBox(QStringLiteral("全字匹配"), this);
    m_pChkRegex = new QCheckBox(QStringLiteral("使用正则表达式"), this);
    m_pChkRegex->setToolTip(QStringLiteral(
        "按 QRegularExpression（PCRE2）语法在整篇文本上匹配，支持跨行模式"));
    auto* pRowChk = new QHBoxLayout();
    pRowChk->addWidget(m_pChkCase);
    pRowChk->addWidget(m_pChkWord);
    pRowChk->addWidget(m_pChkRegex);
    pRowChk->addStretch(1);

    // —— 查找范围（缺省「全部」）——
    m_pCmbScope = new QComboBox(this);
    m_pCmbScope->addItem(QStringLiteral("全部（含关联项目）"));
    m_pCmbScope->addItem(QStringLiteral("本项目"));
    m_pCmbScope->addItem(QStringLiteral("任务和助手日志"));
    m_pCmbScope->setCurrentIndex(0);
    m_pCmbScope->setToolTip(QStringLiteral(
        "全部 = 工程根 + 关联项目根 + 任务/助手日志；本项目 = 仅工程根；\n"
        "任务和助手日志 = 仅 .autodev 的 assistant.json/chat.json/run.log/"
        "verify_run.log（任务运行与对话、助手会话档）"));
    auto* pRowScope = new QHBoxLayout();
    pRowScope->addWidget(new QLabel(QStringLiteral("查找范围："), this));
    pRowScope->addWidget(m_pCmbScope, 1);

    // —— 按钮行：查找所有（缺省键）/ 关闭 ——
    auto* pBtnFind = new QPushButton(QStringLiteral("查找所有"), this);
    pBtnFind->setDefault(true);
    connect(pBtnFind, &QPushButton::clicked, this, &CFindReplaceDialog::OnFindAll);
    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), this);
    connect(pBtnClose, &QPushButton::clicked, this, &QDialog::reject);
    auto* pRowBtn = new QHBoxLayout();
    pRowBtn->addStretch(1);
    pRowBtn->addWidget(pBtnFind);
    pRowBtn->addWidget(pBtnClose);

    auto* pMain = new QVBoxLayout(this);
    pMain->addLayout(pRowMode);
    pMain->addLayout(pForm);
    pMain->addLayout(pRowChk);
    pMain->addLayout(pRowScope);
    pMain->addLayout(pRowBtn);

    OnModeToggled();   // 按初值刷新替换行显隐
    if (!m_aHistory.isEmpty()) {
        pEditFind->setText(m_aHistory.first());   // 最近一次预填
    }
    pEditFind->setFocus();
    resize(460, sizeHint().height());
}

bool CFindReplaceDialog::eventFilter(QObject* a_pObj, QEvent* a_pEvent) {
    // 需求 3：查找输入框获得焦点（点击）即自动弹出历史下拉（有条目才弹；
    //   已弹着不重入）。鼠标点击/Tab 聚焦均触发。
    if (a_pObj == m_pCmbFind->lineEdit()
            && a_pEvent->type() == QEvent::FocusIn
            && m_pCmbFind->count() > 0
            && !m_pCmbFind->view()->isVisible()) {
        QMetaObject::invokeMethod(this, [this]() {
            if (m_pCmbFind != nullptr && m_pCmbFind->hasFocus()
                    && m_pCmbFind->count() > 0
                    && !m_pCmbFind->view()->isVisible()) {
                m_pCmbFind->showPopup();
            }
        }, Qt::QueuedConnection);   // 让 FocusIn 完成后再弹，防吞首次点击
    }
    return QDialog::eventFilter(a_pObj, a_pEvent);
}

void CFindReplaceDialog::RemoveHistoryItem(int a_iIdx) {
    // 下拉右键「删除」：条目从 combo 与历史副本同时出列；输入框文本不动。
    if (a_iIdx < 0 || a_iIdx >= m_aHistory.size()) {
        return;
    }
    m_aHistory.removeAt(a_iIdx);
    m_pCmbFind->removeItem(a_iIdx);
}

void CFindReplaceDialog::ClearHistory() {
    m_aHistory.clear();
    m_pCmbFind->clear();
}

void CFindReplaceDialog::SetUiState(int a_iScope, bool a_bCase, bool a_bWord,
                                    bool a_bRegex) {
    if (m_pCmbScope != nullptr) {
        m_pCmbScope->setCurrentIndex(a_iScope);
    }
    if (m_pChkCase != nullptr) {
        m_pChkCase->setChecked(a_bCase);
    }
    if (m_pChkWord != nullptr) {
        m_pChkWord->setChecked(a_bWord);
    }
    if (m_pChkRegex != nullptr) {
        m_pChkRegex->setChecked(a_bRegex);
    }
}

void CFindReplaceDialog::OnModeToggled() {
    // 替换行只在「在文件中替换」模式出现（查找模式收起，界面回到纯查找形态）。
    const bool bReplace = ReplaceMode();
    m_pLblReplace->setVisible(bReplace);
    m_pEditReplace->setVisible(bReplace);
}

void CFindReplaceDialog::OnFindAll() {
    const QString sFind = FindText();
    if (sFind.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("查找"),
                             QStringLiteral("请输入要查找的内容。"));
        m_pCmbFind->lineEdit()->setFocus();
        return;
    }
    if (UseRegex()) {
        const QString sErr = CFileSearcher::RegexError(sFind, CaseSensitive());
        if (!sErr.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("查找"),
                                 QStringLiteral("正则表达式无效：\n%1").arg(sErr));
            m_pCmbFind->lineEdit()->setFocus();
            return;
        }
    }
    accept();   // 收起对话框 → 主窗口取参数开搜，「查找结果」窗口随即弹出
}

bool CFindReplaceDialog::ReplaceMode() const {
    return m_pRadioReplace != nullptr && m_pRadioReplace->isChecked();
}

QString CFindReplaceDialog::FindText() const {
    return (m_pCmbFind != nullptr && m_pCmbFind->lineEdit() != nullptr)
        ? m_pCmbFind->lineEdit()->text().trimmed() : QString();
}

QString CFindReplaceDialog::ReplaceText() const {
    return m_pEditReplace != nullptr ? m_pEditReplace->text() : QString();
}

bool CFindReplaceDialog::CaseSensitive() const {
    return m_pChkCase != nullptr && m_pChkCase->isChecked();
}

bool CFindReplaceDialog::WholeWord() const {
    return m_pChkWord != nullptr && m_pChkWord->isChecked();
}

bool CFindReplaceDialog::UseRegex() const {
    return m_pChkRegex != nullptr && m_pChkRegex->isChecked();
}

int CFindReplaceDialog::Scope() const {
    return m_pCmbScope != nullptr ? m_pCmbScope->currentIndex() : 0;
}
