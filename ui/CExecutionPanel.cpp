#include "CExecutionPanel.h"

#include <functional>
#include <utility>
#include <algorithm>

#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTabBar>
#include <QUuid>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
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
#include "ui/CFilePreviewWidget.h"
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
        // 2026.08.28 关掉水平滚动条：长条目超宽时不再出横向滚动条（它还会占用
        //   视口高度，与竖向滚动条级联——旧版「3 条出滚动条、8 条反而不出」的
        //   现象即此叠加）；超宽文本直接裁切，hover 有全文 tooltip 兜底。
        m_pList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    void Clear() { m_pList->clear(); m_iMaxItemW = 0; m_iRowH = 0; }
    // 追加一条提问：a_iIdx = 提问在面板 m_aPromptIdx 序号，a_sText 全文（存 UserRole+1 用于 tooltip）。
    void AddItem(int a_iIdx, const QString& a_sText) {
        const QString sView = a_sText.section(QLatin1Char('\n'), 0, 0);   // 单行预览（首行）
        const QString sDisp = QStringLiteral("%1  %2").arg(a_iIdx + 1).arg(sView.left(120));
        auto* pItem = new QListWidgetItem(sDisp, m_pList);
        pItem->setData(Qt::UserRole, a_iIdx);
        pItem->setData(Qt::UserRole + 1, a_sText);   // 全文，tooltip 用
        pItem->setToolTip(QString());   // 关闭系统 tooltip，改用即时 showText
        // 2026.08.28 用列表 sizeHintForIndex（delegate 真实尺寸，含样式 padding，
        //   随字体/DPI 走）测条目宽高；旧版 fontMetrics+40 估算不含样式内边距，
        //   偏窄时会触发裁切/滚动条级联。
        const QSize szItem = m_pList->sizeHintForIndex(
            m_pList->model()->index(m_pList->count() - 1, 0));
        m_iMaxItemW = qMax(m_iMaxItemW, szItem.width());
        m_iRowH     = qMax(m_iRowH, szItem.height());
    }
    int Count() const { return m_pList->count(); }
    // 2026.08.23 内容自适应宽度：提示词少/短 → 贴内容；默认上限翻倍到 520（原 260），
    //   下限保留原 260（避免只有一两个短词时浮层过窄）。
    //   2026.08.28 条宽改用 sizeHintForIndex 实测（含样式 padding），chrome 富余
    //   +24 覆盖列表边框/浮层边距/封顶时的竖向滚动条宽。
    int AdaptiveWidth() const {
        return qBound(m_iWidthHint, m_iMaxItemW + 24, m_iWidthHint * 2);
    }
    // 2026.08.28 内容自适应高度：行高×条数 + 列表边框×2 + 浮层布局边距，封顶
    //   maximumHeight(280)（超顶自然出竖向滚动条，多条时的预期行为）。
    //   旧版 Count()*24+8 硬编码行高——不随字体/DPI/样式走，行高略大时
    //   「3 条出滚动条」；叠加水平滚动条吃高又级联竖向滚动条，出现
    //   「3 条有滚动条、8 条反而没有」的矛盾现象。
    int AdaptiveHeight() const {
        const int iCount = m_pList->count();
        if (iCount <= 0 || m_iRowH <= 0) {
            return 48;   // 空态兜底（正常调用前有 m_aPromptIdx 非空守卫）
        }
        return qMin(m_iRowH * iCount + 2 * m_pList->frameWidth() + 2,
                    maximumHeight());
    }
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
    int          m_iMaxItemW  = 0;   // 本批条目最宽像素（sizeHintForIndex，Clear 复位）
    int          m_iRowH      = 0;   // 本批条目行高（同上，随字体/DPI/样式实测）
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
    m_pArtifactsPage = pOutSplit;   // Tab 布局管制（UpdateTabCloseButtons）识别该页用
    m_pArtifactsSplit = pOutSplit;  // 2026.08.29 持久化分割条位置（CMainWindow 调度）
    m_pTabs->addTab(pOutSplit, QStringLiteral("阶段产物"));
    connect(m_pOutputs, &QListWidget::currentRowChanged,
            this, &CExecutionPanel::OnOutputSelected);

    // —— 2026.08.22 多助手：助手对话流不再固定单 Tab，改为按助手动态增删 ——
    //   Tab 交互（可关闭/可拖动/「+」角标/双击改名）在此统一配置，助手 Tab 随后按需创建。
    SetupTabsInteraction();

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
        ShowContextMenu(m_pBtnContext->mapToGlobal(a_stPos));
    });
    // 2026.08.29 可发现性修复（用户实测找不到入口以为功能被取消）：左键单击进度条
    //   也弹同一菜单——进度条 setTextVisible(false)、无任何操作提示，纯右键入口
    //   对使用者不可见。QProgressBar 无 clicked 信号，走 eventFilter 左键释放分支。
    m_pBtnContext->installEventFilter(this);
    UpdateContextButton();   // 进度条初始态（占比 0、绿色、tooltip 出数值）
    m_pBtnUndo = new QPushButton(QStringLiteral("↶ 撤销"), this);
    m_pBtnUndo->setToolTip(QStringLiteral("撤销最后一条提问"));
    setPt11(m_pBtnUndo);
    m_pBtnSend = new QPushButton(QStringLiteral("发送"), this);
    setPt11(m_pBtnSend);
    pAdhocBar->addWidget(m_pAdhocInput, 1);
    pAdhocBar->addWidget(m_pBtnReadOnly);
    pAdhocBar->addWidget(m_pBtnContext);
    // 2026.08.23 布局调整：发送在前、↶ 回退在后（原顺序相反）。
    pAdhocBar->addWidget(m_pBtnSend);
    pAdhocBar->addWidget(m_pBtnUndo);
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
    // FR-3：提示词下拉浮层（单实例，各助手右上角半透明按钮共用；hover 弹出、点击跳转）。
    m_pPromptFlyout = new CPromptFlyout(this, [this](int a_iIdx) {
        // 浮层存的是提问在 m_aPromptIdx（激活助手）中的序号 → 取真实块下标。
        if (a_iIdx >= 0 && a_iIdx < m_aPromptIdx.size()) {
            OnPromptPicked(m_aPromptIdx.at(a_iIdx));
        }
        if (m_pPromptFlyout != nullptr) {
            m_pPromptFlyout->hide();
        }
    });
    m_pPromptFlyout->installEventFilter(this);   // 离开浮层才收起（见 eventFilter）
    // 2026.08.22 初始默认助手（首个助手，无存档时的空会话）。
    ResetAssistants();
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
    // 2026.08.27 本轮实时 token（Claude Code 式“当前指令消耗”）：仅运行中可见。
    m_pLblLive = new QLabel(this);
    m_pLblLive->hide();
    pCostBar->addWidget(m_pLblLive);
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

    // 2026.08.27 本轮实时计量限频：流式 usage 事件远快于肉眼可辨，UI 刷新限 200ms
    //   一次——事件只暂存最新值，定时器到点统一上屏（丢帧合并，数值不丢）。
    m_pLiveTimer = new QTimer(this);
    m_pLiveTimer->setSingleShot(true);
    m_pLiveTimer->setInterval(200);
    connect(m_pLiveTimer, &QTimer::timeout, this, [this]() {
        ApplyLiveUsage(m_iPendingLiveIn, m_iPendingLiveOut);
    });
}

void CExecutionPanel::UpdateCostLabel() {
    // 底部费用/状态栏：[费用|token|请求|状态|耗时]。
    // 2026.08.27 无任务时显示当前激活助手的独立流量记录（未选任务时助手消耗单独累计）。
    if (m_pCurTask == nullptr) {
        const SAsst* pAsst = ActiveAsst();
        if (pAsst == nullptr || (pAsst->dCost <= 0.0 && pAsst->iInTok == 0 &&
                                 pAsst->iOutTok == 0 && pAsst->iReqs == 0)) {
            m_pLblCost->setText(QStringLiteral("费用: $0.00 | 输入:0 输出:0 | 请求:0"));
            return;
        }
        const QString sCost = pAsst->dCost > 0.0
            ? QStringLiteral("$%1").arg(pAsst->dCost, 0, 'f', 2)
            : QStringLiteral("-");
        QString sText = QStringLiteral("费用: %1 | 输入:%2 输出:%3 | 请求:%4")
            .arg(sCost)
            .arg(FormatTokens(pAsst->iInTok))
            .arg(FormatTokens(pAsst->iOutTok))
            .arg(pAsst->iReqs);
        if (pAsst->iElapsedMs > 0) {
            sText += QStringLiteral(" | 耗时 %1").arg(FormatElapsed(pAsst->iElapsedMs));
        }
        m_pLblCost->setText(sText);
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

// 2026.08.27 本轮实时计量显示：任务阶段/助手轮次流式事件到达即暂存，限频 200ms 上屏一次。
void CExecutionPanel::SetLiveUsage(qint64 a_iIn, qint64 a_iOut) {
    m_iPendingLiveIn = a_iIn;
    m_iPendingLiveOut = a_iOut;
    if (m_pLiveTimer != nullptr && !m_pLiveTimer->isActive()) {
        m_pLiveTimer->start();
    }
}

// 实际上屏（限频定时器到点调用）：(0,0) 隐藏标签。
void CExecutionPanel::ApplyLiveUsage(qint64 a_iIn, qint64 a_iOut) {
    if (m_pLblLive == nullptr) {
        return;
    }
    if (a_iIn <= 0 && a_iOut <= 0) {
        m_pLblLive->hide();
        return;
    }
    m_pLblLive->setText(QStringLiteral("本轮 ↑%1 ↓%2")
                            .arg(QLocale().toString(a_iIn))
                            .arg(QLocale().toString(a_iOut)));
    m_pLblLive->show();
    m_pLblLive->raise();
}

// 2026.08.27 助手独立流量累计：未选任务时助手消耗归集到该助手自身（随档持久化）。
void CExecutionPanel::AddAsstUsage(const QString& a_sId, qint64 a_iIn, qint64 a_iOut,
                                   double a_dCost, qint64 a_iMs, int a_iReqs) {
    SAsst* p = AsstById(a_sId);
    if (p == nullptr) {
        return;
    }
    p->dCost      += a_dCost;
    p->iInTok     += a_iIn;
    p->iOutTok    += a_iOut;
    p->iReqs      += a_iReqs;
    p->iElapsedMs += a_iMs;
    // 当前费用栏展示的是该助手（无任务时）才即时刷新，避免覆盖任务侧显示。
    if (m_pCurTask == nullptr && (a_sId == m_sActiveAsst || a_sId == m_sRunningAsst)) {
        UpdateCostLabel();
    }
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
    // 2026.08.22 Agent 输出带时间前缀（用户提示词带日期+时间，见 DoAdhocSend）。
    const QString sBody = a_sViewId.isEmpty() ? a_sText
                          : QStringLiteral("【视角 %1】%2").arg(a_sViewId, a_sText);
    // 2026.08.22 距上个时间戳 <1 分钟不输出时间（StampPrefix）
    const QString sStamp = StampPrefix(a_pTask->Id(), /*a_bFullDate=*/false);
    st.sText = sStamp.isEmpty() ? sBody : QStringLiteral("[%1] %2").arg(sStamp, sBody);
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
    st.sText = WithLogTime(a_sTaskId, a_sLine);
    AppendData(a_sTaskId, st);
}

void CExecutionPanel::AppendEvent(const QString& a_sTaskId, const QString& a_sKind,
                                  const QString& a_sText) {
    SBlockData st;
    st.eKind      = CMessageBlock::Event;
    st.sEventKind = a_sKind;
    st.sText      = WithLogTime(a_sTaskId, a_sText);
    AppendData(a_sTaskId, st);
}

// 2026.08.22 日志行时间前缀：该任务日志流里已出现过日期（此前行带 yyyy-MM-dd）
//   → 只加 [HH:mm:ss]；否则（首行/全新会话）连日期一起 [yyyy-MM-dd HH:mm:ss]。
//   判定用 m_aDatedLogTasks 记忆 + 兼容从 chat.json 重载的旧流（扫首个块文本）。
// 2026.08.22 补充：与上一次已输出的时间戳间隔 <60s 时本行不带时间（见 StampPrefix）。
QString CExecutionPanel::StampPrefix(const QString& a_sKey, bool a_bFullDate) {
    const QDateTime tNow = QDateTime::currentDateTime();
    qint64 llLast = m_aLastStampSecs.value(a_sKey, 0);
    if (llLast == 0) {
        // 首次（含重启/重载）：从持久化块里回扫最后一个时间戳前缀，防止跨会话 1 分钟内重复盖时间
        static const QString kAdhoc = QStringLiteral("@@adhoc@@");   // 助手流哨兵键前缀（+助手 id）
        const SAsst* pAsst = a_sKey.startsWith(kAdhoc)
                             ? AsstById(a_sKey.mid(kAdhoc.length())) : nullptr;
        const QList<SBlockData> aBlocks = (pAsst != nullptr) ? pAsst->aBlocks
                                                             : m_aBlocks.value(a_sKey);
        static const QRegularExpression reFull(QStringLiteral("^\\[(\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})\\]"));
        static const QRegularExpression reTime(QStringLiteral("^\\[(\\d{2}:\\d{2}:\\d{2})\\]"));
        for (int i = aBlocks.size() - 1; i >= 0; --i) {
            const QString& s = aBlocks[i].sText;
            QRegularExpressionMatch m = reFull.match(s);
            QDateTime t;
            if (m.hasMatch()) {
                t = QDateTime::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            } else {
                m = reTime.match(s);
                if (m.hasMatch()) {
                    // 只有时间：按今天拼；若比当前还晚（跨零点）回退一天
                    t = QDateTime(tNow.date(), QTime::fromString(m.captured(1), QStringLiteral("HH:mm:ss")));
                    if (t.isValid() && t > tNow) t = t.addDays(-1);
                }
            }
            if (t.isValid()) { llLast = t.toSecsSinceEpoch(); break; }
        }
    }
    if (llLast > 0 && llLast + 60 > tNow.toSecsSinceEpoch()) {
        return QString();   // 距上个时间戳不足 1 分钟：不输出时间
    }
    m_aLastStampSecs.insert(a_sKey, tNow.toSecsSinceEpoch());
    return tNow.toString(a_bFullDate ? QStringLiteral("yyyy-MM-dd HH:mm:ss")
                                     : QStringLiteral("HH:mm:ss"));
}

QString CExecutionPanel::WithLogTime(const QString& a_sTaskId, const QString& a_sText) {
    if (!m_aDatedLogTasks.contains(a_sTaskId)) {
        // 旧会话重载：首个系统/事件块若已带日期前缀则视为已有日期
        const QList<SBlockData>& aBlocks = m_aBlocks.value(a_sTaskId);
        static const QRegularExpression reDate(QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2}"));
        bool bHasDate = false;
        for (const SBlockData& st : aBlocks) {
            if (reDate.match(st.sText).hasMatch()) { bHasDate = true; break; }
        }
        if (!bHasDate) {
            const QString sStamp = StampPrefix(a_sTaskId, /*a_bFullDate=*/true);
            if (sStamp.isEmpty()) {
                return a_sText;   // 距上个时间戳 <1 分钟：不盖时间（日期下次再补）
            }
            m_aDatedLogTasks.insert(a_sTaskId);   // 本次写入日期后，后续行只需时间
            return QStringLiteral("[%1] %2").arg(sStamp, a_sText);
        }
        m_aDatedLogTasks.insert(a_sTaskId);
    }
    const QString sStamp = StampPrefix(a_sTaskId, /*a_bFullDate=*/false);
    return sStamp.isEmpty() ? a_sText : QStringLiteral("[%1] %2").arg(sStamp, a_sText);
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
    // 2026.08.22 连续工具调用合并：并入上一张 ×N 折叠卡，否则新建
    if (!TryMergeToolWidget(m_aBlocks[a_sTaskId], m_aCurWidgets,
                            m_aBlocks[a_sTaskId].size() - 1, a_stData)) {
        CMessageBlock* pW = CreateWidget(a_stData);
        m_aCurWidgets.append(pW);
        m_pView->AddBlock(pW);
    }
}

// 2026.08.22 连续工具调用合并上屏：command/读写等工具常成串出现（数十条），
//   逐条渲染既刷屏又拖慢滚动；合并为一张折叠卡，标题显示首条摘要 + ×N，展开看全部。
bool CExecutionPanel::TryMergeToolWidget(const QList<SBlockData>& aBlocks,
                                         QList<CMessageBlock*>& aWidgets,
                                         int a_iNewIdx, const SBlockData& a_stData) {
    if (a_stData.eKind != CMessageBlock::ToolUse || a_iNewIdx <= 0) {
        return false;
    }
    const SBlockData& stPrev = aBlocks.at(a_iNewIdx - 1);
    if (stPrev.eKind != CMessageBlock::ToolUse || stPrev.sViewId != a_stData.sViewId) {
        return false;   // 前一块不是工具调用（或评审视角不同）：不合并
    }
    if (a_iNewIdx > aWidgets.size() || aWidgets.isEmpty()) {
        return false;   // widget 列表未跟上（异常防御）
    }
    CMessageBlock* pW = aWidgets.at(a_iNewIdx - 1);
    if (pW == nullptr || pW->ToolCount() <= 0) {
        return false;
    }
    pW->AddMergedTool(a_stData.sTool, a_stData.sInput);
    if (!a_stData.sResult.isEmpty()) {
        pW->SetResult(a_stData.sResult);
    }
    aWidgets.append(pW);   // 同一 widget 占多个块位：结果回填/提示词跳转的索引对齐仍成立
    return true;
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
        // 2026.08.22 连续工具调用合并：重载/切任务的历史流同样折叠
        if (!TryMergeToolWidget(aBlocks, m_aCurWidgets, m_iRebuildIdx, aBlocks.at(m_iRebuildIdx))) {
            CMessageBlock* pW = CreateWidget(aBlocks.at(m_iRebuildIdx));
            m_aCurWidgets.append(pW);
            m_pView->AddBlock(pW, /*a_bBatch=*/true);   // 批量：不逐块滚底
        }
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

// 2026.08.24 开始任务切「日志」页：LoadAssistantsFromJson 载档后会无条件激活第一个助手
// Tab（v1.0.234 只修了空档分支），任务启动后用户视角应是任务日志而非助手对话流。
QByteArray CExecutionPanel::ArtifactsSplitState() const {
    return m_pArtifactsSplit != nullptr ? m_pArtifactsSplit->saveState() : QByteArray();
}

void CExecutionPanel::RestoreArtifactsSplit(const QByteArray& a_stState) {
    if (m_pArtifactsSplit != nullptr && !a_stState.isEmpty()) {
        m_pArtifactsSplit->restoreState(a_stState);
    }
}

void CExecutionPanel::ShowTaskLogPage() {
    m_pTabs->setCurrentWidget(m_pView);   // m_pView=「日志」页（任务对话流）
}

void CExecutionPanel::ShowActiveAssistantPage() {
    // 2026.08.26 切到当前激活助手的 Tab：新建任务后用户下一步就是跟助手对话，
    //   直接落在助手日志页（2026.08.23 的「保持原页」不再适用，用户反馈不符合常理）。
    if (SAsst* pAsst = ActiveAsst()) {
        m_pTabs->setCurrentWidget(pAsst->pView);
    }
}

// ===========================================================================
// 提示词 agent（按「助手」分 Tab 的独立对话流，#1/#2/#3/#4）
// 2026.08.22 多助手：块数据/控件/分帧状态全部下沉到各 SAsst；运行中的事件路由到
//   正在执行的助手（m_sRunningAsst），空闲时路由到激活助手（m_sActiveAsst）。
// ===========================================================================

CExecutionPanel::SAsst* CExecutionPanel::AsstById(const QString& a_sId) {
    for (SAsst& st : m_aAssts) {
        if (st.sId == a_sId) {
            return &st;
        }
    }
    return nullptr;
}

const CExecutionPanel::SAsst* CExecutionPanel::AsstById(const QString& a_sId) const {
    for (const SAsst& st : m_aAssts) {
        if (st.sId == a_sId) {
            return &st;
        }
    }
    return nullptr;
}

CExecutionPanel::SAsst* CExecutionPanel::ActiveAsst() {
    return AsstById(m_sActiveAsst);
}

CExecutionPanel::SAsst* CExecutionPanel::RunningTargetAsst() {
    // 块归属目标：助手在跑 → 该助手（切 Tab 查看其他助手时日志不串流）；空闲 → 激活助手。
    if (!m_sRunningAsst.isEmpty()) {
        if (SAsst* p = AsstById(m_sRunningAsst)) {
            return p;
        }
    }
    return ActiveAsst();
}

QString CExecutionPanel::AsstStampKey(const QString& a_sId) {
    // 助手流时间戳哨兵键：@@adhoc@@ + 助手 id（StampPrefix 依前缀路由到对应助手块列表）。
    static const QString kAdhoc = QStringLiteral("@@adhoc@@");
    return kAdhoc + a_sId;
}

void CExecutionPanel::AppendAdhocBlock(const SBlockData& a_stData, const QString& a_sTargetId) {
    SAsst* pAsst = a_sTargetId.isEmpty() ? RunningTargetAsst() : AsstById(a_sTargetId);
    if (pAsst == nullptr) {
        return;   // 无助手（理论不达：始终保底一个助手）
    }
    pAsst->aBlocks.append(a_stData);
    // 分帧重建期间只入数据不建控件（镜像 AppendData 让路）：分帧循环读实时列表会按序补上，
    //   否则会同一块建两份控件。重建结束后的常规路径仍即时建块。
    if (!pAsst->bRebuilding) {
        // 2026.08.22 连续工具调用合并：助手流同样折叠为 ×N 卡
        if (!TryMergeToolWidget(pAsst->aBlocks, pAsst->aWidgets,
                                pAsst->aBlocks.size() - 1, a_stData)) {
            // 复用 CreateWidget（其内部以 m_pView 为 parent，AddBlock 会把块 reparent 进助手视图）。
            CMessageBlock* pW = CreateWidget(a_stData);
            pAsst->aWidgets.append(pW);
            pAsst->pView->AddBlock(pW);
        }
    }
    // FR-2：不再无条件 ScrollToBottom；滚动决策交给 CMessageView::AddBlock 内部 m_bFollow
    //        机制（与任务日志 Tab 行为一致，AC-2.1～2.4）。
    // FR-3：新增用户提问时刷新提示词下拉索引（供半透明按钮下拉跳转；仅归属激活助手时）。
    if (pAsst->sId == m_sActiveAsst &&
        a_stData.eKind == CMessageBlock::Event &&
        a_stData.sEventKind == QStringLiteral("user")) {
        RebuildPromptList();
    }
    emit sigAdhocBlocksChanged();   // #1 助手流变化 → 触发落盘 assistant.json
}

void CExecutionPanel::RebuildAdhocView() {
    // 按激活助手数据重建其视图（分帧渐进，与任务视图同款）：单助手历史可达数百块，
    //   同步一次建完会卡 UI；切助手/切工程时代际号作废在途旧帧。
    RebuildAsstView(m_sActiveAsst);
}

void CExecutionPanel::RebuildAsstView(const QString& a_sId) {
    // 2026.08.23 按指定助手重建（多助手各自独立控件；加载后逐个预建，切换 Tab 即时呈现）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || pAsst->bClosed || pAsst->pView == nullptr) {
        return;
    }
    pAsst->pView->ResetFollow();   // FR-4：重建等同「跳到最新」，强制跟随到底
    pAsst->pView->Clear();
    pAsst->aWidgets.clear();
    pAsst->iRebuildIdx = 0;
    ++pAsst->uGen;   // 作废仍在排队的旧帧（上次重建尚未跑完就切了助手）
    pAsst->bRebuilding = true;   // AppendAdhocBlock 新块交分帧循环按序创建，防重复上屏
    RebuildAdhocChunk(pAsst->sId, pAsst->uGen);
}

void CExecutionPanel::RebuildAdhocChunk(const QString& a_sId, quint32 a_uGen) {
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || a_uGen != pAsst->uGen) {
        return;   // 助手已删除 / 已被新的重建顶替（切助手/切工程），本帧作废
    }
    int iCreated = 0;
    while (pAsst->iRebuildIdx < pAsst->aBlocks.size() && iCreated < k_iRebuildChunk) {
        // 2026.08.22 连续工具调用合并：助手流历史重载同样折叠
        if (!TryMergeToolWidget(pAsst->aBlocks, pAsst->aWidgets,
                                pAsst->iRebuildIdx, pAsst->aBlocks.at(pAsst->iRebuildIdx))) {
            CMessageBlock* pW = CreateWidget(pAsst->aBlocks.at(pAsst->iRebuildIdx));
            pAsst->aWidgets.append(pW);
            pAsst->pView->AddBlock(pW, /*a_bBatch=*/true);   // 批量：不逐块滚底
        }
        ++pAsst->iRebuildIdx;
        ++iCreated;
    }
    if (pAsst->iRebuildIdx < pAsst->aBlocks.size()) {
        QTimer::singleShot(0, this, [this, a_sId, a_uGen]() { RebuildAdhocChunk(a_sId, a_uGen); });
        return;
    }
    pAsst->bRebuilding = false;
    pAsst->pView->ScrollToBottomRobust();   // 需求7：恢复完成后多帧滚到助手日志最底
    if (a_sId == m_sActiveAsst) {
        RebuildPromptList();   // FR-3 按恢复后的助手流重建提示词下拉索引
    }
}

void CExecutionPanel::OnAdhocAssistantText(const QString& a_sText) {
    SBlockData st;
    st.eKind = CMessageBlock::Assistant;
    // 2026.08.22 Agent 输出带时间前缀（用户提示词带日期+时间，见 DoAdhocSend）；
    //   距上个时间戳 <1 分钟不输出时间（StampPrefix）。哨兵键按助手 id 区分（多助手）。
    const QString sStamp = StampPrefix(AsstStampKey(RunningTargetAsst() ? RunningTargetAsst()->sId : QString()),
                                        /*a_bFullDate=*/false);
    st.sText = sStamp.isEmpty() ? a_sText : QStringLiteral("[%1] %2").arg(sStamp, a_sText);
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
    SAsst* pAsst = RunningTargetAsst();
    if (pAsst == nullptr) {
        return;
    }
    for (int i = pAsst->aBlocks.size() - 1; i >= 0; --i) {
        if (pAsst->aBlocks[i].eKind == CMessageBlock::ToolUse &&
            pAsst->aBlocks[i].sResult.isEmpty()) {
            pAsst->aBlocks[i].sResult = a_sResult;
            if (i < pAsst->aWidgets.size()) {
                pAsst->aWidgets[i]->SetResult(a_sResult);
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
    // 2026.08.22 多助手：撤销作用于激活助手的对话流（编排器撤销栈即激活助手会话）。
    SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr) {
        return;
    }
    // 从末尾回溯找最后一个「用户提问」块，连同其后所有回复/工具/事件块一并移除。
    int iCut = -1;
    for (int i = pAsst->aBlocks.size() - 1; i >= 0; --i) {
        if (pAsst->aBlocks[i].eKind == CMessageBlock::Event &&
            pAsst->aBlocks[i].sEventKind == QStringLiteral("user")) {
            iCut = i;
            break;
        }
    }
    if (iCut < 0) {
        return;   // 无提问块可撤销（编排器栈空时已 warn「没有可撤销的提问」，此处兜底）
    }
    while (pAsst->aBlocks.size() > iCut) {
        pAsst->aBlocks.removeLast();
    }
    RebuildAdhocView();
    emit sigAdhocBlocksChanged();   // #1 撤销后落盘
}

void CExecutionPanel::OnAdhocFinished() {
    SetAdhocRunning(false);   // 一次助手调用结束：恢复输入栏（受工程开关约束）
    // 2026.08.27 跨助手排队：延迟到本轮收尾链（含编排器对同会话排队追加的自动续发——
    //   续发会同步重置运行态）全部跑完后再消费队列，避免与同会话续发互踩；
    //   仍在运行（续发接管）则本次跳过，下一轮结束再触发。
    QTimer::singleShot(0, this, &CExecutionPanel::ProcessAdhocQueue);
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
    // 2026.08.22 多助手：占用快照记到归属助手（运行中→该助手；否则激活助手），切助手时还原。
    if (SAsst* pAsst = RunningTargetAsst()) {
        pAsst->iCtxFill = a_iFill;
    }
    UpdateContextButton();
}

void CExecutionPanel::ShowContextMenu(const QPoint& a_stGlobalPos) {
    // 2026.08.29 上下文进度条菜单（右键 customContextMenuRequested / 左键单击共用）：
    //   立即压缩（编排器「摘要→重播种」两步）+ 设置上下文窗口上限。
    if (m_pBtnContext == nullptr) {
        return;
    }
    QMenu stMenu(m_pBtnContext);
    QAction* pActCompact = stMenu.addAction(QStringLiteral("立即压缩上下文"));
    QAction* pActSet = stMenu.addAction(QStringLiteral("设置上下文大小…"));
    QAction* pSel = stMenu.exec(a_stGlobalPos);
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
    // 2026.08.29 可发现性：tooltip 注明菜单入口（此前从不提示，右键入口形同虚设）。
    sTip += QStringLiteral("\n点击或右键：立即压缩上下文 / 设置上下文大小");
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
    // 2026.08.29 运行态可视化（修「新助手无法发送」误判）：跨助手（激活≠运行）时
    //   输入框 placeholder 明示可排队发送——旧版空输入时按钮恒「⏹ 停止」且占位符
    //   与空闲态无异，用户以为无法发送（实际输入文字即变「⏳ 排队发送」）。
    const bool bCross = !m_sRunningAsst.isEmpty() && m_sActiveAsst != m_sRunningAsst;
    const SAsst* pRunAsst = AsstById(m_sRunningAsst);
    const QString sRunName = pRunAsst != nullptr
        ? pRunAsst->sName : QStringLiteral("另一个助手");
    if (m_bAdhocRunning && bCross) {
        m_pAdhocInput->setPlaceholderText(QStringLiteral(
            "%1 正在运行；此处输入指令将排队，待其结束后自动发送（Ctrl+Enter）")
            .arg(sRunName));
    } else if (m_bAdhocRunning) {
        m_pAdhocInput->setPlaceholderText(QStringLiteral(
            "向正在运行的助手追加指令（Ctrl+Enter 立即注入/排队续发；清空可停止）"));
    } else {
        m_pAdhocInput->setPlaceholderText(
            QStringLiteral("向助手输入指令（Ctrl+Enter 发送）"));
    }
    if (m_bAdhocRunning && bHasText) {
        // 2026.08.27 运行中发送分两路：激活==运行助手 →「➤ 追加指令」（注入运行轮，
        //   原语义）；激活是另一助手（切走/新建后再发）→「⏳ 排队发送」（入队，
        //   运行轮结束后自动发送到该助手，不再误入运行助手的会话）。
        m_pBtnSend->setText(bCross ? QStringLiteral("⏳ 排队发送")
                                   : QStringLiteral("➤ 追加指令"));
        m_pBtnSend->setToolTip(bCross
            ? QStringLiteral("当前另一个助手正在运行：本条指令将排队，待其结束后自动发送到当前助手")
            : QStringLiteral("向正在执行的助手轮追加指令（立即注入/排队续发）"));
        m_pBtnSend->setEnabled(true);
        m_pBtnSend->setStyleSheet(
            QStringLiteral("QPushButton{background:#2563eb;color:white;}"));
    } else if (m_bAdhocRunning) {
        m_pBtnSend->setText(QStringLiteral("⏹ 停止"));
        // 2026.08.29 跨助手时点停止的是「运行中」那个助手——tooltip 说清归属并提示
        //   排队通道，防误停/误以为无法发送。
        m_pBtnSend->setToolTip(bCross
            ? QStringLiteral("停止 %1 的当前轮；或在输入框输入指令，排队发送到当前助手")
                  .arg(sRunName)
            : QStringLiteral("停止当前助手轮"));
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
    // 切到激活助手 Tab，让用户看到注入结果。
    if (SAsst* pAsst = ActiveAsst()) {
        m_pTabs->setCurrentWidget(pAsst->pView);
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
    // 2026.08.22 多助手：运行态归属助手。显式置 true 但无运行助手（自动续发排队指令的
    //   广播路径）→ 归到激活助手；置 false → 清运行助手（下一轮发送时重新归属）。
    if (a_bRunning && m_sRunningAsst.isEmpty()) {
        m_sRunningAsst = m_sActiveAsst;
    }
    if (!a_bRunning) {
        m_sRunningAsst.clear();
    }
    // #4 助手 Tab 标签提示运行态（●运行中 / ⏳排队 / 空闲）：统一走 RefreshAsstTabTexts。
    RefreshAsstTabTexts();
    UpdateAdhocInputState();
    UpdateBusyIndicator();   // #4 助手运行态变化 → 刷新执行态指示器
}

void CExecutionPanel::ClearRunningAsst() {
    // 2026.08.22 结束态：主窗口在读取运行助手 id（回填会话）之后调用，随后
    //   OnAdhocFinished → SetAdhocRunning(false) 幂等清空。
    m_sRunningAsst.clear();
    RefreshAsstTabTexts();   // 仍有排队助手的保留 ⏳ 标记
}

bool CExecutionPanel::AsstQueued(const QString& a_sId) const {
    for (const SQueuedAdhoc& st : std::as_const(m_aAdhocQueue)) {
        if (st.sAsstId == a_sId) {
            return true;
        }
    }
    return false;
}

QString CExecutionPanel::AsstTabText(const SAsst& a_st) const {
    // Tab 标题三态：● 运行中（优先）> ⏳ 有排队指令 > 纯名字。
    if (m_bAdhocRunning && a_st.sId == m_sRunningAsst) {
        return a_st.sName + QStringLiteral(" ●");
    }
    if (AsstQueued(a_st.sId)) {
        return a_st.sName + QStringLiteral(" ⏳");
    }
    return a_st.sName;
}

void CExecutionPanel::RefreshAsstTabTexts() {
    for (const SAsst& st : std::as_const(m_aAssts)) {
        const int iIdx = m_pTabs->indexOf(st.pView);
        if (iIdx >= 0) {
            m_pTabs->setTabText(iIdx, AsstTabText(st));
        }
    }
}

void CExecutionPanel::QueueAdhocPrompt(SAsst* a_pAsst, const QString& a_sText, bool a_bReadOnly) {
    // 2026.08.27 跨助手排队：提示词归属激活助手但另一助手在跑——入队不发送。
    //   徽标入目标助手日志（ AppendAdhocBlock 直投 id，绕开运行中路由）；真正的 user 块
    //   在实际发送时（ProcessAdhocQueue）才上屏，保证与编排器撤销栈一一对应（排队中
    //   关闭/换档不会留下未发送的悬挂 user 块）。队列不持久化：重启后未发送的排队指令
    //   以徽标形式留痕，可手动重发。
    if (a_pAsst == nullptr || a_sText.trimmed().isEmpty()) {
        return;
    }
    m_aAdhocQueue.append({ a_pAsst->sId, a_sText, a_bReadOnly });
    const int iPos = m_aAdhocQueue.size();
    SBlockData stBadge;
    stBadge.eKind      = CMessageBlock::Event;
    stBadge.sEventKind = QStringLiteral("queued");
    stBadge.sText = QStringLiteral("已排队第 %1 位（当前助手运行结束后自动发送）：%2")
                        .arg(iPos)
                        .arg(a_sText.left(200));
    AppendAdhocBlock(stBadge, a_pAsst->sId);
    RefreshAsstTabTexts();   // 目标助手 Tab 标 ⏳
    UpdateAdhocInputState(); // 按钮文案可能已变（⏳ 排队发送）
}

void CExecutionPanel::ProcessAdhocQueue() {
    // 队列消费（OnAdhocFinished 经 singleShot(0) 延迟触发，避开收尾链重入）：
    //   仍在运行（编排器对同会话排队追加的自动续发已接管）→ 留到下一轮结束；
    //   队首助手已不存在（档切换随 ClearAsstTabs 清空，防御）→ 丢弃取下一个。
    if (m_bAdhocRunning || m_aAdhocQueue.isEmpty()) {
        return;
    }
    while (!m_aAdhocQueue.isEmpty()) {
        const SQueuedAdhoc stQ = m_aAdhocQueue.first();
        SAsst* pAsst = AsstById(stQ.sAsstId);
        if (pAsst == nullptr) {
            m_aAdhocQueue.removeFirst();   // 助手已随档重建消失：丢弃
            continue;
        }
        m_aAdhocQueue.removeFirst();
        if (pAsst->sId != m_sActiveAsst && pAsst->pView != nullptr) {
            // 切到目标助手 Tab：→ OnAsstTabCurrentChanged → sigAsstSwitched →
            //   主窗口 SyncOrchToPanel(旧)/LoadOrchFromPanel(新)，编排器换载该助手会话。
            m_pTabs->setCurrentWidget(pAsst->pView);
        }
        // 与 DoAdhocSend 空闲路径同款：user 块（带时间戳）+ 运行态 + 提交。
        SBlockData stUser;
        stUser.eKind      = CMessageBlock::Event;
        stUser.sEventKind = QStringLiteral("user");
        const QString sStamp = StampPrefix(AsstStampKey(pAsst->sId), /*a_bFullDate=*/true);
        stUser.sText = sStamp.isEmpty() ? stQ.sText : QStringLiteral("[%1] %2").arg(sStamp, stQ.sText);
        AppendAdhocBlock(stUser, pAsst->sId);
        m_pTabs->setCurrentWidget(pAsst->pView);   // #2 发送后切到该助手 Tab
        m_sRunningAsst = pAsst->sId;               // 本轮事件（回复/工具）路由到该助手
        SetAdhocRunning(true);
        emit sigAdhocSubmit(stQ.sText, stQ.bReadOnly);
        RefreshAsstTabTexts();   // ⏳ → ●
        return;   // 一次只发一条；本条结束后 OnAdhocFinished 会再触发消费下一条
    }
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
    // 2026.08.27 跨助手排队：运行中的是另一个助手（用户切到/新建了别的助手再发送）时，
    //   提示词属于激活助手——不再误注入运行助手的会话与日志，改为排队，运行轮结束后
    //   自动发送（QueueAdhocPrompt）。空闲但队列未清空（消费延迟窗口内的新发送）同样
    //   入队保 FIFO 顺序。运行中且激活==运行助手 → 维持原「➤ 追加指令」注入语义。
    const bool bRunOther = m_bAdhocRunning && !m_sRunningAsst.isEmpty()
                           && m_sActiveAsst != m_sRunningAsst;
    const bool bQueueBacklog = !m_bAdhocRunning && !m_aAdhocQueue.isEmpty();
    if ((bRunOther || bQueueBacklog) && ActiveAsst() != nullptr) {
        QueueAdhocPrompt(ActiveAsst(), sText, m_pBtnReadOnly->isChecked());
        m_pAdhocInput->clear();   // 清空 → textChanged → 高度回到单行
        return;
    }
    // 2026.08.22 多助手：本次提交归属「运行中助手」（追加指令场景），空闲时为激活助手。
    SAsst* pAsst = RunningTargetAsst();
    if (pAsst == nullptr) {
        return;
    }
    // 先本地渲染「用户提问」块（❯），与编排器撤销栈一一对应：撤销=移除本块及之后所有块。
    // 2026.08.22 用户提示词带日期+时间前缀（Agent 输出只带时间，见 OnAssistantText）；
    //   距上个时间戳 <1 分钟不输出时间（StampPrefix，键按助手 id 区分）。
    SBlockData stUser;
    stUser.eKind      = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    const QString sStamp = StampPrefix(AsstStampKey(pAsst->sId), /*a_bFullDate=*/true);
    stUser.sText = sStamp.isEmpty() ? sText : QStringLiteral("[%1] %2").arg(sStamp, sText);
    AppendAdhocBlock(stUser);
    // #2 发送后自动切到该助手 Tab，让用户立即看到日志。
    m_pTabs->setCurrentWidget(pAsst->pView);
    if (!m_bAdhocRunning) {
        m_sRunningAsst = pAsst->sId;   // 本轮事件（回复/工具）路由到该助手
    }
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

// 2026.08.25 判定助手是否“有用”：对话流中存在用户发送的提示词（Event/user 块）。
//   没有任何用户提示词的助手（只有系统/事件行或完全无内容）视为空助手。
bool CExecutionPanel::AsstHasUserPrompt(const SAsst& a_st) {
    return std::any_of(a_st.aBlocks.cbegin(), a_st.aBlocks.cend(),
                       [](const SBlockData& b) {
                           return b.eKind == CMessageBlock::Event &&
                                  b.sEventKind == QStringLiteral("user");
                       });
}

void CExecutionPanel::RebuildPromptList() {
    // FR-3：按激活助手流重建「用户提问」下标（旧→新），供半透明按钮下拉跳转。
    m_aPromptIdx.clear();
    const SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr) {
        return;
    }
    for (int i = 0; i < pAsst->aBlocks.size(); ++i) {
        if (pAsst->aBlocks[i].eKind == CMessageBlock::Event &&
            pAsst->aBlocks[i].sEventKind == QStringLiteral("user")) {
            m_aPromptIdx.append(i);
        }
    }
}

void CExecutionPanel::OnPromptPicked(int a_iBlockIdx) {
    // FR-3：点击下拉中某条提示词 → 把对应 User 提问块滚到助手日志最顶 + 横纵滚动条归零。
    SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pView == nullptr) {
        return;
    }
    if (a_iBlockIdx < 0 || a_iBlockIdx >= pAsst->aWidgets.size()) {
        return;
    }
    CMessageBlock* pBlock = pAsst->aWidgets[a_iBlockIdx];
    if (pBlock == nullptr) {
        return;
    }
    // 切到该助手 Tab，确保用户看到滚动结果。
    m_pTabs->setCurrentWidget(pAsst->pView);
    // 滚动到该块顶部：把内容 widget 内该块的 y（相对内容原点）设为垂直滚动条值。
    pAsst->pView->ScrollBlockToTop(pBlock);
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

void CExecutionPanel::ShowFileTab(const QString& a_sAbsPath, bool a_bSwitchTab) {
    // 2026.08.18 资源管理器双击文本文件的内嵌预览：Tab 惰性创建（首次双击才加页，
    //   追加在「助手」右侧即末位），此后复用同一页换内容与标题（始终只有一个预览页）。
    // 2026.08.25 预览控件升级为 CFilePreviewWidget：编码探测（BOM/UTF-8/GB18030/二进制
    //   启发/Latin1 兜底）、.md 带格式、.html 富文本、二进制十六进制双区视图。
    const QFileInfo stInfo(a_sAbsPath);
    if (m_pFileView == nullptr) {
        m_pFileView = new CFilePreviewWidget(m_pTabs);
        m_pTabs->addTab(m_pFileView, stInfo.fileName());
        UpdateTabCloseButtons();   // 2026.08.22 预览页不带「x」（仅助手页可关闭）
    } else {
        m_pTabs->setTabText(m_pTabs->indexOf(m_pFileView), stInfo.fileName());
    }
    m_pFileView->ShowFile(a_sAbsPath);
    // 2026.08.21 单击预览：只加载内容/更新标题不切页（a_bSwitchTab=false）；
    //   双击（默认 true）保持原行为切到预览页。
    if (a_bSwitchTab) {
        m_pTabs->setCurrentWidget(m_pFileView);
    }
}

void CExecutionPanel::CreatePromptButton(SAsst& a_stAsst) {
    // FR-3：助手日志右上角半透明提示词按钮（24x24，默认 50% 透明，hover 全不透明）。
    //   2026.08.22 多助手：按钮 parent=该助手视图，随视图显隐；浮层单实例共用。
    QToolButton* pBtn = new QToolButton(a_stAsst.pView);   // parent = 助手视图，叠在其右上角
    a_stAsst.pBtnPrompts = pBtn;
    pBtn->setText(QStringLiteral("≡"));   // 列表图标占位
    pBtn->setFixedSize(24, 24);
    pBtn->setToolTip(QStringLiteral("历史提问（鼠标移上查看 / 点击跳转）"));
    pBtn->setAttribute(Qt::WA_ShowWithoutActivating);
    pBtn->setMouseTracking(true);
    QFont fBtn = pBtn->font();
    fBtn.setPointSize(11);
    pBtn->setFont(fBtn);
    // 默认 50% 透明；hover 时全不透明。QGraphicsOpacityEffect 作用于单个 widget。
    auto* pEff = new QGraphicsOpacityEffect(pBtn);
    pEff->setOpacity(0.5);
    pBtn->setGraphicsEffect(pEff);
    pBtn->hide();   // 仅该助手 Tab 可见时显示（currentChanged/resize 统一刷）
    // hover（进入/离开按钮）→ 显示/隐藏提示词下拉浮层；视图 resize/show/hide → 按钮贴右上角。
    pBtn->installEventFilter(this);
    // 2026.08.23 提示词按钮正下方补一个「↓」按钮：点击将该助手日志滚动到最底。
    QToolButton* pBtnBottom = new QToolButton(a_stAsst.pView);   // parent 同助手视图
    a_stAsst.pBtnBottom = pBtnBottom;
    pBtnBottom->setText(QStringLiteral("↓"));
    pBtnBottom->setFixedSize(24, 24);
    pBtnBottom->setToolTip(QStringLiteral("滚动到底部"));
    pBtnBottom->setAttribute(Qt::WA_ShowWithoutActivating);
    QFont fBtnB = pBtnBottom->font();
    fBtnB.setPointSize(11);
    pBtnBottom->setFont(fBtnB);
    auto* pEffB = new QGraphicsOpacityEffect(pBtnBottom);
    pEffB->setOpacity(0.5);
    pBtnBottom->setGraphicsEffect(pEffB);
    pBtnBottom->hide();   // 可见性/位置随提示词按钮统一刷
    // hover 半透明→不透明；点击滚到该助手日志最底（多帧确保布局同步后真正到底）。
    pBtnBottom->installEventFilter(this);
    const QString sAsstId = a_stAsst.sId;
    connect(pBtnBottom, &QToolButton::clicked, this, [this, sAsstId]() {
        SAsst* pA = AsstById(sAsstId);
        if (pA != nullptr && pA->pView != nullptr) {
            pA->pView->ScrollToBottomRobust();
        }
    });
    a_stAsst.pView->installEventFilter(this);
}

void CExecutionPanel::OnPromptBtnHovered() {
    // FR-3：鼠标移到半透明按钮 → 全不透明 + 立刻显示提示词下拉浮层（内容取激活助手流）。
    const SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pBtnPrompts == nullptr) {
        return;
    }
    if (auto* pEff = qobject_cast<QGraphicsOpacityEffect*>(pAsst->pBtnPrompts->graphicsEffect())) {
        pEff->setOpacity(1.0);
    }
    if (m_pPromptFlyout == nullptr || m_aPromptIdx.isEmpty()) {
        return;   // 无提问时不弹
    }
    // 刷新浮层内容（提问可能已变化）。
    m_pPromptFlyout->Clear();
    // 2026.08.22 浮层条目去掉时间戳前缀（[yyyy-MM-dd HH:mm:ss] / [HH:mm:ss]），只显示提问内容
    static const QRegularExpression k_reStamp(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\]\\s*"
                       "|^\\[\\d{2}:\\d{2}:\\d{2}\\]\\s*"));
    for (int i = 0; i < m_aPromptIdx.size(); ++i) {
        const int iBlock = m_aPromptIdx.at(i);
        if (iBlock >= 0 && iBlock < pAsst->aBlocks.size()) {
            QString sPrompt = pAsst->aBlocks[iBlock].sText;   // 拷贝后去时间戳（pAsst 为 const）
            m_pPromptFlyout->AddItem(i, sPrompt.remove(k_reStamp));
        }
    }
    // 2026.08.23 浮层自适应宽度（默认上限翻倍到 520；条目少/短时贴内容收缩，下限 260），
    //   右沿仍对齐主窗口右沿（旧版贴按钮左下，长提问会把浮层顶宽出界）。
    //   2026.08.28 高度同步自适应：行高实测（sizeHintForIndex，随字体/DPI），
    //   不再硬编码 24px；水平滚动条已关闭，不再出现裁切级联出的竖向滚动条。
    m_pPromptFlyout->resize(m_pPromptFlyout->AdaptiveWidth(),
                            m_pPromptFlyout->AdaptiveHeight());
    const QPoint ptBelow = pAsst->pBtnPrompts->mapToGlobal(
        QPoint(0, pAsst->pBtnPrompts->height() + 4));
    const QWidget* pWin = window();
    const int iRightX = (pWin != nullptr)
        ? pWin->mapToGlobal(QPoint(pWin->width(), 0)).x() : ptBelow.x();
    m_pPromptFlyout->move(iRightX - m_pPromptFlyout->width(), ptBelow.y());
    m_pPromptFlyout->show();
    m_pPromptFlyout->raise();
}

void CExecutionPanel::OnPromptBtnLeaved() {
    // FR-3：鼠标离开按钮 → 恢复 50% 透明；浮层仅在鼠标也离开浮层时才收起（见 eventFilter）。
    const SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pBtnPrompts == nullptr) {
        return;
    }
    if (auto* pEff = qobject_cast<QGraphicsOpacityEffect*>(pAsst->pBtnPrompts->graphicsEffect())) {
        pEff->setOpacity(0.5);
    }
}

bool CExecutionPanel::eventFilter(QObject* a_pWatched, QEvent* a_pEvent) {
    // 2026.08.29 上下文进度条左键单击 → 弹压缩/设置菜单（与右键同一份；可发现性）。
    if (a_pWatched == m_pBtnContext
            && a_pEvent->type() == QEvent::MouseButtonRelease) {
        auto* pEv = static_cast<QMouseEvent*>(a_pEvent);
        if (pEv->button() == Qt::LeftButton) {
            ShowContextMenu(m_pBtnContext->mapToGlobal(
                QPoint(0, m_pBtnContext->height())));
            return true;
        }
    }
    // 2026.08.24 滚轮管制：Tab 条滚轮切换页不得落入「+」占位页（该页被选中即
    // 触发新增助手，滚轮浏览时误增）——落到「+」时跳过它继续到下一真实页
    // （无下一页则原地不动）。新增助手唯一入口 = 点击「+」页本身。
    if (m_pTabs != nullptr && a_pWatched == m_pTabs->tabBar()
            && a_pEvent->type() == QEvent::Wheel) {
        auto* pWheel = static_cast<QWheelEvent*>(a_pEvent);
        const int iDir = (pWheel->angleDelta().y() < 0) ? 1 : -1;   // 下滚 = 下一个 Tab
        const int iCur = m_pTabs->currentIndex();
        int iNext = iCur + iDir;
        while (iNext >= 0 && iNext < m_pTabs->count()
               && m_pTabs->widget(iNext) == m_pAddTabPage) {
            iNext += iDir;   // 跳过「+」占位页
        }
        if (iNext != iCur && iNext >= 0 && iNext < m_pTabs->count()) {
            m_pTabs->setCurrentIndex(iNext);
        }
        return true;   // 消费原生滚轮切换，防 Qt 自己把当前页落到「+」页
    }
    // FR-3：半透明提示词按钮的 Enter/Leave → 显示/隐藏下拉浮层 + 透明度切换（各助手按钮统一）。
    bool bIsPromptBtn = false;
    SAsst* pBtnAsst = nullptr;
    for (SAsst& st : m_aAssts) {
        if (a_pWatched == st.pBtnPrompts) {
            bIsPromptBtn = true;
            pBtnAsst = &st;
            break;
        }
    }
    if (bIsPromptBtn) {
        if (a_pEvent->type() == QEvent::Enter) {
            // 按钮属于的助手即当前 Tab（仅当前 Tab 按钮可见）→ 以其为准弹出。
            if (pBtnAsst != nullptr && pBtnAsst->sId != m_sActiveAsst) {
                m_sActiveAsst = pBtnAsst->sId;   // 防御：悬停按钮的助手必为当前页
            }
            OnPromptBtnHovered();
        } else if (a_pEvent->type() == QEvent::Leave) {
            OnPromptBtnLeaved();
        }
    }
    // 「↓」按钮：hover 半透明→不透明（同提示词按钮观感，不弹浮层）。
    if (a_pEvent->type() == QEvent::Enter || a_pEvent->type() == QEvent::Leave) {
        for (SAsst& st : m_aAssts) {
            if (a_pWatched == st.pBtnBottom && st.pBtnBottom != nullptr) {
                if (auto* pEffB = qobject_cast<QGraphicsOpacityEffect*>(
                        st.pBtnBottom->graphicsEffect())) {
                    pEffB->setOpacity(a_pEvent->type() == QEvent::Enter ? 1.0 : 0.5);
                }
                break;
            }
        }
    }
    // FR-3：鼠标离开浮层（既不在按钮也不在浮层）→ 收起浮层。
    if (a_pWatched == m_pPromptFlyout && a_pEvent->type() == QEvent::Leave) {
        if (m_pPromptFlyout != nullptr) {
            m_pPromptFlyout->hide();
        }
    }
    // FR-3：助手视图 resize/show/hide → 半透明按钮贴到右上角；仅该助手 Tab 可见时显示按钮。
    if (a_pEvent->type() == QEvent::Resize || a_pEvent->type() == QEvent::Show ||
        a_pEvent->type() == QEvent::Hide) {
        for (SAsst& st : m_aAssts) {
            if (a_pWatched == st.pView && st.pBtnPrompts != nullptr) {
                const bool bCurTab =
                    (m_pTabs != nullptr && m_pTabs->currentWidget() == st.pView);
                const int iX = st.pView->width() - st.pBtnPrompts->width() - 11;   // 左移 5px 避让竖直滚动条
                const int iY = 6;
                st.pBtnPrompts->move(iX, iY);
                st.pBtnPrompts->setVisible(bCurTab && a_pEvent->type() != QEvent::Hide);
                st.pBtnPrompts->raise();
                if (st.pBtnBottom != nullptr) {
                    st.pBtnBottom->move(iX, iY + st.pBtnPrompts->height() + 2);   // 紧贴提示词按钮下方
                    st.pBtnBottom->setVisible(bCurTab && a_pEvent->type() != QEvent::Hide);
                    st.pBtnBottom->raise();
                }
                break;
            }
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

// 2026.08.22 多助手持久化：全部助手（含各自会话态快照）→ JSON 数组。
QJsonArray CExecutionPanel::AssistantsToJson() const {
    QJsonArray aArr;
    for (const SAsst& st : m_aAssts) {
        QJsonObject o;
        o[QStringLiteral("id")]        = st.sId;
        o[QStringLiteral("name")]      = st.sName;
        o[QStringLiteral("sid")]       = st.sSid;
        o[QStringLiteral("guideHash")] = st.sGuideHash;
        o[QStringLiteral("stack")]     = st.aStack;
        o[QStringLiteral("ctxFill")]   = static_cast<double>(st.iCtxFill);
        o[QStringLiteral("closed")]    = st.bClosed;   // 2026.08.23 已关闭（可从「▾」菜单恢复）
        // 2026.08.27 助手独立流量记录（未选任务时累计，随档持久化）
        QJsonObject oUsage;
        oUsage[QStringLiteral("cost")]   = st.dCost;
        oUsage[QStringLiteral("in")]     = static_cast<double>(st.iInTok);
        oUsage[QStringLiteral("out")]    = static_cast<double>(st.iOutTok);
        oUsage[QStringLiteral("reqs")]   = st.iReqs;
        oUsage[QStringLiteral("ms")]     = static_cast<double>(st.iElapsedMs);
        o[QStringLiteral("usage")]     = oUsage;
        QJsonArray aBlocks;
        for (const SBlockData& stB : st.aBlocks) {
            aBlocks.append(BlockToJson(stB));
        }
        o[QStringLiteral("blocks")] = aBlocks;
        aArr.append(o);
    }
    return aArr;
}

void CExecutionPanel::LoadAssistantsFromJson(const QJsonArray& a_aAssts, const QString& a_sActiveId) {
    // 恢复：清掉现有助手 Tab，按数组逐个建助手 + Tab；激活 a_sActiveId（缺省第一个）。
    //   非激活助手的视图惰性重建（切到该 Tab 时 RebuildAdhocView 按数据补建控件）。
    //   用 ClearAsstTabs（不建默认助手）——旧实现走 ResetAssistants 会多出一个空「助手1」，
    //   每次点任务切档累积一个，档文件随之越长越大。
    ClearAsstTabs();
    bool bSetActive = false;
    QString sFirstId;
    for (const QJsonValue& v : a_aAssts) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        SAsst st;
        st.sId = o.value(QStringLiteral("id")).toString();
        if (st.sId.isEmpty()) {
            st.sId = QUuid::createUuid().toString(QUuid::WithoutBraces);   // 旧档缺 id 兜底
        }
        st.sName = o.value(QStringLiteral("name")).toString();
        if (st.sName.isEmpty()) {
            st.sName = QStringLiteral("助手1");
        }
        st.bClosed    = o.value(QStringLiteral("closed")).toBool(false);
        st.sSid       = o.value(QStringLiteral("sid")).toString();
        st.sGuideHash = o.value(QStringLiteral("guideHash")).toString();
        st.aStack     = o.value(QStringLiteral("stack")).toArray();
        st.iCtxFill   = static_cast<qint64>(o.value(QStringLiteral("ctxFill")).toDouble());
        // 2026.08.27 助手独立流量记录恢复（旧档无 usage 键 → 全 0，兼容）
        const QJsonObject oUsage = o.value(QStringLiteral("usage")).toObject();
        st.dCost      = oUsage.value(QStringLiteral("cost")).toDouble();
        st.iInTok     = static_cast<qint64>(oUsage.value(QStringLiteral("in")).toDouble());
        st.iOutTok    = static_cast<qint64>(oUsage.value(QStringLiteral("out")).toDouble());
        st.iReqs      = oUsage.value(QStringLiteral("reqs")).toInt();
        st.iElapsedMs = static_cast<qint64>(oUsage.value(QStringLiteral("ms")).toDouble());
        const QJsonArray aBlocks = o.value(QStringLiteral("blocks")).toArray();
        for (const QJsonValue& vb : aBlocks) {
            if (vb.isObject()) {
                st.aBlocks.append(BlockFromJson(vb.toObject()));
            }
        }
        if (sFirstId.isEmpty() && !st.bClosed) {
            sFirstId = st.sId;
        }
        if (st.sId == a_sActiveId && !st.bClosed) {
            bSetActive = true;
        }
        if (!st.bClosed) {
            // 先入列表再建 Tab：CreateAsstTab 内的 UpdateTabCloseButtons 需能按视图
            // 反查到该助手（否则新 Tab 一度被当未知页，布局整队错位）。
            m_aAssts.append(st);
            CreateAsstTab(m_aAssts.last());   // 已关闭的：数据入列表但不再建 Tab（「▾」菜单可恢复）
        } else {
            m_aAssts.append(st);   // 注意：SAsst 按值入列，后续经 AsstById 取址操作
        }
    }
    if (m_aAssts.isEmpty() || sFirstId.isEmpty()) {
        ResetAssistants();   // 空数组/全关闭兜底：保持至少一个打开的助手
        return;
    }
    UpdateTabCloseButtons();
    // 激活指定助手（无指定/不存在/已关闭 → 第一个打开的，满足「旧任务打开第一个助手显示日志」）。
    const QString sAct = (bSetActive ? a_sActiveId : sFirstId);
    const SAsst* pAct = AsstById(sAct);
    if (pAct != nullptr) {
        m_pTabs->setCurrentWidget(pAct->pView);   // → currentChanged → OnAsstTabCurrentChanged
    }
    // 2026.08.23 各助手独立日志控件：加载后对全部打开助手预建视图（分帧渐进，不卡 UI），
    //   切换 Tab 即时呈现，不再依赖首次切入时的同步重建（此前切换数百块历史会明显卡顿）。
    for (const SAsst& st : m_aAssts) {
        if (!st.bClosed && !st.aBlocks.isEmpty()) {
            RebuildAsstView(st.sId);
        }
    }
}

void CExecutionPanel::UpdateAsstSession(const QString& a_sId, const QString& a_sSid,
                                        const QString& a_sGuideHash, const QJsonArray& a_aStack) {
    if (SAsst* p = AsstById(a_sId)) {
        p->sSid = a_sSid;
        p->sGuideHash = a_sGuideHash;
        p->aStack = a_aStack;
    }
}

QString CExecutionPanel::AsstSid(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return p != nullptr ? p->sSid : QString();
}

QJsonArray CExecutionPanel::AsstStack(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return p != nullptr ? p->aStack : QJsonArray();
}

QString CExecutionPanel::AsstGuideHash(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return p != nullptr ? p->sGuideHash : QString();
}

qint64 CExecutionPanel::AsstCtxFill(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return p != nullptr ? p->iCtxFill : 0;
}

void CExecutionPanel::ResetAssistants() {
    // 复位为单个默认空助手「助手1」（无档显式清空路径的兜底基底）。
    // 2026.08.23 新建任务不跳「助手日志」：复位只重建助手 Tab，不改用户正在看的页——
    //   复位前记住当前页：原本就在某助手 Tab（该 Tab 已被拆）才落到新建的「助手1」；
    //   原本在「日志/阶段产物/文件预览」等任务侧页面 → 保持原页不动（新建任务后停在日志页）。
    QWidget* pKeep = m_pTabs->currentWidget();
    const bool bWasAsst = (pKeep != nullptr) && (AsstByView(pKeep) != nullptr);
    ClearAsstTabs();
    SAsst st;
    st.sId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    st.sName = QStringLiteral("助手1");
    m_aAssts.append(st);
    CreateAsstTab(m_aAssts.last());
    m_sActiveAsst = st.sId;
    UpdateTabCloseButtons();
    if (bWasAsst || m_pTabs->currentWidget() == nullptr) {
        m_pTabs->setCurrentWidget(AsstById(m_sActiveAsst)->pView);
    }
    RebuildPromptList();
}

void CExecutionPanel::ClearAsstTabs() {
    // 拆除全部助手 Tab 并清模型（不动任务侧数据）。供 ResetAssistants（随后补默认助手）
    // 与 LoadAssistantsFromJson（随后按档重建，不应混入默认助手）共用。
    // 再入防护：拆 Tab 期间 QTabWidget 的 currentChanged 一律忽略——否则摘除当前助手页时
    // Qt 自动把相邻「+」占位页设为当前，被 OnAsstTabCurrentChanged 当成点「+」新增助手，
    // 新助手又 append 进正被拆除的 m_aAssts（清一个生一个 → 死循环/迭代器失效，
    // 即「点击任务卡死」的根因）。
    m_bAsstTabsResetting = true;
    for (int i = m_aAssts.size() - 1; i >= 0; --i) {
        if (m_aAssts[i].pView != nullptr) {
            RemoveAsstTab(m_aAssts[i]);
        }
    }
    m_aAssts.clear();
    m_sActiveAsst.clear();
    m_sRunningAsst.clear();
    m_aPromptIdx.clear();
    m_aAdhocQueue.clear();   // 2026.08.27 排队指令随档作废（目标助手 id 已失效；徽标留痕在旧档）
    m_bAsstTabsResetting = false;
    UpdateTabCloseButtons();   // 无助手页 → 顺手摘除「+」占位页
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
    // 切工程：彻底清空内存对话流（任务 + 各助手），避免新旧工程数据混淆。
    m_aBlocks.clear();
    m_aCurWidgets.clear();
    m_sCurId.clear();
    m_aDirtyTaskIds.clear();
    // 作废任务侧分帧重建的在途帧；助手侧整体复位（各 SAsst 连同视图/分帧状态一并重建）。
    m_bRebuilding = false;
    ++m_uRebuildGen;
    if (m_pChatSaveTimer != nullptr) {
        m_pChatSaveTimer->stop();
    }
    m_pView->Clear();
    ResetAssistants();
}

// ===========================================================================
// 2026.08.22 多助手 Tab 管理：可关闭（右上角 x）/ 可拖动排序 /「+」新增 / 双击改名
// ===========================================================================

void CExecutionPanel::SetupTabsInteraction() {
    if (m_pTabs == nullptr) {
        return;
    }
    m_pTabs->setMovable(true);      // 拖动 Tab 换位（含日志/产物页；助手顺序随 Tab 相对顺序同步）
    // 2026.08.23 按钮布局对齐 DSHAgentManage：助手页关闭按钮用 QTabWidget 原生「x」
    //   （tabsClosable + tabCloseRequested，位置/热区/悬停效果与系统一致）；
    //   「+」是一个真实占位 Tab（钉在最后助手 Tab 之后，选中即新增并弹回）；
    //   「▾」放 QTabWidget 右上角角落（cornerWidget），不再叠在 Tab 标签内。
    m_pTabs->setTabsClosable(true);
    connect(m_pTabs, &QTabWidget::tabCloseRequested,
            this, &CExecutionPanel::OnAsstTabCloseRequested);
    // 「+」页（隐藏自身关闭按钮；插入时机见 UpdateTabCloseButtons）。
    m_pAddTabPage = new QWidget(this);
    m_pAddTabPage->setToolTip(QStringLiteral("新增助手（同一任务内多个并行助手，各占一个日志 Tab）"));
    // 「▾」角落按钮：弹出已关闭助手菜单。
    auto* pBtnClosed = new QToolButton(m_pTabs);
    pBtnClosed->setText(QStringLiteral("▾"));
    pBtnClosed->setToolTip(QStringLiteral("已关闭的助手（点击重新打开）"));
    pBtnClosed->setAutoRaise(true);
    connect(pBtnClosed, &QToolButton::clicked,
            this, &CExecutionPanel::OnAsstMenuClicked);
    m_pTabs->setCornerWidget(pBtnClosed, Qt::TopRightCorner);
    // 拖动换位：同步 m_aAssts 顺序到 Tab 相对顺序并落盘。
    connect(m_pTabs->tabBar(), &QTabBar::tabMoved,
            this, &CExecutionPanel::OnAsstTabMoved);
    // 双击助手 Tab → 重命名（Qt 6.8：QTabBar::tabBarDoubleClicked）。
    connect(m_pTabs->tabBar(), &QTabBar::tabBarDoubleClicked,
            this, &CExecutionPanel::OnAsstTabRenamed);
    // 切 Tab：激活助手切换（切到非助手页不改激活；切到「+」页=新增）。
    connect(m_pTabs, &QTabWidget::currentChanged,
            this, &CExecutionPanel::OnAsstTabCurrentChanged);
    // 2026.08.24 滚轮不新增助手：拦截 Tab 条滚轮，切换页跳过「+」占位页（见 eventFilter）。
    m_pTabs->tabBar()->installEventFilter(this);
    UpdateTabCloseButtons();
}

void CExecutionPanel::UpdateTabCloseButtons() {
    // ① 非助手页（日志/阶段产物/文件预览）与「+」页隐藏原生关闭按钮；
    // ② Tab 布局管制：固定分区顺序 [日志][阶段产物][打开助手…][+][文件预览…]——
    //    「+」恒在最后一个助手 Tab 之后、日志/产物恒在最左（对齐 DSHAgentManage 观感，
    //    拖动非助手页不越区，「+」不会漂到条中间）。moveTab 为程序性移动，不发
    //    tabMoved/currentChanged，不会重入。
    // 2026.08.23 修「助手 Tab 无 x」：旧版对「不在 m_aAssts 里的页」一律 setTabButton(nullptr)
    //   ——新建助手时 CreateAsstTab 先插 Tab 后入列表，该瞬间新 Tab 被误判为非助手页，
    //   原生关闭按钮被剥掉且再无恢复路径（助手 Tab 从此没有 x）。改为只对四个已知
    //   非助手页（日志/阶段产物/预览/「+」）隐藏，未知页（新建助手等）一律不碰。
    if (m_pTabs == nullptr) {
        return;
    }
    int iLastAsst = -1;
    for (int i = 0; i < m_pTabs->count(); ++i) {
        if (AsstByView(m_pTabs->widget(i)) != nullptr) {
            iLastAsst = i;
        }
    }
    for (int i = 0; i < m_pTabs->count(); ++i) {
        QWidget* pW = m_pTabs->widget(i);
        const bool bKnownNonAsst = (pW == m_pView || pW == m_pArtifactsPage ||
                                    pW == m_pFileView || pW == m_pAddTabPage);
        if (bKnownNonAsst) {
            m_pTabs->tabBar()->setTabButton(i, QTabBar::RightSide, nullptr);
            m_pTabs->tabBar()->setTabButton(i, QTabBar::LeftSide, nullptr);
        }
    }
    // 有助手页才摆「+」页（构造早期无助手时先不出）。
    if (iLastAsst < 0 || m_pAddTabPage == nullptr) {
        const int iAddTab = m_pTabs->indexOf(m_pAddTabPage);
        if (iAddTab >= 0) {
            m_pTabs->removeTab(iAddTab);
        }
    } else if (m_pTabs->indexOf(m_pAddTabPage) < 0) {
        const int iNew = m_pTabs->insertTab(iLastAsst + 1, m_pAddTabPage,
                                            QStringLiteral("+"));
        m_pTabs->tabBar()->setTabButton(iNew, QTabBar::RightSide, nullptr);
        m_pTabs->tabBar()->setTabButton(iNew, QTabBar::LeftSide, nullptr);
    }
    // 分区顺序整队：[日志][阶段产物][打开助手（按当前 Tab 相对顺序）][+][其余（文件预览）]。
    if (m_pTabs->indexOf(m_pAddTabPage) >= 0) {   // 仅助手存在时整队
        QList<QWidget*> aOrder;
        if (m_pView != nullptr && m_pTabs->indexOf(m_pView) >= 0) {
            aOrder.append(m_pView);
        }
        if (m_pArtifactsPage != nullptr && m_pTabs->indexOf(m_pArtifactsPage) >= 0) {
            aOrder.append(m_pArtifactsPage);
        }
        for (int i = 0; i < m_pTabs->count(); ++i) {
            QWidget* pW = m_pTabs->widget(i);
            if (pW != m_pAddTabPage && AsstByView(pW) != nullptr) {
                aOrder.append(pW);   // 助手按当前相对顺序
            }
        }
        aOrder.append(m_pAddTabPage);   // 「+」钉在最后助手之后
        for (int i = 0; i < m_pTabs->count(); ++i) {
            QWidget* pW = m_pTabs->widget(i);
            if (!aOrder.contains(pW)) {
                aOrder.append(pW);   // 文件预览等其余页保持相对顺序殿后
            }
        }
        for (int iPos = 0; iPos < aOrder.size(); ++iPos) {
            const int iIdx = m_pTabs->indexOf(aOrder.at(iPos));
            if (iIdx >= 0 && iIdx != iPos) {
                m_pTabs->tabBar()->moveTab(iIdx, iPos);
            }
        }
    }
}

void CExecutionPanel::OnAsstTabCloseRequested(int a_iIdx) {
    // 原生「x」：关闭对应助手（数据保留，可经「▾」恢复）。
    SAsst* pAsst = AsstByView(m_pTabs->widget(a_iIdx));
    if (pAsst != nullptr) {
        CloseAsst(pAsst->sId);
    }
}

CExecutionPanel::SAsst* CExecutionPanel::AsstByView(QWidget* a_pView) {
    for (SAsst& st : m_aAssts) {
        if (st.pView == a_pView) {
            return &st;
        }
    }
    return nullptr;
}

CMessageView* CExecutionPanel::CreateAsstTab(SAsst& a_stAsst, int a_iPos) {
    // 建视图 + 右上角提示词按钮 + 插 Tab（默认插在最后一个助手 Tab 右侧）。
    a_stAsst.pView = new CMessageView(this);
    QFont fView = a_stAsst.pView->font();
    fView.setPointSize(11);
    a_stAsst.pView->setFont(fView);
    CreatePromptButton(a_stAsst);
    int iIdx = (a_iPos >= 0) ? a_iPos : m_pTabs->count();
    if (a_iPos < 0) {
        // 默认：插到最后一个助手 Tab 之后（文件预览页保持在最右）。
        for (int i = m_pTabs->count() - 1; i >= 0; --i) {
            if (AsstByView(m_pTabs->widget(i)) != nullptr) {
                iIdx = i + 1;
                break;
            }
        }
    }
    const bool bRun = (m_bAdhocRunning && a_stAsst.sId == m_sRunningAsst);
    Q_UNUSED(bRun);
    m_pTabs->insertTab(iIdx, a_stAsst.pView, AsstTabText(a_stAsst));   // 2026.08.27 ●/⏳ 统一 AsstTabText
    UpdateTabCloseButtons();
    return a_stAsst.pView;
}

void CExecutionPanel::RemoveAsstTab(SAsst& a_stAsst) {
    // 摘 Tab + 删视图（提示词按钮是其子控件随删）。
    // 摘除前先预切走当前页：QTabWidget 摘掉当前页时会自动激活相邻页——相邻常是「+」占位页，
    // 会被 OnAsstTabCurrentChanged 当成点「+」而新增助手（重置场景即死循环根因，也是
    // 手动关闭助手时的幽灵新增）。预切到另一个打开的助手，无则日志页。
    if (m_pTabs->currentWidget() == a_stAsst.pView) {
        CMessageView* pNext = nullptr;
        for (SAsst& st : m_aAssts) {
            if (&st != &a_stAsst && !st.bClosed && st.pView != nullptr) {
                pNext = st.pView;
                break;
            }
        }
        m_pTabs->setCurrentWidget(pNext != nullptr ? static_cast<QWidget*>(pNext)
                                                   : static_cast<QWidget*>(m_pView));
    }
    const int iIdx = m_pTabs->indexOf(a_stAsst.pView);
    if (iIdx >= 0) {
        m_pTabs->removeTab(iIdx);   // 预切后不再是当前页，Qt 不再自动激活「+」页
    }
    delete a_stAsst.pView;
    a_stAsst.pView = nullptr;
    a_stAsst.pBtnPrompts = nullptr;
    a_stAsst.pBtnBottom  = nullptr;
    UpdateTabCloseButtons();
}

void CExecutionPanel::CloseAsst(const QString& a_sId) {
    // 点「x」：关闭该助手——Tab 摘除、视图控件销毁，但对话/会话数据保留（bClosed=true），
    //   可经「▾」菜单随时恢复（2026.08.23 由「删除」改为「关闭」语义）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || pAsst->bClosed) {
        return;
    }
    if (pAsst->sId == m_sRunningAsst && m_bAdhocRunning) {
        QMessageBox::information(this, QStringLiteral("关闭助手"),
            QStringLiteral("该助手正在执行指令，请先停止再关闭。"));
        return;
    }
    // 2026.08.27 有排队指令的助手不可关闭：关闭会丢弃未发送的指令（防静默丢失）。
    if (AsstQueued(pAsst->sId)) {
        QMessageBox::information(this, QStringLiteral("关闭助手"),
            QStringLiteral("该助手有排队等待发送的指令，请等它发送完成后再关闭。"));
        return;
    }
    if (std::count_if(m_aAssts.begin(), m_aAssts.end(),
                      [](const SAsst& st) { return !st.bClosed; }) <= 1) {
        QMessageBox::information(this, QStringLiteral("关闭助手"),
            QStringLiteral("至少需要保留一个打开的助手。"));
        return;
    }
    RemoveAsstTab(*pAsst);
    // 2026.08.25 空助手（无用户发送的提示词）关闭即舍弃，不进「▾」历史：
    //   数据无价值，保留只会占用档位/编号。
    const bool bEmpty = !AsstHasUserPrompt(*pAsst);
    // 控件已随视图销毁：清对齐列表与分帧重建状态（数据 aBlocks 保留，重开时按需重建）。
    pAsst->aWidgets.clear();
    pAsst->iRebuildIdx = 0;
    ++pAsst->uGen;
    pAsst->bRebuilding = false;
    pAsst->bClosed = true;
    if (bEmpty) {
        m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });
    } else {
    // 移到列表末尾（m_aAssts 顺序 = Tab 顺序，已关闭的排在后面）。
    const SAsst stKeep = *pAsst;
    m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });
    m_aAssts.append(stKeep);
    }
    if (m_sRunningAsst == a_sId) {
        m_sRunningAsst.clear();   // 防御（运行中已拦截，正常不达）
    }
    // QTabWidget 摘页时已自动切到相邻页 → OnAsstTabCurrentChanged 已更新 m_sActiveAsst；
    // 兜底：若当前页不是助手页，激活第一个打开的助手。
    if (AsstById(m_sActiveAsst) == nullptr || AsstById(m_sActiveAsst)->bClosed) {
        const SAsst* pFirst = nullptr;
        for (const SAsst& st : m_aAssts) {
            if (!st.bClosed) { pFirst = &st; break; }
        }
        if (pFirst != nullptr) {
            m_pTabs->setCurrentWidget(pFirst->pView);
        }
    }
    UpdateAdhocInputState();
    emit sigAsstLayoutChanged();   // 关闭状态入档 → 落盘
}

void CExecutionPanel::OnAsstMenuClicked() {
    // 点「▾」：弹出已关闭助手菜单，选择恢复（无已关闭项时置灰提示）。
    QMenu menu(this);
    bool bAny = false;
    for (const SAsst& st : m_aAssts) {
        if (!st.bClosed) {
            continue;
        }
        if (!AsstHasUserPrompt(st)) {
            continue;   // 2026.08.25 空助手（无用户发送的提示词）不入历史列表
        }
        bAny = true;
        const QString sLabel = QStringLiteral("%1（%2 条对话）").arg(st.sName).arg(st.aBlocks.size());
        QAction* pAct = menu.addAction(sLabel);
        connect(pAct, &QAction::triggered, this, [this, id = st.sId]() { ReopenAsst(id); });
    }
    if (!bAny) {
        QAction* pEmpty = menu.addAction(QStringLiteral("无已关闭的助手"));
        pEmpty->setEnabled(false);
    }
    // 在「▾」按钮下方弹出（菜单按点击时的鼠标位置对齐）。
    menu.exec(QCursor::pos());
}

void CExecutionPanel::ReopenAsst(const QString& a_sId) {
    // 从「▾」菜单恢复已关闭助手：重建 Tab（插到最后一个打开的助手之后）并激活。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || !pAsst->bClosed) {
        return;
    }
    // 移到第一个已关闭助手之前（即打开助手的末尾，与 Tab 插入位置一致）。
    SAsst stKeep = *pAsst;
    m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });
    int iPos = 0;
    while (iPos < m_aAssts.size() && !m_aAssts[iPos].bClosed) { ++iPos; }
    m_aAssts.insert(iPos, stKeep);
    SAsst* pNew = AsstById(a_sId);
    pNew->bClosed = false;
    CreateAsstTab(*pNew);
    UpdateTabCloseButtons();
    m_pTabs->setCurrentWidget(pNew->pView);   // → sigAsstSwitched（主窗口换载该助手会话）
    emit sigAsstLayoutChanged();              // 恢复入档 → 落盘
}

void CExecutionPanel::OnAsstAddClicked() {
    // 点「+」：新增助手（名字自动编号「助手N」：取已有编号（打开/已关闭、未被改名）中
    //   缺失的最小编号，如已有 1、2、10 → 新增为「助手3」，避开重名），自动激活。
    QSet<int> aUsed;
    for (const SAsst& st : m_aAssts) {
        const QRegularExpression re(QStringLiteral("^助手(\\d+)$"));
        const auto m = re.match(st.sName);
        if (m.hasMatch()) {
            aUsed.insert(m.captured(1).toInt());
        }
    }
    int iN = 1;
    while (aUsed.contains(iN)) { ++iN; }
    const QString sName = QStringLiteral("助手%1").arg(iN);
    SAsst st;
    st.sId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    st.sName = sName;
    m_aAssts.append(st);   // 先入列表再建 Tab（UpdateTabCloseButtons 需按视图反查助手）
    CreateAsstTab(m_aAssts.last());
    UpdateTabCloseButtons();
    m_pTabs->setCurrentWidget(AsstById(st.sId)->pView);   // → sigAsstSwitched（主窗口换会话）
    emit sigAsstAdded(st.sId);
}

void CExecutionPanel::OnAsstTabRenamed(int a_iIdx) {
    // 双击助手 Tab → 改名（Tab 标题即助手名）。
    SAsst* pAsst = AsstByView(m_pTabs->widget(a_iIdx));
    if (pAsst == nullptr) {
        return;
    }
    bool bOk = false;
    const QString sName = QInputDialog::getText(this, QStringLiteral("重命名助手"),
        QStringLiteral("助手名："), QLineEdit::Normal, pAsst->sName, &bOk)
        .trimmed();
    if (!bOk || sName.isEmpty() || sName == pAsst->sName) {
        return;
    }
    pAsst->sName = sName;
    m_pTabs->setTabText(a_iIdx, AsstTabText(*pAsst));   // 2026.08.27 ●/⏳ 统一 AsstTabText
    emit sigAsstLayoutChanged();   // 名字入档 → 落盘
}

void CExecutionPanel::OnAsstTabMoved(int, int) {
    SyncAsstsOrderToTabs();
    UpdateTabCloseButtons();   // 「+」页钉回最后助手 Tab 之后
    emit sigAsstLayoutChanged();   // 顺序入档 → 落盘
}

void CExecutionPanel::SyncAsstsOrderToTabs() {
    // 按 Tab 实际顺序重排 m_aAssts（助手之间的相对顺序；非助手页位置忽略，
    //   已关闭助手不在 Tab 上，统一保持在列表末尾）。
    QList<SAsst> aNew;
    QSet<QString> aTaken;   // 已按 Tab 顺序取走的助手 id（同 id 唯一，直接标记）
    for (int i = 0; i < m_pTabs->count(); ++i) {
        SAsst* p = AsstByView(m_pTabs->widget(i));
        if (p != nullptr && !aTaken.contains(p->sId)) {
            aTaken.insert(p->sId);
            aNew.append(*p);
        }
    }
    for (const SAsst& st : m_aAssts) {
        if (st.bClosed && !aTaken.contains(st.sId)) {
            aNew.append(st);
        }
    }
    if (aNew.size() == m_aAssts.size()) {
        m_aAssts = aNew;
    }
}

void CExecutionPanel::OnAsstTabCurrentChanged(int a_iIdx) {
    // 重置拆除期（ResetAssistants 拆 Tab / 预切页）的程序性 currentChanged 一律忽略：
    // 此时「+」页可能被 Qt 自动激活，不应当成用户新增；激活助手也即将整体复位。
    if (m_bAsstTabsResetting) {
        return;
    }
    // 「+」页被选中：新增助手并弹回（OnAsstAddClicked 末尾会把当前页切到新助手 Tab）。
    if (m_pTabs->widget(a_iIdx) == m_pAddTabPage) {
        const QString sBack = (AsstById(m_sActiveAsst) && !AsstById(m_sActiveAsst)->bClosed)
                                  ? m_sActiveAsst : QString();
        OnAsstAddClicked();
        if (!sBack.isEmpty() && AsstById(m_sActiveAsst) == nullptr) {
            m_pTabs->setCurrentWidget(AsstById(sBack)->pView);
        }
        return;
    }
    // 切 Tab：当前页是助手页 → 激活切换（非助手页如日志/产物/预览不改激活助手）。
    SAsst* pAsst = AsstByView(m_pTabs->widget(a_iIdx));
    if (pAsst == nullptr || pAsst->sId == m_sActiveAsst) {
        return;
    }
    const QString sOld = m_sActiveAsst;
    m_sActiveAsst = pAsst->sId;
    // 首次切到该助手（或其控件列表为空）→ 按数据重建其视图（恢复档时惰性补建）。
    if (pAsst->aWidgets.isEmpty() && !pAsst->aBlocks.isEmpty()) {
        RebuildAdhocView();
    } else {
        RebuildPromptList();
    }
    // 上下文占用进度条切换到该助手快照。
    m_iLastContextFill = pAsst->iCtxFill;
    UpdateContextButton();
    UpdateCostLabel();   // 2026.08.27 无任务时费用栏随助手切换显示各自动流量记录
    UpdateAdhocInputState();   // 2026.08.27 运行中切助手：发送按钮 ➤追加/⏳排队 文案随目标切换
    // FR-3：各助手右上角提示词按钮可见性（仅当前页的按钮显示并贴右上角）。
    for (SAsst& st : m_aAssts) {
        if (st.pBtnPrompts != nullptr && st.pView != nullptr) {
            const bool bCur = (st.pView == pAsst->pView);
            st.pBtnPrompts->setVisible(bCur);
            if (bCur) {
                st.pBtnPrompts->move(st.pView->width() - st.pBtnPrompts->width() - 11, 6);
                st.pBtnPrompts->raise();
            }
            if (st.pBtnBottom != nullptr) {
                const bool bCurB = (st.pView == pAsst->pView);
                st.pBtnBottom->setVisible(bCurB);
                if (bCurB) {
                    st.pBtnBottom->move(st.pView->width() - st.pBtnBottom->width() - 11, 6 + 24 + 2);
                    st.pBtnBottom->raise();
                }
            }
        }
    }
    emit sigAsstSwitched(sOld, m_sActiveAsst);
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
