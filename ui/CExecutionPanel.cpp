#include "CExecutionPanel.h"

#include <functional>
#include <utility>

#include <QLabel>
#include <QTreeWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QSet>
#include <QTabWidget>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QStringDecoder>
#include <QPushButton>
#include <QProgressBar>
#include <QToolButton>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QTextDocument>
#include <QTextCursor>
#include <QTimer>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QJsonValue>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPoint>
#include <QInputDialog>
#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QToolTip>
#include <QEnterEvent>

#include "ui/CArtifactView.h"
#include "ui/CMessageView.h"
#include "ui/CMessageBlock.h"
#include "core/CTaskInfo.h"
#include "core/CStageMachine.h"
#include "core/StageDef.h"

namespace {
// 渐进重建每帧创建的块数：单块成本约几毫秒~十几毫秒（Markdown 排版/工具卡构建），
// 20 块/帧 ≈ 数十毫秒，事件循环可穿插处理用户输入，避免整段同步重建卡死 UI。
constexpr int k_iRebuildChunk = 20;
}

// 费用栏前的执行态指示器（#4）：运行中转圈动画（12 段渐变弧），空闲显示静态状态色点。
// 无 Q_OBJECT（仅重写 paintEvent + 用基类 connect），无需 moc，定义在本 .cpp 内即可。
class CBusyIndicator : public QWidget {
public:
    explicit CBusyIndicator(QWidget* a_pParent = nullptr)
        : QWidget(a_pParent), m_iAngle(0), m_colIdle(180, 180, 180) {
        m_pTimer = new QTimer(this);
        m_pTimer->setInterval(70);
        connect(m_pTimer, &QTimer::timeout, this, [this]() {
            m_iAngle = (m_iAngle + 30) % 360;
            update();
        });
        setFixedSize(20, 20);
    }
    void SetBusy(bool a_bBusy) {
        if (a_bBusy) {
            m_pTimer->start();
        } else {
            m_pTimer->stop();
        }
        update();
    }
    void SetIdleColor(const QColor& a_col) { m_colIdle = a_col; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal dSide = qMin(width(), height());
        const QPointF ptCenter(width() / 2.0, height() / 2.0);
        if (m_pTimer->isActive()) {
            // 转圈 spinner：12 段弧透明度递增，整体随 m_iAngle 旋转，呈「旋转」视感。
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
            // 空闲：静态实心圆点，颜色由 SetIdleColor 按任务状态设定（灰=待运行/绿=完成/黄=暂停停止/红=失败）。
            p.setBrush(QBrush(m_colIdle));
            p.setPen(Qt::NoPen);
            p.drawEllipse(ptCenter, dSide / 4.0, dSide / 4.0);
        }
    }
private:
    int     m_iAngle;
    QTimer* m_pTimer;
    QColor  m_colIdle;
};

// FR-3：助手日志提示词下拉浮层。
// 一个含 QListWidget 的弹出窗口（Qt::Tool + 无边框，手动定位在半透明按钮下方），
// 列表项存「提问在 m_aPromptIdx 中的序号」到 UserRole。hover 列表项立即显示全文 tooltip
// （拦截 ToolTip 事件 + QToolTip::showText，无系统延迟）；点击回调 m_fnPicked。
// 无 Q_OBJECT（用 std::function 回调而非信号，避免 .cpp 内 Q_OBJECT 需 AUTOMOC 扫 .cpp）。
class CPromptFlyout : public QWidget {
public:
    using FPicked = std::function<void(int)>;
    explicit CPromptFlyout(QWidget* a_pParent, FPicked a_fnPicked)
        : QWidget(a_pParent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          m_fnPicked(std::move(a_fnPicked)) {
        setAttribute(Qt::WA_ShowWithoutActivating);   // 弹出但不抢焦点（不干扰输入框）
        auto* pV = new QVBoxLayout(this);
        pV->setContentsMargins(1, 1, 1, 1);
        m_pList = new QListWidget(this);
        m_pList->setMouseTracking(true);   // itemEntered 需开 mouseTracking
        m_pList->viewport()->installEventFilter(this);   // 拦 ToolTip：hover 项立即显全文
        // #2 提示词列表字体统一 11pt（CPromptFlyout 是独立类，不继承 CExecutionPanel 的 setPt11）。
        QFont fList = m_pList->font();
        fList.setPointSize(11);
        m_pList->setFont(fList);
        pV->addWidget(m_pList);
        setMaximumHeight(280);
        connect(m_pList, &QListWidget::itemClicked, this, [this](QListWidgetItem* a_pItem) {
            if (a_pItem != nullptr && m_fnPicked) {
                m_fnPicked(a_pItem->data(Qt::UserRole).toInt());
            }
        });
    }
    void Clear() { m_pList->clear(); }
    // 追加一条提问：a_iIdx = 提问在面板 m_aPromptIdx 序号，a_sText 全文（存 UserRole+1 用于 tooltip）。
    void AddItem(int a_iIdx, const QString& a_sText) {
        const QString sView = a_sText.section(QLatin1Char('\n'), 0, 0);   // 单行预览（首行）
        auto* pItem = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(a_iIdx + 1).arg(sView.left(60)), m_pList);
        pItem->setData(Qt::UserRole, a_iIdx);
        pItem->setData(Qt::UserRole + 1, a_sText);   // 全文，tooltip 用
        pItem->setToolTip(QString());   // 关闭系统 tooltip，改用即时 showText
    }
    int Count() const { return m_pList->count(); }
    void SetFixedWidthHint(int a_iW) { m_iWidthHint = a_iW; }
    int WidthHint() const { return m_iWidthHint; }
protected:
    bool eventFilter(QObject* a_pWatched, QEvent* a_pEvent) override {
        // hover 到列表项：立即（无延迟）用 QToolTip 显示该提问全文。
        if (a_pWatched == m_pList->viewport() &&
            a_pEvent->type() == QEvent::ToolTip) {
            const QPoint ptPos = m_pList->viewport()->mapFromGlobal(QCursor::pos());
            QListWidgetItem* pItem = m_pList->itemAt(ptPos);
            if (pItem != nullptr) {
                QToolTip::showText(QCursor::pos(), pItem->data(Qt::UserRole + 1).toString(), m_pList);
                return true;   // 吃掉，阻止系统 tooltip 延迟机制
            }
        }
        return QWidget::eventFilter(a_pWatched, a_pEvent);
    }
private:
    QListWidget* m_pList    = nullptr;
    FPicked      m_fnPicked;
    int          m_iWidthHint = 260;
};

// token 数格式化：<1000 原值，<1M 显示 K（1 位小数），<1G 显示 M，否则 G（2 位小数）。
// 供 UpdateContextButton / 费用统计栏复用，统一人可读的 K/M/G 文案。
QString FormatTokens(qint64 a_iTokens) {
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

// 运行耗时格式化（2026.08.15）：<1h 为 mm:ss，≥1h 为 h:mm:ss（状态栏「运行中」后显示）。
QString FormatElapsed(qint64 a_iMs) {
    const qint64 iTotalSec = a_iMs / 1000;
    const qint64 iH  = iTotalSec / 3600;
    const qint64 iM  = (iTotalSec % 3600) / 60;
    const qint64 iS  = iTotalSec % 60;
    if (iH > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(iH)
            .arg(iM, 2, 10, QLatin1Char('0'))
            .arg(iS, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(iM, 2, 10, QLatin1Char('0'))
        .arg(iS, 2, 10, QLatin1Char('0'));
}

CExecutionPanel::CExecutionPanel(QWidget* a_pParent)
    : QWidget(a_pParent) {
    // 统一 11pt 字体辅助（#7）：列表/表头/输入框/按钮显式设点数，不依赖样式继承。
    auto setPt11 = [](QWidget* a_pW) {
        QFont f = a_pW->font();
        f.setPointSize(11);
        a_pW->setFont(f);
    };

    auto* pV = new QVBoxLayout(this);
    // 右栏紧贴左侧分隔条：清零朝向分隔条的左边距（其余三边保留默认留白），
    // 使「阶段」内容与左栏「当前任务」之间只剩分隔条本身（5px），消除过大空隙（#4）。
    {
        QMargins mg = pV->contentsMargins();
        mg.setLeft(0);
        pV->setContentsMargins(mg);
    }

    auto* pSplit = new QSplitter(Qt::Vertical, this);

    // 阶段进度表
    m_pStages = new QTreeWidget(this);
    m_pStages->setHeaderLabels({QStringLiteral("阶段"), QStringLiteral("状态")});
    m_pStages->setRootIsDecorated(false);
    m_pStages->header()->setStretchLastSection(false);
    m_pStages->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    // FR-9：阶段表高度由左栏任务列表（Tab）底部位置动态同步（见 SyncStagesHeight），
    //       使「阶段表底部」与「任务列表底部」（不含按钮行）精确对齐，随字体/DPI 自适应。
    //       初始给一个防塌缩的最小高度，首次 resize 即被同步值（splitter setSizes）覆盖。
    //       2026.08.18 弃用 setFixedHeight：min==max 会把下方分隔条手柄锁死不可拖
    //       （阶段/日志中间的拖动条失效），改由 SyncStagesHeight 走 setSizes 保对齐。
    m_pStages->setMinimumHeight(120);
    setPt11(m_pStages);
    setPt11(m_pStages->header());
    // 需求3：阶段表右键「再次执行」（仅重跑该阶段，不推进后续）。
    m_pStages->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pStages, &QTreeWidget::customContextMenuRequested,
            this, &CExecutionPanel::OnStagesContextMenu);

    // 对话流（Claude Code 风格：Markdown 助手块 + 可折叠工具卡 + 事件徽章）
    m_pView = new CMessageView(this);

    // 日志 / 阶段产物 / 助手 三页 Tab
    m_pTabs = new QTabWidget(this);
    m_pTabs->addTab(m_pView, QStringLiteral("日志"));

    // —— Tab2：阶段产物（左列阶段清单 + 右栏 Markdown 渲染）——
    auto* pOutSplit = new QSplitter(Qt::Horizontal, m_pTabs);
    m_pOutputs = new QListWidget(pOutSplit);
    m_pOutputs->setMinimumWidth(160);
    setPt11(m_pOutputs);
    m_pArtifactView = new CArtifactView(pOutSplit);
    pOutSplit->addWidget(m_pOutputs);
    pOutSplit->addWidget(m_pArtifactView);
    pOutSplit->setStretchFactor(0, 1);
    pOutSplit->setStretchFactor(1, 4);
    m_pTabs->addTab(pOutSplit, QStringLiteral("阶段产物"));
    connect(m_pOutputs, &QListWidget::currentRowChanged,
            this, &CExecutionPanel::OnOutputSelected);

    // —— Tab3：提示词 agent（独立对话流，#1/#2）——
    m_pAdhocView = new CMessageView(this);
    setPt11(m_pAdhocView);
    m_pTabs->addTab(m_pAdhocView, QStringLiteral("助手"));

    pSplit->addWidget(m_pStages);
    pSplit->addWidget(m_pTabs);
    m_pStageSplit = pSplit;   // FR-9 对齐需要随时 setSizes（记成成员）
    // FR-6：收紧「当前任务」与「阶段」区之间的分隔条厚度（≈5px），其余间距不变。
    pSplit->setHandleWidth(5);
    pSplit->setStretchFactor(0, 2);
    pSplit->setStretchFactor(1, 5);
    pSplit->setChildrenCollapsible(false);   // 2026.08.18 两段都保底可见（手柄恢复可拖）
    pV->addWidget(pSplit, 1);

    // —— FR-3 任务运行中介入输入栏（阶段表/Tabs 下方，adhoc 助手栏之上）——
    // 仅当前任务 Running 时启用（UpdateTaskInjectState 按 m_bTaskRunning 刷新）。
    auto* pInjectBar = new QHBoxLayout();
    pInjectBar->setContentsMargins(0, 0, 0, 0);
    m_pTaskInput = new QPlainTextEdit(this);
    m_pTaskInput->setPlaceholderText(
        QStringLiteral("向当前运行中的任务输入介入指令（Ctrl+Enter 发送；任务运行中可用）"));
    m_pTaskInput->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pTaskInput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pTaskInput->setTabChangesFocus(true);
    m_pTaskInput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setPt11(m_pTaskInput);
    m_pTaskInput->installEventFilter(this);   // Ctrl+Enter 发送
    // #3 与助手输入框同一套高度自适应（单行起步，按行数增长，封顶 k_iTaskMaxLines 行），
    //    取代原先写死的 setFixedHeight(40)，使两框初始高度与增长行为完全一致。
    constexpr int k_iTaskMaxLines = 10;
    connect(m_pTaskInput, &QPlainTextEdit::blockCountChanged,
            this, [this](int){ AdjustInputHeight(m_pTaskInput, k_iTaskMaxLines); });
    AdjustInputHeight(m_pTaskInput, k_iTaskMaxLines);
    m_pBtnInject = new QPushButton(QStringLiteral("介入"), this);
    m_pBtnInject->setToolTip(QStringLiteral("向当前运行中的任务注入指令（Ctrl+Enter）"));
    setPt11(m_pBtnInject);
    pInjectBar->addWidget(m_pTaskInput, 1);
    pInjectBar->addWidget(m_pBtnInject);
    pV->addLayout(pInjectBar);

    // 介入发送：Ctrl+Enter（eventFilter 拦截）或点击「介入」。
    connect(m_pBtnInject, &QPushButton::clicked, this, [this]() {
        if (m_sCurId.isEmpty() || m_pTaskInput == nullptr) {
            return;
        }
        const QString sText = m_pTaskInput->toPlainText().trimmed();
        if (sText.isEmpty()) {
            return;
        }
        emit sigTaskInject(m_sCurId, sText);
        m_pTaskInput->clear();
    });

    // —— 提示词输入栏（日志区下方，#1）：输入框 + 只读切换 + 上下文大小 + 回退 + 发送 ——
    auto* pAdhocBar = new QHBoxLayout();
    pAdhocBar->setContentsMargins(0, 0, 0, 0);
    m_pAdhocInput = new QPlainTextEdit(this);
    m_pAdhocInput->setPlaceholderText(QStringLiteral("向助手输入指令（Ctrl+Enter 发送）"));
    m_pAdhocInput->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);   // #3 超 10 行显示滚动条
    m_pAdhocInput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pAdhocInput->setTabChangesFocus(true);   // Tab 切焦点而非缩进，避免偷键盘焦点
    m_pAdhocInput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setPt11(m_pAdhocInput);
    m_pAdhocInput->installEventFilter(this);   // #3 Ctrl+Enter 发送（Enter 留作换行）
    connect(m_pAdhocInput, &QPlainTextEdit::textChanged,
            this, &CExecutionPanel::OnAdhocTextChanged);
    // #3 高度自适应主驱动：blockCountChanged 在回车增/删行时必定触发、值即时准确，
    // 不依赖 documentLayout 的 documentSize（后者在构造瞬间 documentLayout() 可能为空，
    // 导致下方 connect 被跳过、信号从未连上，setFixedHeight 仅初始设一次单行高后再不更新）。
    connect(m_pAdhocInput, &QPlainTextEdit::blockCountChanged,
            this, [this](int){ OnAdhocTextChanged(); });
    OnAdhocTextChanged();   // 初始单行高度
    m_pBtnReadOnly = new QToolButton(this);
    m_pBtnReadOnly->setText(QStringLiteral("只读"));
    m_pBtnReadOnly->setCheckable(true);
    m_pBtnReadOnly->setToolTip(QStringLiteral("勾选后助手仅可 Read/Grep/Glob，不可改动工程"));
    setPt11(m_pBtnReadOnly);
    // 上下文占比进度条：本体只画进度（绿≤70% / 红>70%），当前值/最大值走 tooltip；
    // 右键「立即压缩」。原 autocompact 阈值档位菜单已去掉（阈值不再在 UI 暴露，
    // 内部沿用 config.ini 默认值维持自动压缩）。
    m_pBtnContext = new QProgressBar(this);
    m_pBtnContext->setRange(0, 100);
    m_pBtnContext->setTextVisible(false);
    m_pBtnContext->setFixedWidth(110);
    setPt11(m_pBtnContext);
    m_pBtnContext->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pBtnContext, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& a_stPos) {
        if (m_pBtnContext == nullptr) {
            return;
        }
        QMenu stMenu(m_pBtnContext);
        QAction* pActCompact = stMenu.addAction(QStringLiteral("立即压缩上下文"));
        QAction* pActSet = stMenu.addAction(QStringLiteral("设置上下文大小…"));
        QAction* pSel = stMenu.exec(m_pBtnContext->mapToGlobal(a_stPos));
        if (pSel == pActSet) {
            // 设置模型上下文窗口上限（千 token），覆盖按模型自动探测；持久化到 config.ini。
            bool bOk = false;
            const int iK = QInputDialog::getInt(
                this, QStringLiteral("设置上下文大小"),
                QStringLiteral("上下文窗口上限（千 token，范围 4~2000）："),
                static_cast<int>(qBound(qint64(4), m_iContextLimit / 1000, qint64(2000))),
                4, 2000, 1, &bOk);
            if (bOk && iK > 0) {
                SetUserContextLimit(static_cast<qint64>(iK) * 1000);
                emit sigAdhocContextLimitChanged(m_iUserContextLimit);
            }
        } else if (pSel == pActCompact) {
            emit sigAdhocCompact();
        }
    });
    UpdateContextButton();   // 进度条初始态（占比 0、绿色、tooltip 出数值）
    m_pBtnUndo = new QPushButton(QStringLiteral("↶ 回退"), this);
    m_pBtnUndo->setToolTip(QStringLiteral("撤销最后一条提问"));
    setPt11(m_pBtnUndo);
    m_pBtnSend = new QPushButton(QStringLiteral("发送"), this);
    setPt11(m_pBtnSend);
    pAdhocBar->addWidget(m_pAdhocInput, 1);
    pAdhocBar->addWidget(m_pBtnReadOnly);
    pAdhocBar->addWidget(m_pBtnContext);
    pAdhocBar->addWidget(m_pBtnUndo);
    pAdhocBar->addWidget(m_pBtnSend);
    pV->addLayout(pAdhocBar);

    // 发送：Ctrl+Enter（eventFilter 拦截）或点击「发送」；运行中点击 = 停止或追加指令。
    //   2026.08.19 运行中输入框有字 → 点击「➤ 追加指令」向正在执行的轮发送追加（claude
    //   轮中注入 / DSH 排队自动续发）；无字 → 维持原「⏹ 停止」。
    connect(m_pBtnSend, &QPushButton::clicked, this, [this]() {
        if (m_bAdhocRunning) {
            if (m_pAdhocInput != nullptr
                && !m_pAdhocInput->toPlainText().trimmed().isEmpty()) {
                DoAdhocSend();   // 运行中追加：走提交流，编排器按后端注入/排队
                return;
            }
            emit sigAdhocStop();   // #4 运行中无输入：按钮即「停止」
            return;
        }
        DoAdhocSend();
    });
    connect(m_pBtnUndo, &QPushButton::clicked, this, &CExecutionPanel::sigAdhocUndo);
    // FR-3：助手日志右上角半透明提示词按钮（历次 User 提问快速跳转，取代已删除的 ◀▶ 导航）。
    CreatePromptButton();
    // 右键「立即压缩」菜单已在 m_pBtnContext 创建处接好（customContextMenuRequested）。

    SetAdhocEnabled(false);   // 初始无工程：禁用输入栏（打开工程后由主窗口启用）

    // 费用 / token 统计栏（费用两位小数；token 用 K/M/G）
    // 费用栏：[执行态指示器][费用文本] 同行（#4）；指示器运行中转圈、空闲显状态色点。
    auto* pCostBar = new QHBoxLayout();
    pCostBar->setContentsMargins(0, 0, 0, 0);
    m_pBusy = new CBusyIndicator(this);
    m_pLblCost = new QLabel(QStringLiteral("费用: $0.00 | 输入:0 输出:0"), this);
    pCostBar->addWidget(m_pBusy);
    pCostBar->addWidget(m_pLblCost, 1);
    pV->addLayout(pCostBar);

    // #6 任务对话流去抖落盘：AppendData 标脏并启动 500ms 单次定时器，
    // 到期批量发 sigTaskChatChanged，由 CMainWindow 序列化写 chat.json（合并 claude 高频输出）。
    m_pChatSaveTimer = new QTimer(this);
    m_pChatSaveTimer->setSingleShot(true);
    connect(m_pChatSaveTimer, &QTimer::timeout, this, &CExecutionPanel::FlushTaskChat);

    // 2026.08.15 运行耗时显示：当前任务 Running 时每秒刷新费用/状态栏的「耗时 mm:ss」
    //   （时长由 CTaskInfo::RunElapsedMs 跨段累计，暂停/停止冻结显示）。
    m_pRunTimer = new QTimer(this);
    connect(m_pRunTimer, &QTimer::timeout, this, &CExecutionPanel::OnRunTimer);
}

void CExecutionPanel::UpdateCostLabel() {
    // 底部费用/状态栏：[费用|token|请求|状态|耗时]。无任务 / 从未运行过 → 不带耗时段。
    if (m_pCurTask == nullptr) {
        m_pLblCost->setText(QStringLiteral("费用: $0.00 | 输入:0 输出:0 | 请求:0"));
        return;
    }
    // 2026.08.18 费用 ≤0 显示 "-"（DSH 后端无计费恒 0）。
    const QString sCost = m_pCurTask->TotalCost() > 0.0
        ? QStringLiteral("$%1").arg(m_pCurTask->TotalCost(), 0, 'f', 2)
        : QStringLiteral("-");
    QString sText = QStringLiteral("费用: %1 | 输入:%2 输出:%3 | 请求:%4 | %5")
        .arg(sCost)
        .arg(FormatTokens(m_pCurTask->TotalInTokens()))
        .arg(FormatTokens(m_pCurTask->TotalOutTokens()))
        .arg(m_pCurTask->RequestCount())
        .arg(ToString(m_pCurTask->Status()));
    // 运行过（有累计或正在跑）才显示耗时；纯待执行任务不显示 00:00 噪音。
    if (m_pCurTask->Status() == ETaskStatus::Running || m_pCurTask->RunElapsedMs() > 0) {
        sText += QStringLiteral(" | 耗时 %1").arg(FormatElapsed(m_pCurTask->RunElapsedMs()));
    }
    m_pLblCost->setText(sText);
}

void CExecutionPanel::OnRunTimer() {
    // 运行中每秒刷新耗时（任务对象仍在即可刷新；非 Running 由 RefreshTask 停表）。
    if (m_pCurTask != nullptr && m_pCurTask->Status() == ETaskStatus::Running) {
        UpdateCostLabel();
    }
}

void CExecutionPanel::OnAssistantText(CTaskInfo* a_pTask, const QString& a_sText, const QString& a_sViewId) {
    if (a_pTask == nullptr) {
        return;
    }
    SBlockData st;
    st.eKind = CMessageBlock::Assistant;
    // 评审多视角并行：带视角前缀，让用户在共享对话流中区分各视角输出（轻量标签）。
    st.sText = a_sViewId.isEmpty() ? a_sText
               : QStringLiteral("【视角 %1】%2").arg(a_sViewId, a_sText);
    st.sViewId = a_sViewId;
    AppendData(a_pTask->Id(), st);
}

void CExecutionPanel::OnToolUse(CTaskInfo* a_pTask, const QString& a_sTool,
                                const QString& a_sInputJson, const QString& a_sViewId) {
    if (a_pTask == nullptr) {
        return;
    }
    SBlockData st;
    st.eKind  = CMessageBlock::ToolUse;
    st.sTool  = a_sTool;
    st.sInput = a_sInputJson;
    st.sViewId = a_sViewId;
    AppendData(a_pTask->Id(), st);
}

void CExecutionPanel::OnToolResult(CTaskInfo* a_pTask, const QString& a_sResult, const QString& a_sViewId) {
    if (a_pTask == nullptr) {
        return;
    }
    const QString sId = a_pTask->Id();
    QList<SBlockData>& aBlocks = m_aBlocks[sId];

    // 回填到最近一个尚无结果的 ToolUse（且视角匹配：并发下按 sViewId 过滤防串配）。
    // Claude 工具流严格 use→result 交替；单 runner 串行下 sViewId 为空，行为与原启发式一致。
    for (int i = aBlocks.size() - 1; i >= 0; --i) {
        if (aBlocks[i].eKind == CMessageBlock::ToolUse && aBlocks[i].sResult.isEmpty()
            && aBlocks[i].sViewId == a_sViewId) {
            aBlocks[i].sResult = a_sResult;
            // 当前任务：同步刷新对应 widget
            if (sId == m_sCurId && i < m_aCurWidgets.size()) {
                m_aCurWidgets[i]->SetResult(a_sResult);
            }
            MarkTaskDirty(sId);   // #6 回填结果也落盘
            return;
        }
    }
    // 没找到未闭合的工具卡：作为系统行兜底（避免丢失）
    AppendLog(sId, a_sResult);
}

void CExecutionPanel::AppendLog(const QString& a_sTaskId, const QString& a_sLine) {
    SBlockData st;
    // 错误信息单独着色
    st.eKind = a_sLine.startsWith(QStringLiteral("[错误]"))
               ? CMessageBlock::Error
               : CMessageBlock::System;
    st.sText = a_sLine;
    AppendData(a_sTaskId, st);
}

void CExecutionPanel::AppendEvent(const QString& a_sTaskId, const QString& a_sKind,
                                  const QString& a_sText) {
    SBlockData st;
    st.eKind      = CMessageBlock::Event;
    st.sEventKind = a_sKind;
    st.sText      = a_sText;
    AppendData(a_sTaskId, st);
}

void CExecutionPanel::AppendData(const QString& a_sTaskId, const SBlockData& a_stData) {
    m_aBlocks[a_sTaskId].append(a_stData);
    MarkTaskDirty(a_sTaskId);   // #6 标脏，去抖落盘 chat.json
    if (a_sTaskId != m_sCurId) {
        return;   // 非当前任务：只缓存，不刷视图
    }
    if (m_bRebuilding) {
        return;   // 渐进重建中：分帧循环按实时列表顺序创建本块，此处跳过防重复上屏
    }
    CMessageBlock* pW = CreateWidget(a_stData);
    m_aCurWidgets.append(pW);
    m_pView->AddBlock(pW);
}

CMessageBlock* CExecutionPanel::CreateWidget(const SBlockData& a_stData) {
    auto* pW = new CMessageBlock(a_stData.eKind, m_pView);
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            pW->SetAssistantText(a_stData.sText);
            break;
        case CMessageBlock::ToolUse:
            pW->SetTool(a_stData.sTool, a_stData.sInput);
            if (!a_stData.sResult.isEmpty()) {
                pW->SetResult(a_stData.sResult);   // 重建时已有结果（回填过的）
            }
            break;
        case CMessageBlock::Event:
            pW->SetEvent(a_stData.sEventKind, a_stData.sText);
            break;
        case CMessageBlock::System:
        case CMessageBlock::Error:
            pW->SetSystemText(a_stData.sText);
            break;
    }
    return pW;
}

void CExecutionPanel::RebuildView() {
    // FR-4：重建等同「跳到最新」，强制跟随到底。
    // 分帧渐进创建：历史任务对话流可达数百块（Markdown 排版/工具卡构建均不便宜），
    // 同步一次建完会把 UI 卡住数秒；分帧让事件循环穿插呼吸，点击任务立即上屏、
    // 其余块随后若干帧补齐，期间逐块滚底也一并关闭（O(N²) 布局是最大卡顿源）。
    m_pView->ResetFollow();
    m_pView->Clear();
    m_aCurWidgets.clear();
    m_iRebuildIdx = 0;
    ++m_uRebuildGen;   // 作废仍在排队的旧帧（上次重建尚未跑完就切了任务）
    m_bRebuilding = true;   // AppendData 新块交分帧循环按序创建，防重复上屏
    RebuildChunk(m_uRebuildGen);
}

void CExecutionPanel::RebuildChunk(quint32 a_uGen) {
    if (a_uGen != m_uRebuildGen) {
        return;   // 已被新的重建顶替（切任务/切工程），本帧作废
    }
    // 取实时列表引用：重建期间新到的当前任务块（AppendData 已延迟到这里）也能被覆盖到。
    const QList<SBlockData>& aBlocks = m_aBlocks[m_sCurId];
    int iCreated = 0;
    while (m_iRebuildIdx < aBlocks.size() && iCreated < k_iRebuildChunk) {
        CMessageBlock* pW = CreateWidget(aBlocks.at(m_iRebuildIdx));
        m_aCurWidgets.append(pW);
        m_pView->AddBlock(pW, /*a_bBatch=*/true);   // 批量：不逐块滚底
        ++m_iRebuildIdx;
        ++iCreated;
    }
    if (m_iRebuildIdx < aBlocks.size()) {
        QTimer::singleShot(0, this, [this, a_uGen]() { RebuildChunk(a_uGen); });
        return;
    }
    m_bRebuilding = false;
    m_pView->ScrollToBottomRobust();   // 需求7：重建完成后多帧滚到任务日志最底
}

void CExecutionPanel::LoadStageOutputs(CTaskInfo* a_pTask) {
    // 「阶段产物」页：按选中任务的流程阶段，列出任务私有目录下已生成的 .md 产物。
    m_pOutputs->clear();
    m_pArtifactView->Clear();
    if (a_pTask == nullptr) {
        return;
    }
    const QString sTaskDir = QDir::cleanPath(
        a_pTask->ProjectDir() + QStringLiteral("/.autodev/tasks/") + a_pTask->Id());
    // 2026.08.15 (流程档位) 按任务生效流列出产物（清单为空=完整规范流，行为同旧版）。
    const QList<EStage> aFlow = ResolveFlow(a_pTask->Type(), a_pTask->FlowStages());
    for (const EStage eStage : aFlow) {
        const SStageDef* pDef = FindStage(eStage);
        if (pDef == nullptr || pDef->sOutput.isEmpty()) {
            continue;   // 无独立产物文件的阶段（如开发/测试）跳过
        }
        const QString sPath = QDir::cleanPath(sTaskDir + QStringLiteral("/") + pDef->sOutput);
        if (!QFileInfo::exists(sPath)) {
            continue;   // 该阶段尚未产出文件
        }
        auto* pItem = new QListWidgetItem(
            QStringLiteral("%1  (%2)").arg(pDef->sNameZh, pDef->sOutput), m_pOutputs);
        pItem->setData(Qt::UserRole, sPath);
    }
    if (m_pOutputs->count() > 0) {
        m_pOutputs->setCurrentRow(0);   // 触发 OnOutputSelected 显示首份产物
    }
}

void CExecutionPanel::RefreshStageOutputs(CTaskInfo* a_pTask) {
    // FR-5：重扫前记录当前选中产物的绝对路径（按 UserRole 存的值），重扫后若该文件仍存在
    // 则重新选中同一行（按路径精确匹配，非按行号），避免查看内容被无故清空（AC-5.2）。
    QString sSelPath;
    const int iCurRow = m_pOutputs->currentRow();
    if (iCurRow >= 0 && iCurRow < m_pOutputs->count()) {
        QListWidgetItem* pItem = m_pOutputs->item(iCurRow);
        if (pItem != nullptr) {
            sSelPath = pItem->data(Qt::UserRole).toString();
        }
    }

    // LoadStageOutputs 会 clear + 重扫 + 默认选中第 0 行（触发 OnOutputSelected 显示首份产物）。
    LoadStageOutputs(a_pTask);

    // 重扫后按路径找回原选中项；找到则切到该行（ShowFile 同一文件，内容不变）。
    if (!sSelPath.isEmpty()) {
        for (int i = 0; i < m_pOutputs->count(); ++i) {
            QListWidgetItem* pItem = m_pOutputs->item(i);
            if (pItem != nullptr && pItem->data(Qt::UserRole).toString() == sSelPath) {
                m_pOutputs->setCurrentRow(i);
                break;
            }
        }
    }
}

void CExecutionPanel::OnOutputSelected(int a_iRow) {
    if (a_iRow < 0 || a_iRow >= m_pOutputs->count()) {
        return;
    }
    QListWidgetItem* pItem = m_pOutputs->item(a_iRow);
    if (pItem == nullptr) {
        return;
    }
    const QString sPath = pItem->data(Qt::UserRole).toString();
    m_pArtifactView->ShowFile(sPath);   // 文件读取 + 编码兜底 + Markdown 渲染由 CArtifactView 负责
}

void CExecutionPanel::OnStagesContextMenu(const QPoint& a_stPos) {
    // 需求3：阶段表右键「再次执行」——仅重跑选中阶段，不推进后续（编排器 RunSingleStage）。
    QTreeWidgetItem* pItem = m_pStages->itemAt(a_stPos);
    if (pItem == nullptr) {
        return;
    }
    bool bOk = false;
    const int iStage = pItem->data(0, Qt::UserRole).toInt(&bOk);
    if (!bOk || m_pCurTask == nullptr) {
        return;
    }
    QMenu menu(this);
    QAction* pActRerun = menu.addAction(QStringLiteral("再次执行"));
    QAction* pSel = menu.exec(m_pStages->viewport()->mapToGlobal(a_stPos));
    if (pSel == pActRerun) {
        emit sigRerunStage(m_pCurTask, iStage);
    }
}

void CExecutionPanel::RefreshTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr || a_pTask->Id() != m_sCurId) {
        return;
    }
    m_pStages->clear();

    // 2026.08.15 (流程档位) 阶段表按任务生效流显示（清单为空=完整规范流，行为同旧版）。
    const QList<EStage> aFlow = ResolveFlow(a_pTask->Type(), a_pTask->FlowStages());

    QSet<EStage> aDone;
    for (const CTaskInfo::SStageRun& stRun : a_pTask->History()) {
        if (stRun.bOk) {
            aDone.insert(stRun.eStage);
        }
    }
    const EStage eCur = a_pTask->CurrentStage();

    for (const EStage eStage : aFlow) {
        const SStageDef* pDef = FindStage(eStage);
        const QString sName = pDef ? pDef->sNameZh : QString::number(static_cast<int>(eStage));

        QString sStatus;
        QColor  color;
        if (aDone.contains(eStage) || a_pTask->Status() == ETaskStatus::Done) {
            sStatus = QStringLiteral("✔ 已完成");
            color   = QColor(0, 128, 0);
        } else if (eStage == eCur) {
            // 2026.08.15 当前阶段按任务执行时状态标注（历史任务回看不再清一色「待执行」）。
            switch (a_pTask->Status()) {
                case ETaskStatus::Running:
                    sStatus = QStringLiteral("▶ 进行中");
                    color   = QColor(0, 0, 200);
                    break;
                case ETaskStatus::Failed:
                    sStatus = QStringLiteral("✖ 失败");
                    color   = QColor(200, 0, 0);
                    break;
                case ETaskStatus::Paused:
                case ETaskStatus::Stopped:
                    sStatus = QStringLiteral("⏸ 未完成");
                    color   = QColor(210, 150, 30);
                    break;
                default:   // Pending：尚未启动，当前阶段也只是待执行
                    sStatus = QStringLiteral("○ 待执行");
                    color   = QColor(150, 150, 150);
                    break;
            }
        } else {
            sStatus = QStringLiteral("○ 待执行");
            color   = QColor(150, 150, 150);
        }

        auto* pItem = new QTreeWidgetItem({sName, sStatus});
        pItem->setForeground(0, color);
        pItem->setForeground(1, color);
        pItem->setData(0, Qt::UserRole, static_cast<int>(eStage));   // 需求3：供右键「再次执行」取阶段
        m_pStages->addTopLevelItem(pItem);
    }

    UpdateCostLabel();   // 2026.08.15 费用/状态栏统一入口（含运行耗时）

    // #4 执行态指示器：记录运行态 + 状态色，刷新指示器。
    m_bTaskRunning = (a_pTask->Status() == ETaskStatus::Running);
    // 2026.08.15 运行中启动耗时 1s 刷新；离开运行态停表（冻结显示最后耗时）。
    if (m_bTaskRunning) {
        m_pRunTimer->start(1000);
    } else {
        m_pRunTimer->stop();
    }
    switch (a_pTask->Status()) {
        case ETaskStatus::Done:    m_colIdle = QColor(40, 160, 60); break;   // 绿
        case ETaskStatus::Paused:
        case ETaskStatus::Stopped: m_colIdle = QColor(210, 150, 30); break;  // 黄
        case ETaskStatus::Failed:  m_colIdle = QColor(210, 60, 60); break;   // 红
        case ETaskStatus::Running: m_colIdle = QColor(60, 130, 220); break;  // 蓝
        default:                   m_colIdle = QColor(180, 180, 180); break; // 灰（待运行）
    }
    UpdateBusyIndicator();
    UpdateTaskInjectState();   // FR-3 按运行态刷新介入栏
}

void CExecutionPanel::SetCurrentTask(CTaskInfo* a_pTask) {
    m_pStages->clear();
    m_pView->Clear();
    m_aCurWidgets.clear();
    m_sCurId = a_pTask ? a_pTask->Id() : QString();
    m_pCurTask = a_pTask;   // 需求3：供阶段表右键「再次执行」取当前任务

    if (a_pTask != nullptr) {
        RebuildView();
        RefreshTask(a_pTask);
    } else {
        UpdateCostLabel();             // 2026.08.15 无任务：费用栏归零（不含耗时段）
        m_bTaskRunning = false;        // #4 无任务：指示器归空闲灰点
        m_pRunTimer->stop();           // 2026.08.15 无任务：停耗时刷新
        m_colIdle = QColor(200, 200, 200);
        UpdateBusyIndicator();
        UpdateTaskInjectState();       // FR-3 无任务：禁用介入栏
    }
    LoadStageOutputs(a_pTask);   // 切换任务时同步刷新「阶段产物」页（nullptr 时清空）
}

// ===========================================================================
// 提示词 agent（「助手」Tab 独立对话流，#1/#2/#3/#4）
// ===========================================================================

void CExecutionPanel::AppendAdhocBlock(const SBlockData& a_stData) {
    m_aAdhocBlocks.append(a_stData);
    // 分帧重建期间只入数据不建控件（镜像 AppendData 让路）：分帧循环读实时列表会按序补上，
    //   否则会同一块建两份控件。重建结束后的常规路径仍即时建块。
    if (!m_bAdhocRebuilding) {
        // 复用 CreateWidget（其内部以 m_pView 为 parent，AddBlock 会把块 reparent 进 m_pAdhocView）。
        CMessageBlock* pW = CreateWidget(a_stData);
        m_aAdhocWidgets.append(pW);
        m_pAdhocView->AddBlock(pW);
    }
    // FR-2：不再无条件 ScrollToBottom；滚动决策交给 CMessageView::AddBlock 内部 m_bFollow
    //        机制（与任务日志 Tab 行为一致，AC-2.1～2.4）。
    // FR-3：新增用户提问时刷新提示词下拉索引（供半透明按钮下拉跳转）。
    if (a_stData.eKind == CMessageBlock::Event &&
        a_stData.sEventKind == QStringLiteral("user")) {
        RebuildPromptList();
    }
    emit sigAdhocBlocksChanged();   // #1 助手流变化 → 触发落盘 assistant.json
}

void CExecutionPanel::RebuildAdhocView() {
    // 分帧渐进创建（与任务视图 RebuildView/RebuildChunk 同款）：助手档按任务分档后，
    //   单任务历史对话可达数百块，同步一次建完会卡 UI；切档/切工程时代际号作废在途旧帧。
    m_pAdhocView->ResetFollow();   // FR-4：重建等同「跳到最新」，强制跟随到底
    m_pAdhocView->Clear();
    m_aAdhocWidgets.clear();
    m_iAdhocRebuildIdx = 0;
    ++m_uAdhocRebuildGen;   // 作废仍在排队的旧帧（上次重建尚未跑完就切了档）
    m_bAdhocRebuilding = true;   // AppendAdhocBlock 新块交分帧循环按序创建，防重复上屏
    RebuildAdhocChunk(m_uAdhocRebuildGen);
}

void CExecutionPanel::RebuildAdhocChunk(quint32 a_uGen) {
    if (a_uGen != m_uAdhocRebuildGen) {
        return;   // 已被新的重建顶替（切档/切工程），本帧作废
    }
    int iCreated = 0;
    while (m_iAdhocRebuildIdx < m_aAdhocBlocks.size() && iCreated < k_iRebuildChunk) {
        CMessageBlock* pW = CreateWidget(m_aAdhocBlocks.at(m_iAdhocRebuildIdx));
        m_aAdhocWidgets.append(pW);
        m_pAdhocView->AddBlock(pW, /*a_bBatch=*/true);   // 批量：不逐块滚底
        ++m_iAdhocRebuildIdx;
        ++iCreated;
    }
    if (m_iAdhocRebuildIdx < m_aAdhocBlocks.size()) {
        QTimer::singleShot(0, this, [this, a_uGen]() { RebuildAdhocChunk(a_uGen); });
        return;
    }
    m_bAdhocRebuilding = false;
    m_pAdhocView->ScrollToBottomRobust();   // 需求7：恢复完成后多帧滚到助手日志最底
    RebuildPromptList();   // FR-3 按恢复后的助手流重建提示词下拉索引
}

void CExecutionPanel::OnAdhocAssistantText(const QString& a_sText) {
    SBlockData st;
    st.eKind = CMessageBlock::Assistant;
    st.sText = a_sText;
    AppendAdhocBlock(st);
}

void CExecutionPanel::OnAdhocToolUse(const QString& a_sTool, const QString& a_sInputJson) {
    SBlockData st;
    st.eKind  = CMessageBlock::ToolUse;
    st.sTool  = a_sTool;
    st.sInput = a_sInputJson;
    AppendAdhocBlock(st);
}

void CExecutionPanel::OnAdhocToolResult(const QString& a_sResult) {
    // 回填到最近一个尚无结果的 ToolUse（与任务流相同的 use→result 交替启发式）。
    for (int i = m_aAdhocBlocks.size() - 1; i >= 0; --i) {
        if (m_aAdhocBlocks[i].eKind == CMessageBlock::ToolUse &&
            m_aAdhocBlocks[i].sResult.isEmpty()) {
            m_aAdhocBlocks[i].sResult = a_sResult;
            if (i < m_aAdhocWidgets.size()) {
                m_aAdhocWidgets[i]->SetResult(a_sResult);
            }
            emit sigAdhocBlocksChanged();   // #1 回填结果也持久化
            return;
        }
    }
    OnAdhocLog(a_sResult);   // 无未闭合工具卡：作为系统行兜底
}

void CExecutionPanel::OnAdhocLog(const QString& a_sLine) {
    SBlockData st;
    st.eKind = a_sLine.startsWith(QStringLiteral("[错误]"))
               ? CMessageBlock::Error
               : CMessageBlock::System;
    st.sText = a_sLine;
    AppendAdhocBlock(st);
}

void CExecutionPanel::OnAdhocEvent(const QString& a_sKind, const QString& a_sText) {
    SBlockData st;
    st.eKind      = CMessageBlock::Event;
    st.sEventKind = a_sKind;
    st.sText      = a_sText;
    AppendAdhocBlock(st);
}

void CExecutionPanel::OnAdhocUndo() {
    // 从末尾回溯找最后一个「用户提问」块，连同其后所有回复/工具/事件块一并移除。
    int iCut = -1;
    for (int i = m_aAdhocBlocks.size() - 1; i >= 0; --i) {
        if (m_aAdhocBlocks[i].eKind == CMessageBlock::Event &&
            m_aAdhocBlocks[i].sEventKind == QStringLiteral("user")) {
            iCut = i;
            break;
        }
    }
    if (iCut < 0) {
        return;   // 无提问块可撤销（理论不达：按钮在编排器栈空时已拦截）
    }
    while (m_aAdhocBlocks.size() > iCut) {
        m_aAdhocBlocks.removeLast();
    }
    RebuildAdhocView();
    emit sigAdhocBlocksChanged();   // #1 撤销后落盘
}

void CExecutionPanel::OnAdhocFinished() {
    SetAdhocRunning(false);   // 一次助手调用结束：恢复输入栏（受工程开关约束）
}

void CExecutionPanel::SetAdhocContext(qint64 a_iFill) {
    // <0 表示撤销后未知；否则显示 当前占用 / 窗口。记录 fill，交由 UpdateContextButton 统一出文案。
    // 2026.08.15 旧档脏值钳制：历史存档可能带有远超窗口的假占用（result 累计兜底/中转脏 usage
    //   产物，7ce53172 实测 1.9M）。还原或收到此类值时按未知（—）处理，不再显示「1.9M（异常）」
    //   满档——否则每次落盘又把脏值写回 assistant.json，形成还原→显示→再落盘的黏住循环。
    if (a_iFill > 0 && m_iContextLimit > 0 && a_iFill > m_iContextLimit) {
        a_iFill = -1;
    }
    m_iLastContextFill = a_iFill;
    UpdateContextButton();
}

void CExecutionPanel::UpdateContextButton() {
    // 进度条显示「当前占用 / 模型上下文窗口」占比；本体只画进度（绿≤70% / 红>70%），
    // 当前值/最大值/百分比走 tooltip。脏数据（fill>窗口）钳到满档红色并标注异常。
    if (m_pBtnContext == nullptr) {
        return;
    }
    // #5 合理性校验：fill 应 ≤ 当前模型上下文窗口。GLM 等中转层偶发返回异常巨大的
    //   cache_read（实测一轮暴涨到 1.4M，远超 200K 窗口），超过窗口上限视为脏数据。
    const bool bUnknown = (m_iLastContextFill < 0);
    const bool bBogus = (m_iLastContextFill > m_iContextLimit);
    int iPct = 0;
    double dRatio = 0.0;
    QString sFillTxt;
    if (bUnknown) {
        sFillTxt = QStringLiteral("—");
    } else if (bBogus) {
        iPct = 100;
        dRatio = 1.0;
        sFillTxt = FormatTokens(m_iLastContextFill) + QStringLiteral("（异常）");
    } else {
        dRatio = m_iContextLimit > 0
            ? static_cast<double>(m_iLastContextFill) / m_iContextLimit : 0.0;
        iPct = static_cast<int>(dRatio * 100.0);
        sFillTxt = FormatTokens(m_iLastContextFill);
    }
    m_pBtnContext->setValue(iPct);
    const bool bRed = bBogus || dRatio > 0.7;   // 超 70%（或脏数据）显红
    m_pBtnContext->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #d1d5db; border-radius: 3px;"
        " background: #f3f4f6; min-height: 20px; }"
        "QProgressBar::chunk { background: %1; }")
        .arg(bRed ? QStringLiteral("#ef4444") : QStringLiteral("#22c55e")));
    QString sTip = QStringLiteral("上下文：%1 / %2（%3%）")
                       .arg(sFillTxt, FormatTokens(m_iContextLimit)).arg(iPct);
    if (!m_sAdhocModel.isEmpty()) {
        sTip += QStringLiteral("  ·  模型: %1").arg(m_sAdhocModel);
    }
    m_pBtnContext->setToolTip(sTip);
}

void CExecutionPanel::OnAdhocModel(const QString& a_sModel) {
    // 收到助手本轮所用模型名 → 更新上下文窗口分母（用户设了上限则覆盖探测），刷新进度条。
    m_sAdhocModel = a_sModel;
    m_iContextLimit = (m_iUserContextLimit > 0)
        ? m_iUserContextLimit : ContextLimitForModel(a_sModel);
    UpdateContextButton();
}

void CExecutionPanel::SetUserContextLimit(qint64 a_iLimit) {
    // 用户右键设的窗口上限：>0 生效（覆盖按模型探测），0/负=回退自动探测；改后即时刷新进度条。
    m_iUserContextLimit = a_iLimit > 0 ? a_iLimit : 0;
    m_iContextLimit = (m_iUserContextLimit > 0)
        ? m_iUserContextLimit
        : ContextLimitForModel(m_sAdhocModel);
    UpdateContextButton();
}

void CExecutionPanel::UpdateTaskInjectState() {
    // FR-3：介入栏仅当前任务 Running 时启用（AC-3.4 非运行态禁用入口）。
    const bool bOk = m_bTaskRunning && !m_sCurId.isEmpty();
    if (m_pTaskInput != nullptr) {
        m_pTaskInput->setEnabled(bOk);
    }
    if (m_pBtnInject != nullptr) {
        m_pBtnInject->setEnabled(bOk);
    }
}

qint64 CExecutionPanel::ContextLimitForModel(const QString& a_sModel) {
    // 按模型名粗分上下文窗口（token）；未命中回退 200K。具体窗口以模型官方规格为准，
    // 按钮 tooltip 会显示真实模型名供核对（新模型可在此补充映射）。
    const QString s = a_sModel.toLower().trimmed();
    if (s.contains(QStringLiteral("glm"))) {
        return 128000;   // GLM-4.x 系列（按公开规格 128K）
    }
    if (s.contains(QStringLiteral("claude"))) {
        return 200000;   // Claude Sonnet/Opus/Haiku 4 系列（公开 200K）
    }
    return 200000;   // 未知模型回退
}

void CExecutionPanel::UpdateAdhocInputState() {
    // 2026.08.19 运行中可输入追加指令：输入栏可用只看工程开关（空闲/运行中都能打字——
    //   运行中输入+发送 = 向正在执行的助手轮追加指令，claude 后端轮中注入、DSH 后端
    //   排队本轮结束后自动发送）；上下文/撤销按钮仍要求空闲。
    const bool bOk = m_bAdhocEnabled && !m_bAdhocRunning;
    m_pAdhocInput->setEnabled(m_bAdhocEnabled);
    m_pBtnContext->setEnabled(bOk);
    m_pBtnUndo->setEnabled(bOk);
    // #4 运行态标识：运行中「发送」变「停止」（红色、可点→sigAdhocStop）；空闲恢复「发送」。
    //   2026.08.19 运行中且输入框有字 → 「➤ 追加指令」（发送追加，不清空则随时可停）。
    const bool bHasText = m_pAdhocInput != nullptr
                          && !m_pAdhocInput->toPlainText().trimmed().isEmpty();
    if (m_bAdhocRunning && bHasText) {
        m_pBtnSend->setText(QStringLiteral("➤ 追加指令"));
        m_pBtnSend->setEnabled(true);
        m_pBtnSend->setStyleSheet(
            QStringLiteral("QPushButton{background:#2563eb;color:white;}"));
    } else if (m_bAdhocRunning) {
        m_pBtnSend->setText(QStringLiteral("⏹ 停止"));
        m_pBtnSend->setEnabled(true);
        m_pBtnSend->setStyleSheet(QStringLiteral("QPushButton{background:#dc2626;color:white;}"));
    } else {
        m_pBtnSend->setText(QStringLiteral("发送"));
        m_pBtnSend->setEnabled(bOk);
        m_pBtnSend->setStyleSheet(QString());
    }
}

void CExecutionPanel::SetAdhocEnabled(bool a_bEnabled) {
    m_bAdhocEnabled = a_bEnabled;
    UpdateAdhocInputState();
}

void CExecutionPanel::AppendToAdhocInput(const QString& a_sText) {
    // FR-3：把资源管理器选中的路径追加到助手输入框末尾（与已有内容之间留一个空格）。
    if (m_pAdhocInput == nullptr || a_sText.isEmpty()) {
        return;
    }
    // 切到助手 Tab，让用户看到注入结果。
    const int iIdx = m_pTabs->indexOf(m_pAdhocView);
    if (iIdx >= 0) {
        m_pTabs->setCurrentIndex(iIdx);
    }
    QString sCur = m_pAdhocInput->toPlainText();
    if (!sCur.isEmpty() && !sCur.endsWith(QLatin1Char('\n')) &&
        !sCur.endsWith(QLatin1Char(' '))) {
        sCur += QLatin1Char(' ');
    }
    m_pAdhocInput->setPlainText(sCur + a_sText);
    // 光标移到末尾并聚焦，便于直接续写指令。
    QTextCursor stCur = m_pAdhocInput->textCursor();
    stCur.movePosition(QTextCursor::End);
    m_pAdhocInput->setTextCursor(stCur);
    m_pAdhocInput->setFocus();
}

void CExecutionPanel::SetAdhocRunning(bool a_bRunning) {
    m_bAdhocRunning = a_bRunning;
    // #4 助手 Tab 标签提示运行态（●运行中 / 空闲）。
    const int iIdx = m_pTabs->indexOf(m_pAdhocView);
    if (iIdx >= 0) {
        m_pTabs->setTabText(iIdx, a_bRunning ? QStringLiteral("助手 ●") : QStringLiteral("助手"));
    }
    UpdateAdhocInputState();
    UpdateBusyIndicator();   // #4 助手运行态变化 → 刷新执行态指示器
}

void CExecutionPanel::UpdateBusyIndicator() {
    // #4 综合「当前任务运行中」或「助手运行中」任一为真 → 转圈动画；否则显示状态色点。
    if (m_pBusy == nullptr) {
        return;
    }
    const bool bBusy = m_bTaskRunning || m_bAdhocRunning;
    m_pBusy->SetBusy(bBusy);
    m_pBusy->SetIdleColor(m_colIdle);
}

void CExecutionPanel::DoAdhocSend() {
    // 发送当前输入（Ctrl+Enter 或「发送」按钮调用）。提取自原 fnSend lambda。
    const QString sText = m_pAdhocInput->toPlainText().trimmed();
    if (sText.isEmpty()) {
        return;
    }
    // 先本地渲染「用户提问」块（❯），与编排器撤销栈一一对应：撤销=移除本块及之后所有块。
    SBlockData stUser;
    stUser.eKind      = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    stUser.sText      = sText;
    AppendAdhocBlock(stUser);
    // #2 发送后自动切到「助手」Tab，让用户立即看到日志。
    m_pTabs->setCurrentWidget(m_pAdhocView);
    // #4 先进入运行态（助手 Tab 标 ●、发送变「停止」），再 emit 提交：
    // 若提交在同步链中即失败（如 claude 启动失败），OnAdhocFinished 的 SetAdhocRunning(false)
    // 会落在 SetAdhocRunning(true) 之后，最终态正确归零；反之(true 覆盖在 false 之后)会卡在「运行中」假态。
    SetAdhocRunning(true);
    emit sigAdhocSubmit(sText, m_pBtnReadOnly->isChecked());
    m_pAdhocInput->clear();   // 清空 → textChanged → 高度回到单行
}

void CExecutionPanel::AdjustInputHeight(QPlainTextEdit* a_pEdit, int a_iMaxLines) {
    // #3 输入框高度自适应通用版（介入栏 / 助手栏共用，保证两框初始高度与增长行为一致）：
    // 按文本块数（blockCount = 回车产生的逻辑行数）从单行向上扩展，封顶 a_iMaxLines 行；
    // 超 a_iMaxLines 固定该高度并交由滚动条（ScrollBarAsNeeded）。清空回单行。
    // 高度直接由 blockCount() 线性计算——blockCountChanged 回车必触发、值即时准确，
    // 不依赖 documentLayout 布局时机（documentSize 在 QPlainTextEdit 上曾测高失效）。
    if (a_pEdit == nullptr || a_iMaxLines < 1) {
        return;
    }
    const QFontMetrics fm = a_pEdit->fontMetrics();
    const int iLineH  = fm.lineSpacing();
    const int iFrame  = a_pEdit->frameWidth() * 2;
    const int iMargin = a_pEdit->contentsMargins().top()
                      + a_pEdit->contentsMargins().bottom();
    // 计入 QTextDocument::documentMargin（默认上下各 4px）：viewport 内文本上下还各有
    // 一段 docMargin 内边距，漏算会让"行数正好"时末行被顶出可视区（如 2 行只看到第 2 行）。
    QTextDocument* pDoc = a_pEdit->document();
    const qreal dDocMargin = (pDoc != nullptr) ? pDoc->documentMargin() : 0.0;
    const int iDocMargin = static_cast<int>(dDocMargin * 2.0 + 0.5);   // 上下各一份
    const int iSingle = iLineH + iFrame + iMargin + iDocMargin;
    const int iMax    = iSingle + (a_iMaxLines - 1) * iLineH;
    const int iBlocks = a_pEdit->blockCount();   // 当前逻辑行数（≥1）
    int iH = iSingle + (iBlocks - 1) * iLineH;
    if (iH < iSingle) { iH = iSingle; }
    if (iH > iMax)    { iH = iMax; }
    a_pEdit->setFixedHeight(iH);
}

void CExecutionPanel::OnAdhocTextChanged() {
    // 助手输入框：复用通用高度自适应（#3），封顶 10 行。
    constexpr int k_iAdhocMaxLines = 10;
    AdjustInputHeight(m_pAdhocInput, k_iAdhocMaxLines);
    // 2026.08.19 运行中可输入追加指令：有字时按钮从「⏹ 停止」切「➤ 追加指令」，
    //   清空回落——按钮态跟随输入内容刷新。
    if (m_bAdhocRunning) {
        UpdateAdhocInputState();
    }
}

void CExecutionPanel::RebuildPromptList() {
    // FR-3：按 m_aAdhocBlocks 重建「用户提问」下标（旧→新），供半透明按钮下拉跳转。
    m_aPromptIdx.clear();
    for (int i = 0; i < m_aAdhocBlocks.size(); ++i) {
        if (m_aAdhocBlocks[i].eKind == CMessageBlock::Event &&
            m_aAdhocBlocks[i].sEventKind == QStringLiteral("user")) {
            m_aPromptIdx.append(i);
        }
    }
}

void CExecutionPanel::OnPromptPicked(int a_iBlockIdx) {
    // FR-3：点击下拉中某条提示词 → 把对应 User 提问块滚到助手日志最顶 + 横纵滚动条归零。
    if (m_pAdhocView == nullptr) {
        return;
    }
    if (a_iBlockIdx < 0 || a_iBlockIdx >= m_aAdhocWidgets.size()) {
        return;
    }
    CMessageBlock* pBlock = m_aAdhocWidgets[a_iBlockIdx];
    if (pBlock == nullptr) {
        return;
    }
    // 切到「助手」Tab，确保用户看到滚动结果。
    const int iIdx = m_pTabs->indexOf(m_pAdhocView);
    if (iIdx >= 0) {
        m_pTabs->setCurrentIndex(iIdx);
    }
    // 滚动到该块顶部：把内容 widget 内该块的 y（相对内容原点）设为垂直滚动条值。
    m_pAdhocView->ScrollBlockToTop(pBlock);
}

void CExecutionPanel::SyncStagesHeight(int a_iTaskListBottomY) {
    // FR-9：阶段表底部对齐左栏任务列表（Tab）底部（不含按钮行）。
    // 两栏在同一水平 splitter 内顶部对齐，故任务列表底部 y（相对左栏顶）减去本栏上 margin
    // 即为阶段表应有高度（阶段表顶部贴本栏内容区上沿）。
    // 2026.08.18 改走 splitter setSizes（弃 setFixedHeight——min==max 锁死手柄，阶段/日志
    //   中间的拖动条不可拖）：左栏变化时对齐优先（重设两段比例）；用户拖本栏手柄时左栏
    //   无 resize/信号，不会回写打架（手动偏移保持到下次左栏变化）。
    if (m_pStageSplit == nullptr || m_pStages == nullptr || a_iTaskListBottomY <= 0) {
        return;
    }
    const int iTopMargin = layout()->contentsMargins().top();
    const int iH = a_iTaskListBottomY - iTopMargin;
    if (iH <= 0 || iH == m_pStages->height()) {
        return;
    }
    // 可用高度 = splitter 高度 - 手柄（5px）；启动早期未布局完成时 splitter 高度不足则跳过，
    //   待下次 resize 再同步（与旧 setFixedHeight 的首帧行为一致）。
    const int iAvail = m_pStageSplit->height() - m_pStageSplit->handleWidth();
    if (iAvail <= iH) {
        return;
    }
    m_pStageSplit->setSizes({iH, iAvail - iH});
}

void CExecutionPanel::ShowFileTab(const QString& a_sAbsPath) {
    // 2026.08.18 资源管理器双击文本文件的内嵌预览：Tab 惰性创建（首次双击才加页，
    //   追加在「助手」右侧即末位），此后复用同一页换内容与标题（始终只有一个预览页）。
    const QFileInfo stInfo(a_sAbsPath);
    if (m_pFileView == nullptr) {
        m_pFileView = new QPlainTextEdit(m_pTabs);
        m_pFileView->setReadOnly(true);
        m_pFileView->setLineWrapMode(QPlainTextEdit::NoWrap);   // 长行横向滚动，编辑器习惯
        m_pFileView->setTabChangesFocus(true);                  // Tab 切焦点，不被文本吞键
        QFont fntFile = m_pFileView->font();
        fntFile.setFamily(QStringLiteral("Consolas"));
        fntFile.setPointSize(11);   // 2026.08.19 与全局 11pt 统一（#7）
        m_pFileView->setFont(fntFile);
        m_pTabs->addTab(m_pFileView, stInfo.fileName());
    } else {
        m_pTabs->setTabText(m_pTabs->indexOf(m_pFileView), stInfo.fileName());
    }
    // 读取：上限 4MB 超出截断提示；编码 UTF-8 → 报错回退 GB18030（中文环境覆盖
    //   GB2312/GBK）→ 均失败按 Latin1 兜底（不丢字节）。
    QString sText;
    QFile stFile(a_sAbsPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        sText = QStringLiteral("无法读取文件：\n%1").arg(a_sAbsPath);
    } else {
        constexpr qint64 kiMaxBytes = 4 * 1024 * 1024;
        QByteArray aBytes = stFile.read(kiMaxBytes + 1);
        const bool bTrunc = aBytes.size() > kiMaxBytes;
        if (bTrunc) {
            aBytes.truncate(kiMaxBytes);
        }
        QStringDecoder stUtf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        sText = stUtf8.decode(aBytes);
        if (stUtf8.hasError()) {
            QStringDecoder stGbk(QStringLiteral("GB18030"), QStringConverter::Flag::Stateless);
            if (stGbk.isValid()) {
                sText = stGbk.decode(aBytes);
            }
        }
        if (bTrunc) {
            sText += QStringLiteral("\n\n……（文件超过 4MB，已截断显示）");
        }
    }
    m_pFileView->setPlainText(sText);
    m_pTabs->setCurrentWidget(m_pFileView);
}

void CExecutionPanel::CreatePromptButton() {
    // FR-3：助手日志右上角半透明提示词按钮（24x24，默认 50% 透明，hover 全不透明）。
    m_pBtnPrompts = new QToolButton(m_pAdhocView);   // parent = 助手视图，叠在其右上角
    m_pBtnPrompts->setText(QStringLiteral("≡"));   // 列表图标占位
    m_pBtnPrompts->setFixedSize(24, 24);
    m_pBtnPrompts->setToolTip(QStringLiteral("历史提问（鼠标移上查看 / 点击跳转）"));
    m_pBtnPrompts->setAttribute(Qt::WA_ShowWithoutActivating);
    m_pBtnPrompts->setMouseTracking(true);
    QFont fBtn = m_pBtnPrompts->font();
    fBtn.setPointSize(11);
    m_pBtnPrompts->setFont(fBtn);
    // 默认 50% 透明；hover 时全不透明。QGraphicsOpacityEffect 作用于单个 widget。
    auto* pEff = new QGraphicsOpacityEffect(m_pBtnPrompts);
    pEff->setOpacity(0.5);
    m_pBtnPrompts->setGraphicsEffect(pEff);
    m_pBtnPrompts->show();
    m_pBtnPrompts->raise();

    // hover（进入/离开按钮）→ 显示/隐藏提示词下拉浮层。
    m_pBtnPrompts->installEventFilter(this);   // 拦 Enter/Leave
    // 助手视图 resize/show/hide → 按钮贴右上角 + 按当前 Tab 可见性显隐。
    if (m_pAdhocView != nullptr) {
        m_pAdhocView->installEventFilter(this);
    }
    // 切 Tab → 刷新按钮可见性（仅在助手 Tab 显示）。
    if (m_pTabs != nullptr) {
        connect(m_pTabs, &QTabWidget::currentChanged, this, [this](int) {
            if (m_pBtnPrompts != nullptr && m_pAdhocView != nullptr && m_pTabs != nullptr) {
                const bool bVis = (m_pTabs->currentWidget() == m_pAdhocView);
                m_pBtnPrompts->setVisible(bVis);
                if (bVis) {
                    // 右侧让出竖直滚动条（宽 10px）：按钮右沿距视图右边 11px，左移 5px 避免重叠。
                    m_pBtnPrompts->move(m_pAdhocView->width() - m_pBtnPrompts->width() - 11, 6);
                    m_pBtnPrompts->raise();
                }
            }
        });
    }
    // 弹出浮层：parent=this，点击回调 OnPromptPicked。
    m_pPromptFlyout = new CPromptFlyout(this, [this](int a_iIdx) {
        // 浮层存的是提问在 m_aPromptIdx 中的序号 → 取真实块下标。
        if (a_iIdx >= 0 && a_iIdx < m_aPromptIdx.size()) {
            OnPromptPicked(m_aPromptIdx.at(a_iIdx));
        }
        if (m_pPromptFlyout != nullptr) {
            m_pPromptFlyout->hide();
        }
    });

    // 浮层也参与 hover/leave：鼠标离开按钮但进入浮层时保持显示，离开浮层才收起。
    m_pPromptFlyout->installEventFilter(this);
}

void CExecutionPanel::OnPromptBtnHovered() {
    // FR-3：鼠标移到半透明按钮 → 全不透明 + 立刻显示提示词下拉浮层。
    if (m_pBtnPrompts == nullptr) {
        return;
    }
    if (auto* pEff = qobject_cast<QGraphicsOpacityEffect*>(m_pBtnPrompts->graphicsEffect())) {
        pEff->setOpacity(1.0);
    }
    if (m_pPromptFlyout == nullptr || m_aPromptIdx.isEmpty()) {
        return;   // 无提问时不弹
    }
    // 刷新浮层内容（提问可能已变化）。
    m_pPromptFlyout->Clear();
    for (int i = 0; i < m_aPromptIdx.size(); ++i) {
        const int iBlock = m_aPromptIdx.at(i);
        if (iBlock >= 0 && iBlock < m_aAdhocBlocks.size()) {
            m_pPromptFlyout->AddItem(i, m_aAdhocBlocks[iBlock].sText);
        }
    }
    // 定位浮层到按钮左下方（全局坐标）。
    const QPoint ptGlobal = m_pBtnPrompts->mapToGlobal(QPoint(0, m_pBtnPrompts->height()));
    m_pPromptFlyout->resize(m_pPromptFlyout->WidthHint(),
                            qMin(m_pPromptFlyout->Count() * 24 + 8, 280));
    m_pPromptFlyout->move(ptGlobal);
    m_pPromptFlyout->show();
    m_pPromptFlyout->raise();
}

void CExecutionPanel::OnPromptBtnLeaved() {
    // FR-3：鼠标离开按钮 → 恢复 50% 透明；浮层仅在鼠标也离开浮层时才收起（见 eventFilter）。
    if (m_pBtnPrompts == nullptr) {
        return;
    }
    if (auto* pEff = qobject_cast<QGraphicsOpacityEffect*>(m_pBtnPrompts->graphicsEffect())) {
        pEff->setOpacity(0.5);
    }
}

bool CExecutionPanel::eventFilter(QObject* a_pWatched, QEvent* a_pEvent) {
    // FR-3：半透明提示词按钮的 Enter/Leave → 显示/隐藏下拉浮层 + 透明度切换。
    if (a_pWatched == m_pBtnPrompts) {
        if (a_pEvent->type() == QEvent::Enter) {
            OnPromptBtnHovered();
        } else if (a_pEvent->type() == QEvent::Leave) {
            OnPromptBtnLeaved();
        }
    }
    // FR-3：鼠标离开浮层（既不在按钮也不在浮层）→ 收起浮层。
    if (a_pWatched == m_pPromptFlyout && a_pEvent->type() == QEvent::Leave) {
        if (m_pPromptFlyout != nullptr) {
            m_pPromptFlyout->hide();
        }
    }
    // FR-3：助手视图 resize/show → 半透明按钮贴到右上角；仅助手 Tab 可见时显示按钮。
    if (a_pWatched == m_pAdhocView &&
        (a_pEvent->type() == QEvent::Resize || a_pEvent->type() == QEvent::Show ||
         a_pEvent->type() == QEvent::Hide)) {
        if (m_pBtnPrompts != nullptr && m_pAdhocView != nullptr) {
            const bool bAdhocTab =
                (m_pTabs != nullptr && m_pTabs->currentWidget() == m_pAdhocView);
            const int iX = m_pAdhocView->width()  - m_pBtnPrompts->width()  - 11;   // 左移 5px 避让竖直滚动条
            const int iY = 6;
            m_pBtnPrompts->move(iX, iY);
            m_pBtnPrompts->setVisible(bAdhocTab);
            m_pBtnPrompts->raise();
        }
    }
    // #3 助手输入框：Ctrl+Enter 发送（Enter 保留默认换行）。
    if (a_pWatched == m_pAdhocInput && a_pEvent->type() == QEvent::KeyPress) {
        auto* pKey = static_cast<QKeyEvent*>(a_pEvent);
        const int iKey = pKey->key();
        if ((iKey == Qt::Key_Return || iKey == Qt::Key_Enter) &&
            (pKey->modifiers() & Qt::ControlModifier)) {
            DoAdhocSend();
            return true;   // 吃掉，避免再插入换行
        }
    }
    // FR-3 任务介入输入框：Ctrl+Enter 发送介入指令。
    if (a_pWatched == m_pTaskInput && a_pEvent->type() == QEvent::KeyPress) {
        auto* pKey = static_cast<QKeyEvent*>(a_pEvent);
        const int iKey = pKey->key();
        if ((iKey == Qt::Key_Return || iKey == Qt::Key_Enter) &&
            (pKey->modifiers() & Qt::ControlModifier)) {
            if (m_pBtnInject != nullptr) {
                emit m_pBtnInject->clicked();   // 复用按钮发送逻辑
            }
            return true;
        }
    }
    return QWidget::eventFilter(a_pWatched, a_pEvent);
}

// ===========================================================================
// 对话流持久化（#1 助手 / #6 任务）
// SBlockData 为本类私有嵌套，序列化逻辑内聚于此，不外露数据结构。
// ===========================================================================

QJsonObject CExecutionPanel::BlockToJson(const SBlockData& a_stData) {
    QJsonObject o;
    if (!a_stData.sViewId.isEmpty()) {
        o[QStringLiteral("view")] = a_stData.sViewId;   // 评审多视角标识（向后兼容：旧记录无此键）
    }
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            o[QStringLiteral("kind")] = QStringLiteral("assistant");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
        case CMessageBlock::ToolUse:
            o[QStringLiteral("kind")]   = QStringLiteral("tool_use");
            o[QStringLiteral("tool")]   = a_stData.sTool;
            o[QStringLiteral("input")]  = a_stData.sInput;
            o[QStringLiteral("result")] = a_stData.sResult;
            break;
        case CMessageBlock::Event:
            o[QStringLiteral("kind")]      = QStringLiteral("event");
            o[QStringLiteral("eventKind")] = a_stData.sEventKind;
            o[QStringLiteral("text")]      = a_stData.sText;
            break;
        case CMessageBlock::Error:
            o[QStringLiteral("kind")] = QStringLiteral("error");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
        case CMessageBlock::System:
        default:
            o[QStringLiteral("kind")] = QStringLiteral("system");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
    }
    return o;
}

CExecutionPanel::SBlockData CExecutionPanel::BlockFromJson(const QJsonObject& a_o) {
    SBlockData st;
    st.sViewId = a_o.value(QStringLiteral("view")).toString();
    const QString sKind = a_o.value(QStringLiteral("kind")).toString();
    if (sKind == QStringLiteral("assistant")) {
        st.eKind = CMessageBlock::Assistant;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    } else if (sKind == QStringLiteral("tool_use")) {
        st.eKind   = CMessageBlock::ToolUse;
        st.sTool   = a_o.value(QStringLiteral("tool")).toString();
        st.sInput  = a_o.value(QStringLiteral("input")).toString();
        st.sResult = a_o.value(QStringLiteral("result")).toString();
    } else if (sKind == QStringLiteral("event")) {
        st.eKind      = CMessageBlock::Event;
        st.sEventKind = a_o.value(QStringLiteral("eventKind")).toString();
        st.sText      = a_o.value(QStringLiteral("text")).toString();
    } else if (sKind == QStringLiteral("error")) {
        st.eKind = CMessageBlock::Error;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    } else {
        st.eKind = CMessageBlock::System;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    }
    return st;
}

QJsonArray CExecutionPanel::AdhocBlocksToJson() const {
    QJsonArray aArr;
    for (const SBlockData& st : m_aAdhocBlocks) {
        aArr.append(BlockToJson(st));
    }
    return aArr;
}

void CExecutionPanel::LoadAdhocBlocksFromJson(const QJsonArray& a_aBlocks) {
    m_aAdhocBlocks.clear();
    for (const QJsonValue& v : a_aBlocks) {
        if (v.isObject()) {
            m_aAdhocBlocks.append(BlockFromJson(v.toObject()));
        }
    }
    RebuildAdhocView();   // 按恢复数据重建助手 Tab 对话流
}

QJsonArray CExecutionPanel::TaskBlocksToJson(const QString& a_sTaskId) const {
    QJsonArray aArr;
    const QList<SBlockData> aBlocks = m_aBlocks.value(a_sTaskId);
    for (const SBlockData& st : aBlocks) {
        aArr.append(BlockToJson(st));
    }
    return aArr;
}

void CExecutionPanel::LoadTaskBlocksFromJson(const QString& a_sTaskId, const QJsonArray& a_aBlocks) {
    QList<SBlockData>& aBlocks = m_aBlocks[a_sTaskId];
    aBlocks.clear();
    for (const QJsonValue& v : a_aBlocks) {
        if (v.isObject()) {
            aBlocks.append(BlockFromJson(v.toObject()));
        }
    }
    if (a_sTaskId == m_sCurId) {
        RebuildView();   // 当前任务：重建视图呈现恢复的对话流
    }
}

void CExecutionPanel::ClearAllBlocks() {
    // 切工程：彻底清空内存对话流（任务 + 助手），避免新旧工程数据混淆。
    m_aBlocks.clear();
    m_aCurWidgets.clear();
    m_aAdhocBlocks.clear();
    m_aAdhocWidgets.clear();
    m_sCurId.clear();
    m_aDirtyTaskIds.clear();
    // 作废两条分帧重建的在途帧：任务侧清 m_aBlocks 后循环自然空转，助手侧是扁平列表、
    //   随后会被 LoadAssistantState 重新填充，必须靠代际号 +1 杀掉排队中的旧帧防串屏。
    m_bRebuilding = false;
    ++m_uRebuildGen;
    m_bAdhocRebuilding = false;
    ++m_uAdhocRebuildGen;
    if (m_pChatSaveTimer != nullptr) {
        m_pChatSaveTimer->stop();
    }
    m_pView->Clear();
    m_pAdhocView->Clear();
}

void CExecutionPanel::MarkTaskDirty(const QString& a_sTaskId) {
    if (a_sTaskId.isEmpty() || m_pChatSaveTimer == nullptr) {
        return;
    }
    m_aDirtyTaskIds.insert(a_sTaskId);
    if (!m_pChatSaveTimer->isActive()) {
        m_pChatSaveTimer->start(500);   // 500ms 去抖，合并 claude 高频输出
    }
}

void CExecutionPanel::FlushTaskChat() {
    // 去抖到期：逐个脏任务发信号，由 CMainWindow 序列化落盘 chat.json。
    const QSet<QString> aIds = m_aDirtyTaskIds;
    m_aDirtyTaskIds.clear();
    for (const QString& sId : aIds) {
        emit sigTaskChatChanged(sId);
    }
}
