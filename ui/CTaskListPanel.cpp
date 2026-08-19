#include "CTaskListPanel.h"

#include <QTabWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFont>
#include <QMenu>
#include <QAction>
#include <QTimer>

#include "core/CTaskInfo.h"
#include "core/StageDef.h"
#include "TaskTypes.h"
#include "CProjectExplorerPanel.h"

QString CTaskListPanel::FormatCost(double a_dCost) {
    // 2026.08.18 费用 ≤0 显示 "-"（缺失值风格；DSH 自 v1.0.38 起按单价折算可 >0，
    //   计费单价全 0 或无数据时仍为 "-"）。
    return a_dCost > 0.0 ? QString::number(a_dCost, 'f', 2) : QStringLiteral("-");
}

QString CTaskListPanel::FormatTokens(qint64 a_iTokens) {
    // 与 CExecutionPanel::FormatTokens 同口径（1000 进制，非 1024）。
    const double dK = 1000.0;
    if (a_iTokens < 1000) {
        return QString::number(a_iTokens);
    }
    if (a_iTokens < 1000000) {
        return QStringLiteral("%1K").arg(QString::number(a_iTokens / dK, 'f', 1));
    }
    if (a_iTokens < 1000000000LL) {
        return QStringLiteral("%1M").arg(QString::number(a_iTokens / (dK * dK), 'f', 1));
    }
    return QStringLiteral("%1G").arg(QString::number(a_iTokens / (dK * dK * dK), 'f', 2));
}

// 2026.08.15 运行耗时格式化：<1h 为 mm:ss，≥1h 为 h:mm:ss（与 CExecutionPanel 同口径）。
static QString FormatElapsed(qint64 a_iMs) {
    const qint64 iTotalSec = a_iMs / 1000;
    const qint64 iH = iTotalSec / 3600;
    const qint64 iM = (iTotalSec % 3600) / 60;
    const qint64 iS = iTotalSec % 60;
    if (iH > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(iH).arg(iM, 2, 10, QLatin1Char('0')).arg(iS, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(iM, 2, 10, QLatin1Char('0'))
                                  .arg(iS, 2, 10, QLatin1Char('0'));
}

// 当前表行文本：名称 [类型] · 阶段 · 状态 [· 耗时] · 创建时间（运行过才带耗时段，待执行不带噪音）。
QString CTaskListPanel::MakeTaskText(const CTaskInfo* a_pTask) {
    const SStageDef* pDef = FindStage(a_pTask->CurrentStage());
    const QString sDate = a_pTask->CreatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QString sElapsed =
        (a_pTask->Status() == ETaskStatus::Running || a_pTask->RunElapsedMs() > 0)
            ? QStringLiteral(" · 耗时 %1").arg(FormatElapsed(a_pTask->RunElapsedMs()))
            : QString();
    return QStringLiteral("%1 [%2] · %3 · %4%5 · %6")
        .arg(a_pTask->Name())
        .arg(ToString(a_pTask->Type()))
        .arg(pDef ? pDef->sNameZh : QStringLiteral("?"))
        .arg(ToString(a_pTask->Status()))
        .arg(sElapsed)
        .arg(sDate);
}

CTaskListPanel::CTaskListPanel(QWidget* a_pParent)
    : QWidget(a_pParent) {
    // 统一 11pt 字体（#7）：两表、表头、按钮显式设点数。
    auto setPt11 = [](QWidget* a_pW) {
        QFont f = a_pW->font();
        f.setPointSize(11);
        a_pW->setFont(f);
    };

    auto* pV = new QVBoxLayout(this);
    // 左栏紧贴右侧分隔条：清零朝向分隔条的右边距（其余三边保留默认留白），
    // 使「当前任务」内容与右栏「阶段」之间只剩分隔条本身（5px），消除过大空隙（#4）。
    {
        QMargins mg = pV->contentsMargins();
        mg.setRight(0);
        pV->setContentsMargins(mg);
    }

    // FR-3：顶部菜单条容器（第一段），供 CMainWindow::CreateMenus 注入「工程(&P)」「功能(&F)」。
    m_pMenuLayout = new QHBoxLayout();
    m_pMenuLayout->setContentsMargins(0, 0, 0, 0);
    m_pMenuLayout->setSpacing(0);
    pV->addLayout(m_pMenuLayout);

    // Tab 页：当前任务 / 历史任务
    m_pTab = new QTabWidget(this);
    m_pListCur = new QListWidget(this);
    setPt11(m_pListCur);
    m_pListCur->viewport()->installEventFilter(this);   // 粘性选择（#5）

    // 历史任务：多列 QTreeWidget（任务名称|类型|阶段|状态|费用|输入|输出|请求|耗时|创建时间）
    m_pTreeHis = new QTreeWidget(this);
    m_pTreeHis->setColumnCount(10);
    m_pTreeHis->setHeaderLabels(QStringList()
        << QStringLiteral("任务名称")
        << QStringLiteral("类型")
        << QStringLiteral("阶段")
        << QStringLiteral("状态")
        << QStringLiteral("费用")
        << QStringLiteral("输入")
        << QStringLiteral("输出")
        << QStringLiteral("请求")
        << QStringLiteral("耗时")
        << QStringLiteral("创建时间"));
    m_pTreeHis->setRootIsDecorated(false);          // 无父节点层级，去掉展开箭头
    m_pTreeHis->setAlternatingRowColors(true);      // 隔行底色，多列可读性
    m_pTreeHis->setSortingEnabled(false);           // 数据已按创建时间倒序，关闭自动排序
    // 列宽：全列可手动拖动分隔线调节（Interactive）；末列（创建时间）拉伸填满剩余避免右侧空白。
    // 默认列宽按经验设，用户拖动后本会话内由 header 自动保持；名称列 tooltip 兜底显示不全。
    QHeaderView* pHdr = m_pTreeHis->header();
    pHdr->setSectionsMovable(false);                       // 禁止拖动换列顺序，仅允许调宽
    pHdr->setSectionResizeMode(QHeaderView::Interactive);  // 全列可拖分隔线调宽
    pHdr->setStretchLastSection(true);                     // 末列拉伸填满剩余空间
    pHdr->resizeSection(0, 220);   // 任务名称
    pHdr->resizeSection(1, 60);    // 类型
    pHdr->resizeSection(2, 70);    // 阶段
    pHdr->resizeSection(3, 60);    // 状态
    pHdr->resizeSection(4, 70);    // 费用
    pHdr->resizeSection(5, 70);    // 输入
    pHdr->resizeSection(6, 70);    // 输出
    pHdr->resizeSection(7, 50);    // 请求
    pHdr->resizeSection(8, 60);    // 耗时
    setPt11(m_pTreeHis);
    setPt11(m_pTreeHis->header());
    m_pTreeHis->viewport()->installEventFilter(this);   // 粘性选择（#5）
    // 数值列右对齐在逐条 QTreeWidgetItem::setTextAlignment 设置（Qt6 无列级对齐接口）

    m_pTab->addTab(m_pListCur, QStringLiteral("当前任务"));
    m_pTab->addTab(m_pTreeHis, QStringLiteral("历史任务"));
    // 2026.08.18 拆掉 FR-2 的 setMaximumHeight 限高（≈5 条）：列表高度改由下方垂直
    //   分隔条控制，拖大上段即可看更多任务（任务多时的核心诉求）。

    auto* pH = new QHBoxLayout();
    m_pBtnNew   = new QPushButton(QStringLiteral("新建"), this);
    m_pBtnStart = new QPushButton(QStringLiteral("启动/继续"), this);
    m_pBtnPause = new QPushButton(QStringLiteral("暂停"), this);
    m_pBtnStop  = new QPushButton(QStringLiteral("停止"), this);
    m_pBtnRollback = new QPushButton(QStringLiteral("回退"), this);   // FR-6
    m_pBtnRollback->setToolTip(QStringLiteral("回退到历史源码版本（改码前自动备份）"));
    for (QPushButton* pBtn : {m_pBtnNew, m_pBtnStart, m_pBtnPause, m_pBtnStop, m_pBtnRollback}) {
        setPt11(pBtn);
    }
    pH->addWidget(m_pBtnNew);
    pH->addWidget(m_pBtnStart);
    pH->addWidget(m_pBtnPause);
    pH->addWidget(m_pBtnStop);
    pH->addWidget(m_pBtnRollback);

    // 2026.08.18 任务列表高度分隔条：按钮行与资源管理器之间的垂直 QSplitter。
    //   上段 = 任务 Tab（拿 stretch，拖大即看更多任务）+ 按钮行；下段 = 资源管理器。
    //   位置由 CMainWindow 经 saveState/restoreState 持久化（[Layout] VSplit）。
    m_pVSplit = new QSplitter(Qt::Vertical, this);
    m_pVSplit->setHandleWidth(5);              // 与主窗口水平分隔条同宽
    m_pVSplit->setChildrenCollapsible(false);  // 两段都不允许拖没，保底可见
    auto* pTop = new QWidget(m_pVSplit);
    auto* pVT = new QVBoxLayout(pTop);
    pVT->setContentsMargins(0, 0, 0, 0);
    pVT->addWidget(m_pTab, 1);   // 上段内列表占满剩余（超出可滚）
    pVT->addLayout(pH);
    m_pVSplit->addWidget(pTop);
    m_pExplorer = new CProjectExplorerPanel(m_pVSplit);
    // 转发「添加到聊天框」信号给主窗口（注入 adhoc 助手输入框）。
    connect(m_pExplorer, &CProjectExplorerPanel::sigAddToChat,
            this, &CTaskListPanel::sigAddToChat);
    // 转发「双击文本文件」信号给主窗口（执行面板内嵌 Tab 显示文件内容）。
    connect(m_pExplorer, &CProjectExplorerPanel::sigFileOpen,
            this, &CTaskListPanel::sigFileOpen);
    m_pVSplit->addWidget(m_pExplorer);
    // 初始默认上段高度 ≈ 旧限高（5 行 Tab + 按钮行），与改造前观感一致；
    //   精确值不关键，restoreState 命中即被覆盖，setSizes 只定首次布局。
    {
        const int iRowH = m_pListCur->sizeHintForRow(0) > 0
                              ? m_pListCur->sizeHintForRow(0)
                              : m_pListCur->fontMetrics().height() + 4;
        const int iBtnRow = m_pBtnNew->sizeHint().height();
        m_pVSplit->setSizes({iRowH * 5 + 40 + iBtnRow, 400});
    }
    pV->addWidget(m_pVSplit, 1);
    // 2026.08.19 本分隔条拖动不再上报底部 y：此前 splitterMoved → sigTaskListBottom →
    //   右栏 SyncStagesHeight setSizes，拖左栏会连带拖动右栏「阶段/日志」分隔条（用户
    //   要求各拖动条互不影响）。FR-9 对齐只在窗口 resize（resizeEvent）路径保留。

    // 2026.08.15 运行耗时秒级刷新：当前任务 Running 时每秒重写当前表对应行文本
    //   （只 setText 不重建、不动选中；无任务运行时空转开销可忽略）。
    m_pElapsedTimer = new QTimer(this);
    connect(m_pElapsedTimer, &QTimer::timeout, this, &CTaskListPanel::OnElapsedTimer);
    m_pElapsedTimer->start(1000);

    // 信号到信号转发的简洁写法
    connect(m_pBtnNew, &QPushButton::clicked, this, &CTaskListPanel::sigNewTask);
    connect(m_pBtnStart, &QPushButton::clicked, this, [this]() {
        if (m_pCur) { emit sigStartTask(m_pCur); }
    });
    connect(m_pBtnPause, &QPushButton::clicked, this, [this]() {
        if (m_pCur) { emit sigPauseTask(m_pCur); }
    });
    connect(m_pBtnStop, &QPushButton::clicked, this, [this]() {
        if (m_pCur) { emit sigStopTask(m_pCur); }
    });
    connect(m_pBtnRollback, &QPushButton::clicked, this, [this]() {
        if (m_pCur) { emit sigRollback(m_pCur); }   // FR-6
    });
    // #3 当前表右键「移除」：把任务从当前栏移到历史（保留记录，可再次执行）
    m_pListCur->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pListCur, &QListWidget::customContextMenuRequested,
            this, &CTaskListPanel::OnCurrentContextMenu);
    // #6 历史表右键「再次执行」（原「继续执行」按钮已移除）：所指任务回到当前栏，从当前阶段续跑
    m_pTreeHis->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTreeHis, &QTreeWidget::customContextMenuRequested,
            this, &CTaskListPanel::OnHistoryContextMenu);
    connect(m_pListCur, &QListWidget::currentItemChanged,
            this, &CTaskListPanel::OnSelectionChanged);
    connect(m_pTreeHis, &QTreeWidget::currentItemChanged,
            this, &CTaskListPanel::OnHistorySelectionChanged);

    UpdateButtons();   // 初始无选中：禁用启动/暂停/停止/删除
}

void CTaskListPanel::resizeEvent(QResizeEvent* a_pEvent) {
    QWidget::resizeEvent(a_pEvent);
    ReportTaskListBottom();
}

void CTaskListPanel::ReportTaskListBottom() {
    // FR-9：上报任务列表（Tab）底部相对本 widget 顶部的 y，供执行面板同步阶段表高度对齐。
    // m_pTab 经布局后 geometry() 方准确；首帧 height()<=0 时跳过，待下次 resize 再报。
    if (m_pTab != nullptr && m_pTab->height() > 0) {
        emit sigTaskListBottom(m_pTab->geometry().bottom() + 1);
    }
}

QByteArray CTaskListPanel::VSplitState() const {
    return m_pVSplit != nullptr ? m_pVSplit->saveState() : QByteArray();
}

void CTaskListPanel::RestoreVSplitState(const QByteArray& a_stState) {
    // 空状态（首跑无记录）跳过，保留 ctor 里 setSizes 的默认比例。
    if (m_pVSplit != nullptr && !a_stState.isEmpty()) {
        m_pVSplit->restoreState(a_stState);
    }
    ReportTaskListBottom();
}

CTaskInfo* CTaskListPanel::TaskOf(QListWidgetItem* a_pItem) {
    if (a_pItem == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<CTaskInfo*>(a_pItem->data(Qt::UserRole).value<quintptr>());
}

CTaskInfo* CTaskListPanel::TaskOf(QTreeWidgetItem* a_pItem) {
    if (a_pItem == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<CTaskInfo*>(a_pItem->data(0, Qt::UserRole).value<quintptr>());
}

void CTaskListPanel::PopulateCurrent() {
    // 当前表只放本工程的非终态任务（终态任务进历史表，由 DB 驱动）。
    // 注意：clear() 会触发 currentItemChanged(nullptr) → OnSelectionChanged 把 m_pCur
    // 置空并 emit sigTaskSelected(nullptr)，从而清空执行面板日志（阶段切换时复现）。
    // 因此先保存 m_pCur，用 QSignalBlocker 屏蔽整个 clear+重建期间的选中信号，再用
    // 保存的指针恢复选中（不依赖可能已被信号清空的 m_pCur）。
    ReconcileCur();   // 2026.08.17 重建前先对账：任务已 delete（不在两表数据源）则清 m_pCur
    CTaskInfo* pPrevCur = m_pCur;
    {
        QSignalBlocker blk(m_pListCur);
        m_pListCur->clear();
        for (CTaskInfo* pTask : m_aTasks) {
            if (pTask->Status() == ETaskStatus::Done ||
                pTask->Status() == ETaskStatus::Failed) {
                continue;
            }
            const QString sText = MakeTaskText(pTask);
            auto* pItem = new QListWidgetItem(sText, m_pListCur);
            pItem->setData(Qt::UserRole, reinterpret_cast<quintptr>(pTask));
            pItem->setToolTip(sText);   // 行文本显示不全时 tooltip 兜底
        }
    }
    RestoreSelection(pPrevCur);
    UpdateButtons();
}

void CTaskListPanel::RestoreSelection(CTaskInfo* a_pTarget) {
    if (a_pTarget == nullptr) {
        return;
    }
    // 当前表找回（屏蔽信号：避免再次扰动 m_pCur / 触发面板刷新）
    for (int i = 0; i < m_pListCur->count(); ++i) {
        if (TaskOf(m_pListCur->item(i)) == a_pTarget) {
            QSignalBlocker blk(m_pListCur);
            m_pListCur->setCurrentRow(i);
            return;
        }
    }
    // 历史表（tree）找回
    for (int i = 0; i < m_pTreeHis->topLevelItemCount(); ++i) {
        if (TaskOf(m_pTreeHis->topLevelItem(i)) == a_pTarget) {
            QSignalBlocker blk(m_pTreeHis);
            m_pTreeHis->setCurrentItem(m_pTreeHis->topLevelItem(i));
            return;
        }
    }
}

void CTaskListPanel::SelectTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr) {
        return;
    }
    // 切到「当前任务」页并选中该行（正常发信号 → 触发 OnTaskSelected 显示其日志）。
    // 启动任务时调用，使任务日志无需用户手动点选即可显示。
    m_pTab->setCurrentWidget(m_pListCur);
    for (int i = 0; i < m_pListCur->count(); ++i) {
        if (TaskOf(m_pListCur->item(i)) == a_pTask) {
            m_pListCur->setCurrentRow(i);   // 不屏蔽信号：需要触发选中→显示日志
            return;
        }
    }
}

void CTaskListPanel::SetProjectRoot(const QString& a_sDir) {
    // FR-3：转发给资源管理器刷新文件树根（空串=清空树）。
    if (m_pExplorer != nullptr) {
        m_pExplorer->SetRoot(a_sDir);
    }
}

void CTaskListPanel::UpdateButtons() {
    // 按当前选中任务的状态映射按钮文案/可用性，让用户一眼看出是否已开始。
    const ETaskStatus eStatus = m_pCur ? m_pCur->Status() : ETaskStatus::Pending;
    const bool bRunning = (eStatus == ETaskStatus::Running);

    if (m_pCur == nullptr) {
        m_pBtnStart->setText(QStringLiteral("启动/继续"));
        m_pBtnStart->setEnabled(false);
        m_pBtnPause->setEnabled(false);
        m_pBtnStop->setEnabled(false);
        m_pBtnRollback->setEnabled(false);   // FR-6 无当前任务：禁用回退
        return;
    }

    // 启动按钮：文案随状态变化，运行中禁用并标注「运行中…」
    switch (eStatus) {
        case ETaskStatus::Pending:
            m_pBtnStart->setText(QStringLiteral("启动"));
            m_pBtnStart->setEnabled(true);
            break;
        case ETaskStatus::Paused:
        case ETaskStatus::Stopped:
            m_pBtnStart->setText(QStringLiteral("继续"));
            m_pBtnStart->setEnabled(true);
            break;
        case ETaskStatus::Running:
            m_pBtnStart->setText(QStringLiteral("运行中…"));
            m_pBtnStart->setEnabled(false);
            break;
        default: // Done/Failed（一般不在当前表，兜底）
            m_pBtnStart->setText(QStringLiteral("启动/继续"));
            m_pBtnStart->setEnabled(false);
            break;
    }

    m_pBtnPause->setEnabled(bRunning);
    m_pBtnStop->setEnabled(bRunning ||
                           eStatus == ETaskStatus::Paused ||
                           eStatus == ETaskStatus::Stopped);
    // FR-6：有当前任务且非运行中时可回退（运行中应先停止，避免恢复与运行冲突）。
    m_pBtnRollback->setEnabled(!bRunning);
}

void CTaskListPanel::SetTasks(const QList<CTaskInfo*>& a_aTasks) {
    m_aTasks = a_aTasks;
    PopulateCurrent();
}

void CTaskListPanel::ClearAll() {
    // 2026.08.15 切工程：两表清空 + 选中复位（屏蔽 clear 触发的选中信号，防误清执行面板外的状态）。
    // 数据源 m_aTasks 由 CMainWindow::ClearTasks 随后整体 delete，此处同步置空防悬垂。
    {
        QSignalBlocker blkList(m_pListCur);
        QSignalBlocker blkTree(m_pTreeHis);
        m_pListCur->clear();
        m_pTreeHis->clear();
    }
    m_aTasks.clear();
    m_aHis.clear();   // 2026.08.17 历史源副本一并清
    m_pCur = nullptr;
    UpdateButtons();
}

void CTaskListPanel::ReconcileCur() {
    // 2026.08.17 悬垂对账：任务对象由 CMainWindow 统一 delete（终态回收 / ReloadHistory
    //   整批重建历史副本），删除后两表 Set* 重建是唯一的同步通知点。m_pCur 不在新数据源
    //   里即已悬垂——立即置空，防止 1s 耗时定时器（OnElapsedTimer 读 m_pCur->Status()）和
    //   UpdateButtons 解引用已释放内存（d7f99cac 01:32:53 崩溃，0xc0000005）。
    if (m_pCur != nullptr && !m_aTasks.contains(m_pCur) && !m_aHis.contains(m_pCur)) {
        m_pCur = nullptr;
        UpdateButtons();
    }
}

void CTaskListPanel::SetHistoryTasks(const QList<CTaskInfo*>& a_aTasks) {
    // 2026.08.17 记录历史表数据源副本（悬垂对账用），重建后对账清 m_pCur
    //   （ReloadHistory 会整批 delete 旧历史副本，选中项若是旧副本即成悬垂）。
    m_aHis = a_aTasks;
    ReconcileCur();
    // 屏蔽 clear+重建期间的 currentItemChanged(nullptr)，避免误清 m_pCur（与 PopulateCurrent 对称）。
    QSignalBlocker blkTree(m_pTreeHis);
    m_pTreeHis->clear();
    for (CTaskInfo* pTask : a_aTasks) {
        // 过滤已由 CMainWindow::ReloadHistory 完成（终态 + 被移除归档），此处直接展示传入列表。
        const SStageDef* pDef = FindStage(pTask->CurrentStage());
        const QString sDate = pTask->CreatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        auto* pItem = new QTreeWidgetItem();
        pItem->setText(0, pTask->Name());
        pItem->setText(1, ToString(pTask->Type()));
        pItem->setText(2, pDef ? pDef->sNameZh : QStringLiteral("?"));
        pItem->setText(3, ToString(pTask->Status()));
        pItem->setText(4, FormatCost(pTask->TotalCost()));
        pItem->setText(5, FormatTokens(pTask->TotalInTokens()));
        pItem->setText(6, FormatTokens(pTask->TotalOutTokens()));
        pItem->setText(7, pTask->RequestCount() > 0
            ? QString::number(pTask->RequestCount()) : QStringLiteral("-"));
        pItem->setText(8, pTask->RunElapsedMs() > 0
            ? FormatElapsed(pTask->RunElapsedMs()) : QStringLiteral("-"));
        pItem->setText(9, sDate);
        // 数值列右对齐
        pItem->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(6, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(7, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setTextAlignment(8, Qt::AlignRight | Qt::AlignVCenter);
        pItem->setData(0, Qt::UserRole, reinterpret_cast<quintptr>(pTask));
        pItem->setToolTip(0, pTask->Name());   // 名称显示不全时 tooltip 兜底
        m_pTreeHis->addTopLevelItem(pItem);   // 调用方已按创建时间倒序，保持原序
    }
    RestoreSelection(m_pCur);
}

void CTaskListPanel::RefreshTask(CTaskInfo* /*a_pTask*/) {
    // 状态可能变化（如 Running → Done：由当前表迁出），重建当前表；
    // 历史表由 CMainWindow 在任务终结/刷新历史时调 SetHistoryTasks 更新。
    PopulateCurrent();
}

void CTaskListPanel::UpdateTaskRow(CTaskInfo* a_pTask) {
    // 2026.08.15 轻量数值刷新：只 setText 匹配行，不重建、不动选中（避免触发选中连锁）。
    if (a_pTask == nullptr) {
        return;
    }
    // 当前表：整行文本（含耗时段）
    for (int i = 0; i < m_pListCur->count(); ++i) {
        QListWidgetItem* pItem = m_pListCur->item(i);
        if (TaskOf(pItem) == a_pTask) {
            const QString sText = MakeTaskText(a_pTask);
            pItem->setText(sText);
            pItem->setToolTip(sText);
            break;
        }
    }
    // 历史表：费用/输入/输出/请求/耗时列（与 SetHistoryTasks 列定义一致）
    const int iCount = m_pTreeHis->topLevelItemCount();
    for (int i = 0; i < iCount; ++i) {
        QTreeWidgetItem* pItem = m_pTreeHis->topLevelItem(i);
        if (TaskOf(pItem) == a_pTask) {
            pItem->setText(4, FormatCost(a_pTask->TotalCost()));
            pItem->setText(5, FormatTokens(a_pTask->TotalInTokens()));
            pItem->setText(6, FormatTokens(a_pTask->TotalOutTokens()));
            pItem->setText(7, a_pTask->RequestCount() > 0
                ? QString::number(a_pTask->RequestCount()) : QStringLiteral("-"));
            pItem->setText(8, a_pTask->RunElapsedMs() > 0
                ? FormatElapsed(a_pTask->RunElapsedMs()) : QStringLiteral("-"));
            break;
        }
    }
}

void CTaskListPanel::OnElapsedTimer() {
    // 2026.08.15 当前任务运行中 → 每秒重写当前表对应行文本（只 setText，不动选中/不重建）。
    // 2026.08.17 双保险：m_pCur 必须仍在当前表数据源里才解引用（运行中任务必属 m_aTasks；
    //   历史/已回收任务即便指针漏清也不至于踩已释放内存——此为 01:32:53 崩溃点）。
    if (m_pCur == nullptr || !m_aTasks.contains(m_pCur)
        || m_pCur->Status() != ETaskStatus::Running) {
        return;
    }
    for (int i = 0; i < m_pListCur->count(); ++i) {
        QListWidgetItem* pItem = m_pListCur->item(i);
        if (TaskOf(pItem) == m_pCur) {
            const QString sText = MakeTaskText(m_pCur);
            pItem->setText(sText);
            pItem->setToolTip(sText);
            return;
        }
    }
}

void CTaskListPanel::OnSelectionChanged(QListWidgetItem* a_pCur, QListWidgetItem* /*a_pPrev*/) {
    if (m_bSyncing) {
        return;   // 互斥同步中（清历史表触发的本槽），忽略以免互踩
    }
    CTaskInfo* pTask = TaskOf(a_pCur);
    if (pTask == nullptr) {
        // 粘性选择（#5）：仅当用户点了空白（m_bAllowDeselect）才取消；否则保留原选中。
        // （程序性清空/重建泄漏的 nullptr 不应丢失选中，否则连锁清空执行面板日志。）
        if (!m_bAllowDeselect && m_pCur != nullptr) {
            RestoreSelection(m_pCur);   // 恢复视觉选中
            m_bAllowDeselect = false;
            return;
        }
        m_bAllowDeselect = false;
        m_pCur = nullptr;
        emit sigTaskSelected(nullptr);
        UpdateButtons();
        return;
    }
    m_bAllowDeselect = false;
    m_pCur = pTask;
    // 互斥（#1）：选中当前表任务时，程序性清除历史表选中（设 m_bSyncing 防其槽反应）。
    if (m_pTreeHis != nullptr && m_pTreeHis->currentItem() != nullptr) {
        m_bSyncing = true;
        m_pTreeHis->setCurrentItem(nullptr);
        m_pTreeHis->clearSelection();
        m_bSyncing = false;
    }
    emit sigTaskSelected(m_pCur);
    UpdateButtons();
}

void CTaskListPanel::OnHistorySelectionChanged(QTreeWidgetItem* a_pCur, QTreeWidgetItem* /*a_pPrev*/) {
    if (m_bSyncing) {
        return;   // 互斥同步中（清当前表触发的本槽），忽略
    }
    CTaskInfo* pTask = TaskOf(a_pCur);
    if (pTask == nullptr) {
        // 粘性选择（#5）：同当前表，仅空白点击才允许取消。
        if (!m_bAllowDeselect && m_pCur != nullptr) {
            RestoreSelection(m_pCur);
            m_bAllowDeselect = false;
            return;
        }
        m_bAllowDeselect = false;
        m_pCur = nullptr;
        emit sigTaskSelected(nullptr);
        UpdateButtons();
        return;
    }
    m_bAllowDeselect = false;
    m_pCur = pTask;
    // 互斥（#1）：选中历史表任务时，程序性清除当前表选中（设 m_bSyncing 防其槽反应）。
    if (m_pListCur != nullptr && m_pListCur->currentItem() != nullptr) {
        m_bSyncing = true;
        m_pListCur->setCurrentItem(nullptr);
        m_pListCur->clearSelection();
        m_bSyncing = false;
    }
    emit sigTaskSelected(m_pCur);
    UpdateButtons();
}

bool CTaskListPanel::eventFilter(QObject* a_pWatched, QEvent* a_pEvent) {
    // 粘性选择（#5）+ 空白取消（#2）：鼠标按下时探测落点是否在空白（无条目）处。
    // 空白处点击时 QTreeWidget/QListWidget 不会改变 current item，也就不触发
    // currentItemChanged —— 单靠置标志无法取消选中。故空白落点时主动 setCurrentItem(nullptr)
    // 强制触发 On*SelectionChanged(nullptr)，此时 m_bAllowDeselect 已置 true，走取消分支。
    if (a_pEvent->type() == QEvent::MouseButtonPress) {
        const QPoint pt = static_cast<QMouseEvent*>(a_pEvent)->pos();
        if (a_pWatched == m_pListCur->viewport()) {
            if (m_pListCur->itemAt(pt) == nullptr) {
                m_bAllowDeselect = true;
                m_pListCur->setCurrentItem(nullptr);
            }
        } else if (a_pWatched == m_pTreeHis->viewport()) {
            if (m_pTreeHis->itemAt(pt) == nullptr) {
                m_bAllowDeselect = true;
                m_pTreeHis->setCurrentItem(nullptr);
            }
        }
    }
    return QWidget::eventFilter(a_pWatched, a_pEvent);
}

void CTaskListPanel::OnHistoryContextMenu(const QPoint& a_pos) {
    // #6 历史表右键菜单：对所指任务（Done/Failed）提供「再次执行」——回到当前栏从当前阶段续跑。
    // （语义=原 OnReviveTask 续跑；入口由按钮改为右键菜单，去掉常驻「继续执行」按钮。）
    // 2026.08.19 增「删除」：彻底删除该任务记录（两表 + 全局历史库 + 工程侧 task.json），
    //   确认与删除逻辑在 CMainWindow::OnDeleteTask，此处只转发所选任务。
    QTreeWidgetItem* pItem = m_pTreeHis->itemAt(a_pos);
    if (pItem == nullptr) { return; }
    CTaskInfo* pTask = TaskOf(pItem);
    if (pTask == nullptr) { return; }
    QMenu menu(this);
    QAction* pActView = menu.addAction(QStringLiteral("查看任务内容"));   // FR-5（移首位：查看优先于再次执行）
    menu.addSeparator();   // 查看与再次执行之间加分隔线
    QAction* pActRetry = menu.addAction(QStringLiteral("再次执行"));
    menu.addSeparator();   // 删除是破坏性操作，与常规项分隔
    QAction* pActDelete = menu.addAction(QStringLiteral("删除"));
    QAction* pSel = menu.exec(m_pTreeHis->viewport()->mapToGlobal(a_pos));
    if (pSel == pActRetry) {
        emit sigReviveTask(pTask);
    } else if (pSel == pActView) {
        emit sigViewTask(pTask);
    } else if (pSel == pActDelete) {
        emit sigDeleteTask(pTask);
    }
}

void CTaskListPanel::OnCurrentContextMenu(const QPoint& a_pos) {
    // #3 当前表右键「移除」：把所指任务移到历史任务列表（保留记录，可再次执行）。
    // 确认对话框与归档逻辑在 CMainWindow::OnRemoveTask（业务层），此处只转发所选任务。
    // 2026.08.19 增「删除」：彻底删除该任务记录（与「移除」区别=不再进历史表），
    //   确认与删除逻辑在 CMainWindow::OnDeleteTask，此处只转发所选任务。
    QListWidgetItem* pItem = m_pListCur->itemAt(a_pos);
    if (pItem == nullptr) { return; }
    CTaskInfo* pTask = TaskOf(pItem);
    if (pTask == nullptr) { return; }
    QMenu menu(this);
    QAction* pActView = menu.addAction(QStringLiteral("查看任务内容"));   // FR-5
    menu.addSeparator();   // 查看与移除之间加分隔线
    QAction* pActRemove = menu.addAction(QStringLiteral("移除"));
    QAction* pActDelete = menu.addAction(QStringLiteral("删除"));
    QAction* pSel = menu.exec(m_pListCur->viewport()->mapToGlobal(a_pos));
    if (pSel == pActView) {
        emit sigViewTask(pTask);
    } else if (pSel == pActRemove) {
        emit sigRemoveTask(pTask);
    } else if (pSel == pActDelete) {
        emit sigDeleteTask(pTask);
    }
}
