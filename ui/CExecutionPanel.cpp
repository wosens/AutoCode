#include "CExecutionPanel.h"
#include "ui/CMenuIcon.h"
#include "ui/CAsstTabBar.h"   // 2026-09-22 多行 Tab 栏（自 HarnessAgent/CAgentTabBar 迁移）
#include "../agent/CAgentRunnerFactory.h"   // 2026.09.02 Assistant 块拍摄产出后端（说话人标签）

#include <functional>
#include <utility>
#include <algorithm>

#include <QLabel>
#include "ui/CTipStyle.h"   // 2026.09.13 自建条目 tip 窗的富文本换行
#include <QLineEdit>
#include <QMessageBox>
#include <QTabBar>
#include <QUuid>
#include <QRegularExpression>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
#include <QTabWidget>
#include <QPointer>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QStringDecoder>
#include <QPushButton>
#include <QProgressBar>
#include <QToolButton>
#include <QComboBox>   // 2026-09-11 权限下拉（自动/手工/只读）
#include <QKeyEvent>
#include <QFontMetrics>
#include <QTextDocument>
#include <QTextBlock>       // 2026.09.26 软换行视觉行估算（逐块遍历）
#include <QTextCursor>
#include <QTimer>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QJsonValue>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QActionGroup>   // 2026-10-05 右键「选择模型」子菜单互斥勾选组
#include <QPoint>
#include <QInputDialog>
#include <QCursor>
#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QToolTip>
#include <QEnterEvent>
#include <QScreen>   // 2026.09.19 tip 出屏处理：光标所在屏可用区取几何

#include "ui/CArtifactView.h"
#include "ui/CMessageView.h"
#include "ui/CMessageBlock.h"
#include "ui/CFilePreviewWidget.h"
#include "core/CTaskInfo.h"
#include "core/CStageMachine.h"
#include "core/StageDef.h"

namespace {
// （2026.09.03 流呈现常量 k_iRebuildChunk/k_iViewInitial/k_iViewPrepend 已随实现
//   迁入 CStreamPresenter.cpp——任务/助手两路共用，不再散落面板。）
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
    bool IsBusy() const { return m_pTimer->isActive(); }   // 2026.09.05 观察口（测试/诊断）
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
        setObjectName(QStringLiteral("PromptFlyout"));   // 2026.09.13 测试可寻址（无 Q_OBJECT，className 不可靠）
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
    // 追加一条提问：a_iIdx = 提问在面板 m_aPromptIdx 序号，a_sText 全文（存 UserRole+1 用于 tooltip）；
    // a_sStamp = 提问时间戳（2026-10-08 tooltip 首行灰字日期用；空=无日期行，存 UserRole+2）。
    void AddItem(int a_iIdx, const QString& a_sText, const QString& a_sStamp = QString()) {
        const QString sView = a_sText.section(QLatin1Char('\n'), 0, 0);   // 单行预览（首行）
        const QString sDisp = QStringLiteral("%1  %2").arg(a_iIdx + 1).arg(sView.left(120));
        auto* pItem = new QListWidgetItem(sDisp, m_pList);
        pItem->setData(Qt::UserRole, a_iIdx);
        pItem->setData(Qt::UserRole + 1, a_sText);   // 全文，自建 tip 窗显示
        pItem->setData(Qt::UserRole + 2, a_sStamp);  // 时间戳，tip 首行日期
        pItem->setToolTip(QString());   // 关闭系统 tooltip，改用自建 tip 窗（见 ShowItemTip）
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
    // 2026.09.13 自建条目全文 tip 窗（替代 QToolTip）：QTipLabel 在 app 级事件过滤器里
    //   对「任何 Leave 事件」（含浮层自身/列表子控件的 Leave）都会 ~300ms 自毁，鼠标
    //   移到 tip 上读全文必被打断；自建窗 WA_TransparentForMouseEvents 鼠标穿透，停在
    //   上面不触发任何 Enter/Leave，想读多久读多久，随浮层收起/换项更新。
    QWidget* TipWindow() const { return m_pTip; }
    void ShowItemTip(const QString& a_sText, const QString& a_sStamp = QString()) {
        if (m_pTip == nullptr) {
            m_pTip = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint
                                         | Qt::NoDropShadowWindowHint);
            m_pTip->setObjectName(QStringLiteral("PromptFlyoutTip"));
            m_pTip->setAttribute(Qt::WA_ShowWithoutActivating);   // 不抢焦点
            m_pTip->setAttribute(Qt::WA_TransparentForMouseEvents);   // 鼠标穿透（核心）
            m_pTip->setFont(QToolTip::font());            // 与全局 tooltip 同字体（11pt，CTipStyle）
            // 2026.09.16 白底 + 1px 外框（用户需求：浅黄无边框时与浮层列表/消息区
            //   混排易混淆）：白底黑字 + 灰外框；QFrame::Box 的 frameWidth 计入
            //   QLabel::heightForWidth，下方 resize 自适应无需另调。
            QPalette palTip = m_pTip->palette();
            palTip.setColor(QPalette::Window, Qt::white);
            palTip.setColor(QPalette::WindowText, Qt::black);
            palTip.setColor(QPalette::Shadow, QColor(0x80, 0x80, 0x80));   // 外框线深灰
            m_pTip->setPalette(palTip);
            m_pTip->setFrameShape(QFrame::Box);
            m_pTip->setFrameShadow(QFrame::Plain);
            m_pTip->setLineWidth(1);
            m_pTip->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            m_pTip->setWordWrap(true);                    // 富文本自动换行（同 CTipStyle）
        }
        // 2026-10-08 tooltip 首行加提问日期（用户需求）：有则置顶灰字一行，正文另起段落。
        QString sBody = CTipStyle::WrapText(a_sText);
        if (!a_sStamp.isEmpty()) {
            sBody = QStringLiteral("<p style='margin-top:0;margin-bottom:2px'>"
                                   "<span style='color:#6b7280'>%1</span></p>%2")
                        .arg(a_sStamp.toHtmlEscaped(), sBody);
        }
        m_pTip->setText(sBody);
        // 2026.09.19 tip 出屏处理（用户需求：屏幕边缘/超长提示词时 tip 可能越出屏幕）：
        //   ① 取屏以【光标】为准（pt=光标+偏移在屏幕边缘可能落屏外，screenAt 落空回退
        //     主屏在多显示器下会选错屏）；
        //   ② 宽度不超过本屏可用宽，高度封顶可用高（超长全文换行后可超屏高，无滚动条的
        //     QLabel 只能裁切显示，宁可裁切不出屏）；
        //   ③ 右缘放不下 → 翻到光标左侧；下缘放不下 → 翻到光标上方；翻转后仍越
        //     左/顶缘（光标贴屏左/顶部）→ qBound 贴边收口，保证四边全在屏内。
        QScreen* pScr = QGuiApplication::screenAt(QCursor::pos());
        if (pScr == nullptr) {
            pScr = QGuiApplication::primaryScreen();
        }
        const QRect rcAvail = pScr->availableGeometry();
        const int iTipW = qMin(440, rcAvail.width());
        const int iMaxH = qMax(24, rcAvail.height() - 2);
        m_pTip->resize(iTipW, qBound(24, m_pTip->heightForWidth(iTipW), iMaxH));
        QPoint pt = QCursor::pos() + QPoint(2, 21);   // 同 QToolTip placeTip 的偏移规则
        if (pt.x() + iTipW > rcAvail.right() + 1) {
            pt.rx() -= 4 + iTipW;   // 右缘放不下 → 翻到光标左侧
        }
        pt.rx() = qBound(rcAvail.left(), pt.x(), rcAvail.right() + 1 - iTipW);
        if (pt.y() + m_pTip->height() > rcAvail.bottom() + 1) {
            pt.ry() -= 24 + m_pTip->height();   // 下缘放不下 → 翻到光标上方
        }
        pt.ry() = qBound(rcAvail.top(), pt.y(), rcAvail.bottom() + 1 - m_pTip->height());
        m_pTip->move(pt);
        m_pTip->show();
        m_pTip->raise();
    }
    void HideItemTip() {
        if (m_pTip != nullptr) {
            m_pTip->hide();
        }
    }
protected:
    bool eventFilter(QObject* a_pWatched, QEvent* a_pEvent) override {
        // hover 到列表项：立即（无延迟）显示该提问全文；空区（项间缝隙/边留白）则藏 tip。
        if (a_pWatched == m_pList->viewport() &&
            a_pEvent->type() == QEvent::ToolTip) {
            const QPoint ptPos = m_pList->viewport()->mapFromGlobal(QCursor::pos());
            QListWidgetItem* pItem = m_pList->itemAt(ptPos);
            if (pItem != nullptr) {
                ShowItemTip(pItem->data(Qt::UserRole + 1).toString(),
                            pItem->data(Qt::UserRole + 2).toString());
            } else {
                HideItemTip();
            }
            return true;   // 吃掉，阻止系统 tooltip 延迟机制
        }
        return QWidget::eventFilter(a_pWatched, a_pEvent);
    }
protected:
    void hideEvent(QHideEvent*) override {
        HideItemTip();   // 2026.09.13 浮层收起：同步藏自建 tip 窗，防孤儿滞留
    }
private:
    QListWidget* m_pList    = nullptr;
    QLabel*      m_pTip     = nullptr;      // 2026.09.13 自建条目全文 tip（鼠标穿透）
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

CExecutionPanel::~CExecutionPanel() {
    // 2026.09.03 释放任务流堆对象（助手流随 SAsst 的 shared_ptr 自清；Unbind 防在途帧回调）。
    if (m_pCurStream != nullptr) {
        m_pCurStream->Unbind();
    }
    qDeleteAll(m_aStreams);
    m_aStreams.clear();
    m_pCurStream = nullptr;
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

    // 2026.09.04 阶段横向图标条（替代 QTreeWidget 阶段进度表）：旧表每阶段一行
    //   纵向占 120px+ 且需与左栏任务列表底部做高度同步（SyncStagesHeight 已随本次
    //   改造移除）；图标条固定 58px 横向排布（圆形状态图标 + 连接线 + 短名），
    //   纵向空间全部让给日志区，横向空间充分利用。
    m_pStageBar = new CStageBar(this);
    // 2026.09.23 v3.1.227 用户需求：无任务选中时阶段条不占位——初始为空先隐藏，
    //   日志 Tab 区顶到面板顶（载入任务后 RefreshTask/SetCurrentTask 恢复显示）。
    m_pStageBar->setVisible(false);
    // 需求3：阶段右键「再次执行」（仅重跑该阶段，不推进后续）——坐标经 StageAt 反查。
    m_pStageBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pStageBar, &CStageBar::customContextMenuRequested,
            this, &CExecutionPanel::OnStagesContextMenu);

    auto* pSplit = new QSplitter(Qt::Vertical, this);

    // 对话流（Claude Code 风格：Markdown 助手块 + 可折叠工具卡 + 事件徽章）。
    //   2026.09.03 到顶补建/丢块同步等接线由各流 CStreamPresenter::Bind 负责
    //   （任务流=EnsureStream 创建时；助手流=CreateAsstTab 建视图时），面板不再直连。
    m_pView = new CMessageView(this);

    // 日志 / 阶段产物 / 助手 三页 Tab（2026-09-22 换多行 Tab 栏：宽度不足自动换行，
    //   助手多时不再出滚动箭头；setTabBar 必须在加页前调用）
    m_pTabs = new CAsstTabWidget(this);
    m_pAsstBar = new CAsstTabBar;
    m_pTabs->setTabBar(m_pAsstBar);
    m_pAsstBar->SetPageResolver([this](int a_iIndex) -> QWidget* {
        return m_pTabs->widget(a_iIndex);   // 2026-10-02 外档粗体绘制反查页
    });
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
    m_pTabs->tabBar()->setTabVisible(m_pTabs->indexOf(pOutSplit), false);
    // 2026-10-02 缺省隐藏：无任务选中就没有阶段产物（SetCurrentTask 按选中态显隐）
    connect(m_pOutputs, &QListWidget::currentRowChanged,
            this, &CExecutionPanel::OnOutputSelected);

    // —— 2026.08.22 多助手：助手对话流不再固定单 Tab，改为按助手动态增删 ——
    //   Tab 交互（可关闭/可拖动/「+」角标/双击改名）在此统一配置，助手 Tab 随后按需创建。
    SetupTabsInteraction();

    // 2026.09.04 阶段条固定高放 splitter 之外（纵向空间全给下方 Tab 区）；
    //   splitter 现仅含 Tab（日志/产物/助手），旧「阶段/日志」分隔条随之消失。
    pV->addWidget(m_pStageBar, 0);
    pSplit->addWidget(m_pTabs);
    pSplit->setChildrenCollapsible(false);
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
    // 2026.09.26 软换行视觉行计高：无回车的长文本（右键「添加到聊天框」注入）也长高；
    //   Resize 重算的上限行数经属性携带（见 eventFilter）。
    m_pTaskInput->setProperty("acMaxLines", k_iTaskMaxLines);
    connect(m_pTaskInput, &QPlainTextEdit::blockCountChanged,
            this, [this](int){ AdjustInputHeight(m_pTaskInput, k_iTaskMaxLines); });
    connect(m_pTaskInput, &QPlainTextEdit::textChanged,
            this, [this](){ AdjustInputHeight(m_pTaskInput, k_iTaskMaxLines); });
    AdjustInputHeight(m_pTaskInput, k_iTaskMaxLines);
    m_pBtnInject = new QPushButton(QStringLiteral("介入"), this);
    m_pBtnInject->setToolTip(QStringLiteral("向当前运行中的任务注入指令（Ctrl+Enter）"));
    setPt11(m_pBtnInject);
    pInjectBar->addWidget(m_pTaskInput, 1);
    pInjectBar->addWidget(m_pBtnInject);
    // 2026-10-03 用户需求：无任务选中时介入栏整体隐藏（不占纵向空间）——条上控件
    //   （输入框/按钮）只对当前任务有意义；包一层容器统一 set_visible，隐藏时
    //   QBoxLayout 把容器整个从布局剔除（无空行残留）。载任务由 SetCurrentTask 恢复。
    m_pTaskBarWidget = new QWidget(this);
    m_pTaskBarWidget->setLayout(pInjectBar);
    pV->addWidget(m_pTaskBarWidget);

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

    // —— 提示词输入栏（2026.09.05 per-助手化已从面板级移除）：输入框/只读切换/上下文
    //    进度条/发送/撤销不再全局共享，随各助手 Tab 页内的独立实例切换（CreateAsstWidgets
    //    建页时创建，SAsst 持有）；面板底部仅保留任务侧费用栏。 ——
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
    m_pPromptFlyout->installEventFilter(this);   // Enter/Leave → 维护收起定时器（见 eventFilter）
    // 2026.09.13 浮层收起规则改为「鼠标位置 + 500ms 容忍」：离开浮层不再立即收起，
    //   而是延迟 500ms 检查全局鼠标位是否仍在 浮层∪触发按钮∪当前Tooltip 区域内，
    //   在则续命重查；鼠标常会移到条目 Tooltip（贴光标在浮层外沿展开）上读全文，
    //   旧规则 Leave 即收会把浮层连同 Tooltip 一起带走，很烦。
    m_pFlyoutHideTimer = new QTimer(this);
    m_pFlyoutHideTimer->setSingleShot(true);
    m_pFlyoutHideTimer->setInterval(500);
    connect(m_pFlyoutHideTimer, &QTimer::timeout, this, [this]() {
        if (m_pPromptFlyout == nullptr || !m_pPromptFlyout->isVisible()) {
            return;   // 已收起/未弹出：无事件
        }
        if (!IsCursorInsideFlyoutRegion()) {
            m_pPromptFlyout->hide();   // 鼠标离开区域超 500ms → 收起（hideEvent 同步藏自建 tip 窗）
        } else {
            m_pFlyoutHideTimer->start();   // 仍在区域内（如在 tip 窗上读全文）：续命重查
        }
    });
    // 2026.08.22 初始默认助手（首个助手，无存档时的空会话）。
    ResetAssistants();

    SetAdhocEnabled(false);   // 初始无工程：禁用各助手输入栏（打开工程后由主窗口启用）

    // 费用 / token 统计栏（费用两位小数；token 用 K/M/G）
    // 2026.09.05 任务/助手统计完全隔离：面板底部此栏仅显示【任务侧】统计（无任务时置空）；
    //   各助手的费用/token/请求/耗时/本轮实時在其自己 Tab 页内的统计栏（SAsst::pLblCost/Live）。
    // 费用栏：[执行态指示器][费用文本] 同行（#4）；指示器运行中转圈、空闲显状态色点。
    auto* pCostBar = new QHBoxLayout();
    pCostBar->setContentsMargins(0, 0, 0, 0);
    m_pBusy = new CBusyIndicator(this);
    m_pLblCost = new QLabel(this);
    pCostBar->addWidget(m_pBusy);
    pCostBar->addWidget(m_pLblCost, 1);
    // 2026.08.27 本轮实时 token（Claude Code 式“当前指令消耗”）：仅运行中可见。
    m_pLblLive = new QLabel(this);
    m_pLblLive->hide();
    pCostBar->addWidget(m_pLblLive);
    // 2026-10-03 用户需求：费用栏同介入栏——无任务选中时整体隐藏（任务侧专属控件，
    //   助手统计在各自 Tab 页内，取消选中后此栏只占空白）；容器化便于整体显隐。
    m_pCostBarWidget = new QWidget(this);
    m_pCostBarWidget->setLayout(pCostBar);
    pV->addWidget(m_pCostBarWidget);
    UpdateTaskBarsVisibility();   // 初始无任务：两栏隐藏
    SetTaskTabsVisible(false);    // 2026-10-03 初始无任务：日志/阶段产物两页也不留

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
        FlushAsstLiveUsage();   // 2026.09.05 助手侧 per-助手本轮实時一并冲刷
    });
}

// 2026.09.05 助手「本轮 ↑↓」限频到点：逐助手冲刷暂存值到各自页内标签。
void CExecutionPanel::FlushAsstLiveUsage() {
    const QList<QString> aIds = m_aPendingAsstLive.keys();
    for (const QString& sId : aIds) {
        const QPair<qint64, qint64> pr = m_aPendingAsstLive.value(sId);
        SAsst* p = AsstById(sId);
        if (p == nullptr || p->pLblLive == nullptr) {
            m_aPendingAsstLive.remove(sId);   // 助手/控件已不在：丢暂存
            continue;
        }
        if (pr.first <= 0 && pr.second <= 0) {
            p->pLblLive->hide();
            m_aPendingAsstLive.remove(sId);   // 归零指令冲刷即丢，防残留条目随助手生灭堆积
        } else {
            p->pLblLive->setText(QStringLiteral("本轮 ↑%1 ↓%2")
                                      .arg(QLocale().toString(pr.first))
                                      .arg(QLocale().toString(pr.second)));
            p->pLblLive->show();
            p->pLblLive->raise();
        }
    }
}

// 2026.09.05 助手本轮实時（per-助手）：编排器按会话累计后带 id 上抛，暂存 + 限频上屏。
void CExecutionPanel::OnAdhocLiveUsage(const QString& a_sAsstId, qint64 a_iIn, qint64 a_iOut) {
    m_aPendingAsstLive[a_sAsstId] = qMakePair(a_iIn, a_iOut);
    if (m_pLiveTimer != nullptr && !m_pLiveTimer->isActive()) {
        m_pLiveTimer->start();
    }
}

void CExecutionPanel::UpdateCostLabel() {
    // 底部费用/状态栏（2026.09.05 起仅任务侧）：[费用|token|请求|状态|耗时]。
    //   无任务时置空——助手统计已完全隔离到各自 Tab 页内（UpdateAsstCostLabel）。
    if (m_pCurTask == nullptr) {
        m_pLblCost->setText(QString());
        return;
    }
    // 2026.08.18 费用 ≤0 显示 "-"（DSH 后端无计费恒 0）。
    const QString sCost = m_pCurTask->TotalCost() > 0.0
        ? QStringLiteral("$%1").arg(m_pCurTask->TotalCost(), 0, 'f', 2)
        : QStringLiteral("-");
    QString sText = QStringLiteral("费用: %1 | 输入(累计):%2 输出:%3 | 请求:%4 | %5")
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

// 2026.09.05 per-助手统计栏：刷该助手页内的费用/token/请求/耗时（不再借用面板底部栏）。
void CExecutionPanel::UpdateAsstCostLabel(const SAsst& a_stAsst) {
    if (a_stAsst.pLblCost == nullptr) {
        return;
    }
    if (a_stAsst.dCost <= 0.0 && a_stAsst.iInTok == 0 && a_stAsst.iOutTok == 0
        && a_stAsst.iReqs == 0 && !a_stAsst.bRunning) {
        a_stAsst.pLblCost->setText(QStringLiteral("费用: $0.00 | 输入(累计):0 输出:0 | 请求:0"));
        return;
    }
    const QString sCost = a_stAsst.dCost > 0.0
        ? QStringLiteral("$%1").arg(a_stAsst.dCost, 0, 'f', 2)
        : QStringLiteral("-");
    QString sText = QStringLiteral("费用: %1 | 输入(累计):%2 输出:%3 | 请求:%4")
        .arg(sCost)
        .arg(FormatTokens(a_stAsst.iInTok))
        .arg(FormatTokens(a_stAsst.iOutTok))
        .arg(a_stAsst.iReqs);
    // 运行中：耗时 = 历史累计 + 本轮起始钟实时值（per-助手钟，多助手并发各自计时）。
    const qint64 iLiveMs = (a_stAsst.bRunning && a_stAsst.tClock.isValid())
                            ? a_stAsst.iElapsedMs + a_stAsst.tClock.elapsed()
                            : a_stAsst.iElapsedMs;
    if (iLiveMs > 0) {
        sText += QStringLiteral(" | 耗时 %1").arg(FormatElapsed(iLiveMs));
    }
    a_stAsst.pLblCost->setText(sText);
    // 2026.09.21 口径说明（他机 GLM-5.3 实录：两轮提问后输入 60 万被误读为
    //   「上下文 500K」）：输入=历次 API 请求的 plain input token 总和——模型无
    //   状态，agent 每次工具调用后的下一跳都重发全量对话，长会话累计远大于
    //   单次占用；真实上下文占用看右侧上下文进度条（最后请求的
    //   input+cacheRead+cacheWrite，右键看数值/压缩入口）。
    a_stAsst.pLblCost->setToolTip(QStringLiteral(
        "输入/输出 = 本助手所有请求的 token 累计（每跳重发全量对话，长会话远大于上下文占用）；\n"
        "当前上下文占用请看右侧进度条（右键：数值 / 立即压缩 / 设上限）。"));
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

// 2026.08.27 助手独立流量累计：助手消耗归集到该助手自身（随档持久化）。
//   2026.09.05 任务/助手统计完全隔离：无论是否选中任务，助手消耗一律归助手自身
//   （不再并入任务统计）；标签刷新无条件（该助手自己的标签，与激活与否无关）。
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
    UpdateAsstCostLabel(*p);
}

// 2026.09.05 耗时定时器统一开关：任务运行中或任一助手运行中 → 跑（各自标签秒跳）；
//   都空闲 → 停（冻结显示）。取代此前「任务跑才跑 / 无任务且助手跑才跑」的分散判定。
void CExecutionPanel::SyncRunTimer() {
    if (m_pRunTimer == nullptr) {
        return;
    }
    const bool bNeed = (m_pCurTask != nullptr && m_pCurTask->Status() == ETaskStatus::Running)
                       || AnyAsstRunning();
    if (bNeed) {
        m_pRunTimer->start(1000);
    } else {
        m_pRunTimer->stop();
    }
}

void CExecutionPanel::OnRunTimer() {
    // 任务侧：运行中每秒刷新耗时（非 Running 由 RefreshTask 停表）。
    if (m_pCurTask != nullptr && m_pCurTask->Status() == ETaskStatus::Running) {
        UpdateCostLabel();
    }
    // 2026.09.05 助手侧：每个运行中的助手其页内统计栏「耗时」秒级跳动
    //   （流量/token 仍收尾归集；并发助手各自计时互不影响）。
    for (const SAsst& st : std::as_const(m_aAssts)) {
        if (st.bRunning) {
            UpdateAsstCostLabel(st);
        }
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
    // 2026.09.02 产出后端随块入档（说话人标签用；到达时刻拍摄，切后端不改历史）。
    st.sBackend = CAgentRunnerFactory::BackendDisplayName();
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
    // 回填到最近一个尚无结果的 ToolUse（且视角匹配：并发评审下按 sViewId 过滤防串配；
    //   未建件区间由数据层承接，补建时呈现）——与助手日志同一段流呈现代码。
    if (EnsureStream(sId)->BackfillResult(a_sResult, a_sViewId) >= 0) {
        return;   // 命中（流内已 fnDirty→MarkTaskDirty 落盘）
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
        // 2026.09.03 数据在流对象内：助手流取其 pStream（未建=空表）；任务流 EnsureStream。
        static const QList<SBlockData> k_aEmpty;
        const QList<SBlockData>& aBlocks = (pAsst != nullptr)
            ? (pAsst->pStream ? pAsst->pStream->Blocks() : k_aEmpty)
            : EnsureStream(a_sKey)->Blocks();
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
        const QList<SBlockData>& aBlocks = EnsureStream(a_sTaskId)->Blocks();
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

// —— 任务侧流路由（2026.09.03 收拢：原 AppendData/TryMergeWidget/CreateWidget/
//    RebuildView/RebuildChunk 五个函数的实现全部迁入 CStreamPresenter，面板只保留薄路由）——

CStreamPresenter* CExecutionPanel::EnsureStream(const QString& a_sTaskId) {
    // 取/建任务流：fnDirty→MarkTaskDirty（数据变化去抖落盘 chat.json 的统一入口）
    CStreamPresenter* pS = m_aStreams.value(a_sTaskId, nullptr);
    if (pS == nullptr) {
        pS = new CStreamPresenter([this, a_sTaskId]() { MarkTaskDirty(a_sTaskId); });
        m_aStreams.insert(a_sTaskId, pS);
    }
    return pS;
}

void CExecutionPanel::AppendData(const QString& a_sTaskId, const SBlockData& a_stData) {
    // 入流数据 +（当前任务=已绑定视图）上屏；非当前任务流未绑定，Append 内部自动只入数据。
    //   旧实现在此内联合并/建件逻辑，现统一走流呈现器（与助手日志同一段代码）。
    EnsureStream(a_sTaskId)->Append(a_stData);
}

void CExecutionPanel::RebuildView() {
    // 重建当前任务流视图（切任务/重载对话流后调用；无当前流空操作）
    if (m_pCurStream != nullptr) {
        m_pCurStream->Rebuild();
    }
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
    // 需求3：阶段图标条右键「再次执行」——仅重跑选中阶段，不推进后续（编排器 RunSingleStage）。
    if (m_pStageBar == nullptr) {
        return;
    }
    const int iStage = m_pStageBar->StageAt(a_stPos);
    if (iStage < 0 || m_pCurTask == nullptr) {
        return;
    }
    QMenu menu(this);
    QAction* pActRerun = menu.addAction(MenuIcon(QStringLiteral("ow-rerun")), QStringLiteral("再次执行"));
    QAction* pSel = menu.exec(m_pStageBar->mapToGlobal(a_stPos));
    if (pSel == pActRerun) {
        emit sigRerunStage(m_pCurTask, iStage);
    }
}

void CExecutionPanel::RefreshTask(CTaskInfo* a_pTask) {
    if (a_pTask == nullptr || a_pTask->Id() != m_sCurId) {
        return;
    }
    // 2026.09.04 阶段横向图标条：状态判定与旧表完全同口径，仅呈现由两列表格
    //   换为圆图标 + 连接线（详见 CStageBar）。
    const QList<EStage> aFlow = ResolveFlow(a_pTask->Type(), a_pTask->FlowStages());

    QSet<EStage> aDone;
    for (const CTaskInfo::SStageRun& stRun : a_pTask->History()) {
        if (stRun.bOk) {
            aDone.insert(stRun.eStage);
        }
    }
    const EStage eCur = a_pTask->CurrentStage();

    QVector<CStageBar::SNode> aNodes;
    for (const EStage eStage : aFlow) {
        const SStageDef* pDef = FindStage(eStage);
        const QString sName = pDef ? pDef->sNameZh : QString::number(static_cast<int>(eStage));

        CStageBar::EState eSt = CStageBar::EState::Pending;
        if (aDone.contains(eStage) || a_pTask->Status() == ETaskStatus::Done) {
            eSt = CStageBar::EState::Done;
        } else if (eStage == eCur) {
            // 2026.08.15 当前阶段按任务执行时状态标注（历史任务回看不再清一色「待执行」）。
            switch (a_pTask->Status()) {
                case ETaskStatus::Running:
                    eSt = CStageBar::EState::Running;   break;
                case ETaskStatus::Failed:
                    eSt = CStageBar::EState::Failed;    break;
                case ETaskStatus::Paused:
                case ETaskStatus::Stopped:
                    eSt = CStageBar::EState::Incomplete;break;
                default:   // Pending：尚未启动，当前阶段也只是待执行
                    eSt = CStageBar::EState::Pending;   break;
            }
        }
        CStageBar::SNode stN;
        stN.iStage = static_cast<int>(eStage);   // 右键「再次执行」路由用
        stN.sName  = sName;
        stN.eState = eSt;
        aNodes.append(stN);
    }
    m_pStageBar->SetStages(aNodes);
    // 2026.09.23 v3.1.227 无任务/空流不占位（用户需求：无任务时日志 Tab 顶上移）；
    //   实际任务流恒非空（ResolveFlow 兑底规范流），此处仅防空档。
    m_pStageBar->setVisible(!aNodes.isEmpty());

    UpdateCostLabel();   // 2026.08.15 费用/状态栏统一入口（含运行耗时）

    // #4 执行态指示器：记录运行态 + 状态色，刷新指示器。
    m_bTaskRunning = (a_pTask->Status() == ETaskStatus::Running);
    // 2026.08.15 运行中启动耗时 1s 刷新；离开运行态停表（冻结显示最后耗时）。
    //   2026.09.05 统一 SyncRunTimer：助手还在跑则定时器不因任务停而停（助手页内耗时继续跳）。
    SyncRunTimer();
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
    // 2026-10-02 无任务选中：阶段产物 Tab 整体隐藏（产物只属于任务）；2026-10-03
    //   日志页同口径并入（任务对话流只属于任务，无任务时连页都不留）。
    //   页面收尾由本函数尾部统一口径（选任务→日志页；置空→激活助手页），
    //   隐藏当前页不再需要中间切换（同步链内无事件处理，无悬空窗口期）。
    SetTaskTabsVisible(a_pTask != nullptr);
    if (m_pStageBar != nullptr) {
        m_pStageBar->SetStages({});
        // 2026.09.23 v3.1.227 用户需求：无任务选中时日志 Tab 顶上移到原任务进度位置——
        //   阶段条自隐藏不占布局（载任务路径由 RefreshTask/SetCurrentTask 下方恢复）。
        m_pStageBar->setVisible(a_pTask != nullptr);
    }
    // 2026.09.03 流呈现器接手：旧流解绑（弃件保数据+作废在途帧，含视图 Clear），
    //   新流绑共享视图并重建（初始窗口）；未选中任务则无流绑定（视图已清空）。
    if (m_pCurStream != nullptr) {
        m_pCurStream->Unbind();
    }
    m_pCurStream = nullptr;
    m_sCurId = a_pTask ? a_pTask->Id() : QString();
    m_pCurTask = a_pTask;   // 需求3：供阶段表右键「再次执行」取当前任务

    if (a_pTask != nullptr) {
        m_pCurStream = EnsureStream(a_pTask->Id());
        m_pCurStream->Bind(m_pView);
        RebuildView();
        RefreshTask(a_pTask);
    } else {
        UpdateCostLabel();             // 2026.09.05 无任务：任务侧费用栏置空（助手统计在各自页内）
        m_bTaskRunning = false;        // #4 无任务：指示器归空闲灰点
        SyncRunTimer();                // 2026.09.05 取消选中任务但助手仍在跑：定时器保持（助手耗时继续跳）
        m_colIdle = QColor(200, 200, 200);
        UpdateBusyIndicator();
        UpdateTaskInjectState();       // FR-3 无任务：禁用介入栏
    }
    // 2026-10-03 无任务隐藏任务侧控件（介入栏+费用栏），选任务恢复（用户需求：
    //   取消选中后这两行只占空白；任务运行态由任务列表图标呈现，不丢观测）。
    UpdateTaskBarsVisibility();
    LoadStageOutputs(a_pTask);   // 切换任务时同步刷新「阶段产物」页（nullptr 时清空）
    // 2026.09.10 v2.2.153/155 切换口径全句：「选任务=任务日志；无任务选中=助手日志」。
    //   载任务（启动/再次执行/点选任务列表任一路径）自动选中「日志」页——此前载助手档
    //   （LoadAssistantsFromJson 会激活首个助手 Tab）后停在助手日志，需各调用点散补
    //   ShowTaskLogPage，逻辑不通；清空（nullptr）反之切当前激活助手日志（取消选中/
    //   移除/删除任务后不再停在已清空的任务日志页）。无激活助手（异常态）保持原页。
    //   新建任务路径由调用方收尾 ShowActiveAssistantPage 覆盖（用户下一步是与助手对话）。
    if (a_pTask != nullptr) {
        m_pTabs->setCurrentWidget(m_pView);   // m_pView=「日志」页（任务对话流）
    } else {
        ShowActiveAssistantPage();
    }
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

void CExecutionPanel::SetTaskTabsVisible(bool a_bShow) {
    // 2026-10-03 任务侧 Tab（日志/阶段产物）显隐统一入口：两页只属于当前任务，
    //   无任务选中时连页都不留（用户口径：没选任务就不该看到任务日志页）。
    //   调方负责当前页收尾（SetCurrentTask 尾部：选任务→日志页；置空→激活助手页）。
    if (m_pTabs == nullptr || m_pAsstBar == nullptr) {
        return;
    }
    bool bChanged = false;
    QWidget* apPages[] = { m_pView, m_pArtifactsPage };
    for (QWidget* pPage : apPages) {
        if (pPage == nullptr) {
            continue;
        }
        const int iIdx = m_pTabs->indexOf(pPage);
        if (iIdx < 0 || m_pTabs->tabBar()->isTabVisible(iIdx) == a_bShow) {
            continue;
        }
        m_pTabs->tabBar()->setTabVisible(iIdx, a_bShow);
        bChanged = true;
    }
    if (bChanged) {
        m_pAsstBar->ApplyLayout();   // 2026-10-02 多行栏重排（可见集合变化）
    }
}

bool CExecutionPanel::TaskLogTabVisible() const {
    // 2026-10-03 观察口：日志页在 Tab 栏可见与否（测试/诊断用）。
    return m_pTabs != nullptr && m_pView != nullptr
        && m_pTabs->indexOf(m_pView) >= 0
        && m_pTabs->tabBar()->isTabVisible(m_pTabs->indexOf(m_pView));
}

void CExecutionPanel::ShowActiveAssistantPage() {
    // 2026.08.26 切到当前激活助手的 Tab：新建任务后用户下一步就是跟助手对话，
    //   直接落在助手日志页（2026.08.23 的「保持原页」不再适用，用户反馈不符合常理）。
    if (SAsst* pAsst = ActiveAsst()) {
        m_pTabs->setCurrentWidget(pAsst->pPage);
    }
}

// ===========================================================================
// 提示词 agent（按「助手」分 Tab 的独立对话流，#1/#2/#3/#4）
// 2026.08.22 多助手：块数据/控件/分帧状态全部下沉到各 SAsst。
// 2026.09.03 多助手并发：编排器信号带助手 id 直投对应助手流（不再按运行态猜测路由）；
//   运行态 per-助手（SAsst::bRunning），每个助手是独立 Agent（独立进程/会话链）。
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

QString CExecutionPanel::AsstDisplayName(const QString& a_sId) const {
    // 2026.09.18 退出确认提示用：助手名（未命中/已删除回退 id 本身）。
    const SAsst* pAsst = AsstById(a_sId);
    return (pAsst != nullptr && !pAsst->sName.isEmpty()) ? pAsst->sName : a_sId;
}

QStringList CExecutionPanel::BuildAsstTranscript(const QString& a_sAsstId,
                                                 const QString& a_sCurText) const {
    // 2026.09.02 跨轮上下文回灌（修复：DSH/Codex/Hermes 每轮全新会话，第 2 轮起
    //   模型完全失忆——实测用户发「作为默认值」时助手回「消息不完整」）。
    //   权威源 = 对话流块（含重启后从 assistant.json 恢复的历史），每次提交重建。
    //   预算：条目cap 40、用户行 1500 字、助手行 4000 字——首轮 guide 全文投喂的
    //   user 块与长回答截断保尾，防止回灌块无限膨胀挤占上下文。
    constexpr int k_iMaxEntries = 40;
    constexpr int k_iMaxUserChars = 1500;
    constexpr int k_iMaxAsstChars = 4000;
    const SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return QStringList();
    }
    static const QRegularExpression reStamp(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\]\\s*"));
    QStringList aLines;
    // 2026.09.07 v2.1.124 连发助手段只留末条（终答）：Hermes v2.0.116 中间段叙述以独立
    //   Assistant 块落流，逐条回灌会让历史像"未完成的现行任务"带偏模型（实测用户问
    //   「你现在用的是什么模型？」被带偏成继续执行 record.md 追加）——段内只保留
    //   最后一条（终答在末尾），中间叙述全部丢弃。用户块到达即冲刷上一段。
    QString sPendingAsst;
    auto fnFlushAsst = [&aLines, &sPendingAsst]() {
        if (!sPendingAsst.isEmpty()) {
            aLines.append(sPendingAsst);
            sPendingAsst.clear();
        }
    };
    // 2026.09.09 崩溃修复防御：取值快照（COW，廉价）而非引用绑定——防流对象在
    //   遍历期间被换档/重排后引用失效。
    const QList<SBlockData> aBlocks = pAsst->pStream ? pAsst->pStream->Blocks()
                                                     : QList<SBlockData>();
    for (const SBlockData& st : aBlocks) {
        QString sLine;
        if (st.eKind == CMessageBlock::Event && st.sEventKind == QStringLiteral("user")) {
            fnFlushAsst();   // 新一轮开始：上一轮的终答入列
            QString sBody = st.sText;
            sBody.remove(reStamp);   // 去 "[yyyy-MM-dd HH:mm:ss] " 前缀，省 token
            if (sBody.size() > k_iMaxUserChars) {
                sBody = sBody.left(k_iMaxUserChars) + QStringLiteral(" …[截断]");
            }
            sLine = QStringLiteral("用户：") + sBody;
        } else if (st.eKind == CMessageBlock::Assistant && !st.sText.trimmed().isEmpty()) {
            QString sBody = st.sText;
            if (sBody.size() > k_iMaxAsstChars) {
                sBody = sBody.left(k_iMaxAsstChars) + QStringLiteral(" …[截断]");
            }
            // 段内覆盖（不 append）：同轮叙述多条时只留最新一条，轮末自然成终答。
            sPendingAsst = QStringLiteral("助手：") + sBody;
            continue;
        }
        if (!sLine.isEmpty()) {
            aLines.append(sLine);
        }
    }
    fnFlushAsst();   // 尾部无后续 user 块的残段（如失败轮叙述）同样只留末条
    // 剔除本次即将提交的提问（调用前 user 块已上屏，prompt 尾部已含原文，防重复）。
    if (!aLines.isEmpty()) {
        const QString sCur = aLines.last();
        if (sCur.startsWith(QStringLiteral("用户："))
            && sCur.mid(3).trimmed() == a_sCurText.trimmed()) {
            aLines.removeLast();
        }
    }
    while (aLines.size() > k_iMaxEntries) {
        aLines.removeFirst();   // 超额丢最旧（保最新对话）
    }
    return aLines;
}

CExecutionPanel::SAsst* CExecutionPanel::ActiveAsst() {
    return AsstById(m_sActiveAsst);
}

// （RunningTargetAsst 已删：2026.09.03 起编排器信号带助手 id 直投；本地路径
//   （DoAdhocSend 等）提交目标=激活助手，不再有「运行中助手优先」的路由猜测。）

QString CExecutionPanel::AsstStampKey(const QString& a_sId) {
    // 助手流时间戳哨兵键：@@adhoc@@ + 助手 id（StampPrefix 依前缀路由到对应助手块列表）。
    static const QString kAdhoc = QStringLiteral("@@adhoc@@");
    return kAdhoc + a_sId;
}

// —— 助手侧流路由（2026.09.03 收拢：原 AppendAdhocBlock/RebuildAsstView/RebuildAdhocChunk/
//    BuildOlderChunk/PrependOlderBlocks/PrependAsstOlder/EnsureAsstCoverage 七个函数的实现
//    全部迁入 CStreamPresenter，面板只保留生命周期与钩子）——

CStreamPresenter* CExecutionPanel::AsstStream(SAsst& a_stAsst) {
    // 取/建助手流：fnDirty→sigAdhocBlocksChanged（#1 存盘信号，主窗口接手落 assistant.json）。
    //   懒建；SAsst 值拷贝搬移（ReopenAsst 等）经 shared_ptr 共享同一流，不产生双份。
    if (!a_stAsst.pStream) {
        a_stAsst.pStream = std::make_shared<CStreamPresenter>(
            [this]() { emit sigAdhocBlocksChanged(); });
    }
    return a_stAsst.pStream.get();
}

void CExecutionPanel::AppendAdhocBlock(const SBlockData& a_stData, const QString& a_sTargetId) {
    SAsst* pAsst = a_sTargetId.isEmpty() ? ActiveAsst() : AsstById(a_sTargetId);
    if (pAsst == nullptr) {
        return;   // 无助手（理论不达：始终保底一个助手）
    }
    // 2026.09.09 崩溃修复（发送提示词即闪退，dump 分析结论）：Append 内 fnDirty →
    //   sigAdhocBlocksChanged → SaveAssistantState 是同步重入链，链上若发生助手数组
    //   增删/重排（载档/撤销/关闭重开），QList<SAsst> 搬家后 pAsst 即悬空——链前取
    //   id 与流指针，链后不再解引用 pAsst。
    const QString sId = pAsst->sId;
    CStreamPresenter* pStream = AsstStream(*pAsst);
    pStream->Append(a_stData);
    // FR-3：新增用户提问时刷新提示词下拉索引（供半透明按钮下拉跳转；仅归属激活助手时）。
    if (sId == m_sActiveAsst &&
        a_stData.eKind == CMessageBlock::Event &&
        a_stData.sEventKind == QStringLiteral("user")) {
        RebuildPromptList();
    }
    // （落盘：Append 内 fnDirty 已 emit sigAdhocBlocksChanged，原尾处显式 emit 不再需要）
}

void CExecutionPanel::RebuildAdhocView() {
    // 按激活助手数据重建其视图（撤销/切档后调用）。
    RebuildAsstView(m_sActiveAsst);
}

void CExecutionPanel::RebuildAsstView(const QString& a_sId) {
    // 2026.08.23 按指定助手重建（多助手各自独立控件；加载后逐个预建，切换 Tab 即时呈现）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || pAsst->bClosed || pAsst->pView == nullptr) {
        return;
    }
    // 完成回调：激活助手刷新提示词下拉索引（FR-3）；其余助手无钩子。
    const QString sId = a_sId;
    AsstStream(*pAsst)->Rebuild([this, sId]() {
        if (sId == m_sActiveAsst) {
            RebuildPromptList();
        }
    });
}

void CExecutionPanel::OnAdhocAssistantText(const QString& a_sAsstId, const QString& a_sText) {
    SBlockData st;
    st.eKind = CMessageBlock::Assistant;
    // 2026.08.22 Agent 输出带时间前缀（用户提示词带日期+时间，见 DoAdhocSend）；
    //   距上个时间戳 <1 分钟不输出时间（StampPrefix）。哨兵键按助手 id 区分（多助手）。
    const QString sStamp = StampPrefix(AsstStampKey(a_sAsstId), /*a_bFullDate=*/false);
    st.sText = sStamp.isEmpty() ? a_sText : QStringLiteral("[%1] %2").arg(sStamp, a_sText);
    // 2026.09.02 产出后端随块入档（说话人标签用；到达时刻拍摄，切后端不改历史）。
    // 2026.09.23 修正：取该助手粘性后端（v2.4.193）而非全局——助手绑 PiAgent、全局
    //   切 Codex 后其输出块被错标成 CodexHarness（用户报告，与 sticky 提示自相矛盾）。
    //   未绑定兑底全局（SubmitAdhoc 首次提交即绑定，正常到块时已绑定）。
    const QString sSticky = AsstBackend(a_sAsstId);
    st.sBackend = sSticky.isEmpty()
        ? CAgentRunnerFactory::BackendDisplayName()
        : CAgentRunnerFactory::DisplayNameOf(sSticky);
    AppendAdhocBlock(st, a_sAsstId);   // 2026.09.03 id 直投（多助手并发各归各流）
}

void CExecutionPanel::OnAdhocToolUse(const QString& a_sAsstId, const QString& a_sTool,
                                     const QString& a_sInputJson) {
    SBlockData st;
    st.eKind  = CMessageBlock::ToolUse;
    st.sTool  = a_sTool;
    st.sInput = a_sInputJson;
    AppendAdhocBlock(st, a_sAsstId);
}

void CExecutionPanel::OnAdhocToolResult(const QString& a_sAsstId, const QString& a_sResult) {
    // 回填到该助手最近一个尚无结果的 ToolUse（流呈现代码与任务日志同源；助手块恒空视角）。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return;
    }
    if (AsstStream(*pAsst)->BackfillResult(a_sResult) >= 0) {
        return;   // 命中（流内已 fnDirty→sigAdhocBlocksChanged 落盘）
    }
    OnAdhocLog(a_sAsstId, a_sResult);   // 无未闭合工具卡：作为系统行兜底
}

void CExecutionPanel::OnAdhocLog(const QString& a_sAsstId, const QString& a_sLine) {
    SBlockData st;
    st.eKind = a_sLine.startsWith(QStringLiteral("[错误]"))
               ? CMessageBlock::Error
               : CMessageBlock::System;
    st.sText = a_sLine;
    AppendAdhocBlock(st, a_sAsstId);
}

void CExecutionPanel::OnAdhocEvent(const QString& a_sAsstId, const QString& a_sKind,
                                   const QString& a_sText) {
    // 2026.09.18 暂存提示词的实际处置（时序：emit sigAdhocSubmit 的同步链内到达，
    //   故「末条暂存」即刚提交的那条）：injected=注入立即生效 → 末条转正上屏；
    //   warn(请稍候)=提交被拒（宽限/重试期）→ 末条退回输入框重试。
    if (SAsst* pAsst = AsstById(a_sAsstId); pAsst != nullptr && !pAsst->aStaged.isEmpty()) {
        if (a_sKind == QStringLiteral("injected")) {
            const QPair<QString, QString> stStaged = pAsst->aStaged.takeLast();
            SBlockData stUser;
            stUser.eKind      = CMessageBlock::Event;
            stUser.sEventKind = QStringLiteral("user");
            stUser.sText      = stStaged.first;
            AppendAdhocBlock(stUser, a_sAsstId);
            RefreshQueueBox(a_sAsstId);
        } else if (a_sKind == QStringLiteral("warn")
                   && a_sText.contains(QStringLiteral("请稍候"))
                   && pAsst->pInput != nullptr) {
            const QPair<QString, QString> stStaged = pAsst->aStaged.takeLast();
            pAsst->pInput->setPlainText(stStaged.second);   // 退回输入框待重试
            pAsst->pInput->setFocus();
            RefreshQueueBox(a_sAsstId);
        }
    }
    SBlockData st;
    st.eKind      = CMessageBlock::Event;
    st.sEventKind = a_sKind;
    // 2026.09.19 done（已完成）带完成时间 + 当前提示词本轮总耗时（用户需求）：耗时锚点 =
    //   最近一次 SetAsstRunning(true)（每轮起点重锚）；tRound 无效/归零（异常路径/秒答）
    //   时只带时间。仅改展示文本，编排器语义不变；文本随对话流持久化到 assistant.json。
    if (a_sKind == QStringLiteral("done")) {
        const QString sNow = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        const SAsst* pAsst = AsstById(a_sAsstId);
        if (pAsst != nullptr && pAsst->tRound.isValid() && pAsst->tRound.elapsed() > 0) {
            st.sText = QStringLiteral("%1（%2，耗时 %3）")
                           .arg(a_sText, sNow, FormatElapsed(pAsst->tRound.elapsed()));
        } else {
            st.sText = QStringLiteral("%1（%2）").arg(a_sText, sNow);
        }
    } else {
        st.sText = a_sText;
    }
    AppendAdhocBlock(st, a_sAsstId);
}

void CExecutionPanel::OnAdhocPendingStarted(const QString& a_sAsstId, const QString& a_sText) {
    // 2026.09.18 排队指令被取出开始处理：按原始文本匹配首条暂存转正上屏
    //   （无匹配也渲染——面板侧暂存丢失（如测试/异常路径）时仍保证日志完整）。
    SAsst* pAsst = AsstById(a_sAsstId);
    QString sDisplay = a_sText;
    if (pAsst != nullptr) {
        for (int i = 0; i < pAsst->aStaged.size(); ++i) {
            if (pAsst->aStaged.at(i).second == a_sText) {
                sDisplay = pAsst->aStaged.takeAt(i).first;
                break;
            }
        }
        RefreshQueueBox(a_sAsstId);
    }
    SBlockData stUser;
    stUser.eKind      = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    stUser.sText      = sDisplay;
    AppendAdhocBlock(stUser, a_sAsstId);
}

void CExecutionPanel::OnAdhocPendingDropped(const QString& a_sAsstId) {
    // 2026.09.18 队列作废（撤销回退链/后端漂移会话链作废）：清空暂存框。
    if (SAsst* pAsst = AsstById(a_sAsstId); pAsst != nullptr && !pAsst->aStaged.isEmpty()) {
        pAsst->aStaged.clear();
        pAsst->bQueueExpanded = false;
        RefreshQueueBox(a_sAsstId);
    }
}

int CExecutionPanel::StagedCount(const QString& a_sId) const {
    const SAsst* pAsst = AsstById(a_sId);
    return (pAsst != nullptr) ? pAsst->aStaged.size() : 0;
}

void CExecutionPanel::RemoveStagedPrompt(const QString& a_sId, int a_iIndex) {
    // 2026.09.20 删除未发送的排队指令（暂存框行 ✕/右键菜单）：面板暂存出队 +
    //   信号同步编排器 aPendingInjects 同删除 + 日志留痕（与入队时的 queued 事件行对账）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || a_iIndex < 0 || a_iIndex >= pAsst->aStaged.size()) {
        return;
    }
    const QString sText = pAsst->aStaged.takeAt(a_iIndex).second;
    emit sigAdhocQueuedRemove(a_sId, sText);
    SBlockData stEv;
    stEv.eKind      = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("queued");
    stEv.sText      = QStringLiteral("已取消未发送的排队指令：%1").arg(sText.left(120));
    AppendAdhocBlock(stEv, a_sId);
    RefreshQueueBox(a_sId);
}

void CExecutionPanel::ApplyStagedEdit(const QString& a_sId, int a_iIndex,
                                      const QString& a_sNewText) {
    // 2026.09.20 改写未发送的排队指令：保留原展示文本的时间戳前缀（[HH:mm:ss] ），
    //   正文换新；信号按（旧文本→新文本）同步编排器侧；日志留痕。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || a_iIndex < 0 || a_iIndex >= pAsst->aStaged.size()
            || a_sNewText.trimmed().isEmpty()) {
        return;
    }
    const QString sOld = pAsst->aStaged.at(a_iIndex).second;
    QString sPrefix;
    const QString sOldDisp = pAsst->aStaged.at(a_iIndex).first;
    if (sOldDisp.startsWith(QLatin1Char('['))) {
        const int iSplit = sOldDisp.indexOf(QStringLiteral("] "));
        if (iSplit > 0) {
            sPrefix = sOldDisp.left(iSplit + 2);   // 含 "] "
        }
    }
    pAsst->aStaged[a_iIndex] = qMakePair(sPrefix + a_sNewText, a_sNewText);
    emit sigAdhocQueuedEdit(a_sId, sOld, a_sNewText);
    SBlockData stEv;
    stEv.eKind      = CMessageBlock::Event;
    stEv.sEventKind = QStringLiteral("queued");
    stEv.sText      = QStringLiteral("已修改未发送的排队指令：%1 → %2")
                          .arg(sOld.left(60), a_sNewText.left(60));
    AppendAdhocBlock(stEv, a_sId);
    RefreshQueueBox(a_sId);
}

void CExecutionPanel::EditStagedPrompt(const QString& a_sId, int a_iIndex) {
    // 2026.09.20 编辑入口（暂存框行右键/双击语义）：多行编辑框，取消零副作用。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || a_iIndex < 0 || a_iIndex >= pAsst->aStaged.size()) {
        return;
    }
    bool bOk = false;
    const QString sNew = QInputDialog::getMultiLineText(
        this, QStringLiteral("编辑排队指令"),
        QStringLiteral("修改后按「确定」生效（尚未发送，不影响已执行内容）："),
        pAsst->aStaged.at(a_iIndex).second, &bOk);
    if (!bOk || sNew.trimmed().isEmpty()) {
        return;
    }
    ApplyStagedEdit(a_sId, a_iIndex, sNew);
}

void CExecutionPanel::RefreshQueueBox(const QString& a_sId) {
    // 2026.09.18 暂存折叠框刷新（无暂存隐藏；仅该助手 Tab 当前可见时显示；右下角定位）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || pAsst->pQueueBox == nullptr
            || pAsst->pQueueHead == nullptr || pAsst->pQueueList == nullptr
            || pAsst->pQueueBtn == nullptr || pAsst->pView == nullptr) {
        return;
    }
    const bool bEmpty = pAsst->aStaged.isEmpty();
    if (bEmpty) {
        pAsst->bQueueExpanded = false;   // 隐藏同步收起，下次出现恢复缺省折叠态
    }
    // 当前 Tab 可见性（隐藏页上的框不显示，避免叠加在其他页上）。
    const bool bCurTab = (m_pTabs != nullptr && m_pTabs->currentWidget() == pAsst->pPage);
    pAsst->pQueueBox->setVisible(!bEmpty && bCurTab);
    if (bEmpty) {
        return;
    }
    // 头部：首条提示词（省略号截断；多条时尾级「共 N 条」）。
    const QString sFirst = pAsst->aStaged.first().second;
    const QString sHeadRaw = (pAsst->aStaged.size() > 1)
        ? QStringLiteral("%1 …（共 %2 条）").arg(sFirst, QString::number(pAsst->aStaged.size()))
        : sFirst;
    const QFontMetrics fm = pAsst->pQueueHead->fontMetrics();
    QString sHeadFlat = sHeadRaw;
    pAsst->pQueueHead->setText(fm.elidedText(
        sHeadFlat.replace(QChar('\n'), QChar(' ')), Qt::ElideRight, 300));
    pAsst->pQueueHead->setToolTip(sFirst);
    // 展开/折叠图标与列表。
    pAsst->pQueueBtn->setIcon(QIcon(pAsst->bQueueExpanded
        ? QStringLiteral(":/icons/qbox/q-collapse8.png")
        : QStringLiteral(":/icons/qbox/q-expand8.png")));
    // 2026.09.20 折叠态头部右键（编辑/删除第一条）：连接在 CreateAsstWidgets 一次性建
    //   （此处每次刷新重建会叠加连接）；仅折叠态弹，展开态用行内右键/✕。
    // 重建列表（展开态显示全部；折叠态收起）。
    if (auto* pOldLay = pAsst->pQueueList->layout()) {
        while (pOldLay->count() > 0) {
            QLayoutItem* pItem = pOldLay->takeAt(0);
            if (pItem->widget() != nullptr) {
                pItem->widget()->deleteLater();
            }
            delete pItem;
        }
    }
    if (pAsst->bQueueExpanded) {
        auto* pListLay = qobject_cast<QVBoxLayout*>(pAsst->pQueueList->layout());
        for (int iRow = 0; iRow < pAsst->aStaged.size(); ++iRow) {
            // 2026.09.20 每行 = [文本标签（右键菜单：编辑/删除）][✕ 删除钮]，
            //   行索引按快照捕获（框每次刷新整体重建，无陈旧索引）。
            auto* pRowW = new QWidget(pAsst->pQueueList);
            auto* pRowLay = new QHBoxLayout(pRowW);
            pRowLay->setContentsMargins(0, 0, 0, 0);
            pRowLay->setSpacing(2);
            auto* pLbl = new QLabel(pRowW);
            QString sOne = pAsst->aStaged.at(iRow).second;
            pLbl->setText(fm.elidedText(sOne.replace(QChar('\n'), QChar(' ')),
                                        Qt::ElideRight, 320));
            pLbl->setToolTip(pAsst->aStaged.at(iRow).second);
            pLbl->setStyleSheet(QStringLiteral("color:#374151;"));
            pLbl->setContextMenuPolicy(Qt::CustomContextMenu);
            const int iIdx = iRow;
            connect(pLbl, &QWidget::customContextMenuRequested,
                    this, [this, a_sId, iIdx, pLbl](const QPoint& a_stPos) {
                QMenu stMenu(pLbl);
                QAction* pActEdit = stMenu.addAction(QStringLiteral("编辑…"));
                QAction* pActDel  = stMenu.addAction(QStringLiteral("删除"));
                QAction* pSel = stMenu.exec(pLbl->mapToGlobal(a_stPos));
                if (pSel == pActEdit) {
                    EditStagedPrompt(a_sId, iIdx);
                } else if (pSel == pActDel) {
                    RemoveStagedPrompt(a_sId, iIdx);
                }
            });
            auto* pBtnDel = new QToolButton(pRowW);
            pBtnDel->setObjectName(QStringLiteral("queueDelBtn"));   // 测试观察口
            pBtnDel->setText(QStringLiteral("✕"));
            pBtnDel->setFixedSize(14, 14);
            pBtnDel->setAutoRaise(true);
            pBtnDel->setToolTip(QStringLiteral("删除该排队指令（未发送）"));
            connect(pBtnDel, &QToolButton::clicked,
                    this, [this, a_sId, iIdx]() { RemoveStagedPrompt(a_sId, iIdx); });
            pRowLay->addWidget(pLbl, 1);
            pRowLay->addWidget(pBtnDel, 0, Qt::AlignVCenter);
            pListLay->addWidget(pRowW);
        }
        pAsst->pQueueList->show();
    } else {
        pAsst->pQueueList->hide();
    }
    // 尺寸与右下角定位（11px 右距与浮层按钮同口径；底部留 8px）。
    pAsst->pQueueBox->adjustSize();
    const int iW = qMax(pAsst->pQueueBox->sizeHint().width(), 160);
    const int iH = pAsst->pQueueBox->sizeHint().height();
    pAsst->pQueueBox->resize(iW, iH);
    pAsst->pQueueBox->move(pAsst->pView->width() - iW - 11,
                           pAsst->pView->height() - iH - 8);
    pAsst->pQueueBox->raise();
}

void CExecutionPanel::OnAdhocUndo(const QString& a_sAsstId) {
    // 2026.09.03 多助手并发：撤销作用于指定助手的对话流（其编排器会话栈同源）。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return;
    }
    // 从末尾回溯找最后一个「用户提问」块，连同其后所有回复/工具/事件块一并移除。
    QList<SBlockData>& aBlocks = AsstStream(*pAsst)->Blocks();
    int iCut = -1;
    for (int i = aBlocks.size() - 1; i >= 0; --i) {
        if (aBlocks[i].eKind == CMessageBlock::Event &&
            aBlocks[i].sEventKind == QStringLiteral("user")) {
            iCut = i;
            break;
        }
    }
    if (iCut < 0) {
        return;   // 无提问块可撤销（编排器栈空时已 warn「没有可撤销的提问」，此处兜底）
    }
    while (aBlocks.size() > iCut) {
        aBlocks.removeLast();
    }
    if (a_sAsstId == m_sActiveAsst) {
        RebuildAdhocView();
    }
    emit sigAdhocBlocksChanged();   // #1 撤销后落盘
}

void CExecutionPanel::OnAdhocFinished(const QString& a_sAsstId) {
    // 该助手一轮结束：清其运行态（其他并发助手不受影响）。
    SetAsstRunning(a_sAsstId, false);
    // 2026-10-08 首条提示词「先改名后执行」补发：命名轮收尾（成功/失败同）→ 暂存的
    //   提示词（首条 + 命名期间追加的）逐条转正上屏，单条直发 / 多条合并一次投送
    //   （合并格式与编排器 MergeQueuedPrompts 同步，改动需两处同步）。先于
    //   bAutoNameQueued：挂起的右键改名会在补发后的新轮上再排队，语义不变。
    if (SAsst* pAsst = AsstById(a_sAsstId); pAsst != nullptr && pAsst->bSendAfterAutoName) {
        pAsst->bSendAfterAutoName = false;
        const int iSendPerm = pAsst->iSendAfterPerm;   // 转正链可能重排数组：先拍快照
        QStringList aTexts;
        for (const QPair<QString, QString>& stP : pAsst->aStaged) {
            if (!stP.second.trimmed().isEmpty()) {
                aTexts << stP.second;
            }
        }
        if (!aTexts.isEmpty()) {
            for (const QString& s : aTexts) {
                OnAdhocPendingStarted(a_sAsstId, s);   // 按文本匹配出队转正（含时间戳展示文本）
            }
            // 转正链（AppendAdhocBlock→fnDirty 载档重排）后重取：清残留 + 提交。
            if (SAsst* pA = AsstById(a_sAsstId)) {
                if (!pA->aStaged.isEmpty()) {   // 异常路径没匹配完的清掉，防孤儿滞留
                    pA->aStaged.clear();
                    pA->bQueueExpanded = false;
                    RefreshQueueBox(a_sAsstId);
                }
            }
            QString sSend = aTexts.value(0);
            if (aTexts.size() > 1) {
                sSend = QStringLiteral(
                    "用户在助手运行期间陆续提交了 %1 条排队指令。请在本轮一次性全部处理："
                    "按【指令 1】→【指令 %1】的顺序逐条执行完毕，全部完成后再统一收尾总结；"
                    "不要只执行其中一部分，也不要要求用户重发。").arg(aTexts.size());
                for (int i = 0; i < aTexts.size(); ++i) {
                    sSend += QStringLiteral("\n\n【指令 %1】\n%2").arg(i + 1).arg(aTexts.at(i));
                }
            }
            SetAsstRunning(a_sAsstId, true);   // 先复位运行态再提交（同步失败链收尾归零口径不变）
            emit sigAdhocSubmit(a_sAsstId, sSend, iSendPerm);
        }
    }
    // 2026-10-03 运行中挂起的自动重命名：当前指令已完成 → 现在执行（用户需求：助手
    //   运行期间请求的自动命名放到本轮之后；置位处见 TriggerAsstAutoRename）。
    //   先于 MaybeAutoNameOnFinish：挂起请求发射前置 bAutoNameTried，收尾自动命名
    //   让位（同一轮不双跑）；发射链同步起新命名轮 → bRunning 重建，下方外档幽灵
    //   检查（!bRunning）自然跳过，命名轮收尾再来一次 OnAdhocFinished 归零。
    if (SAsst* pAsst = AsstById(a_sAsstId); pAsst != nullptr && pAsst->bAutoNameQueued) {
        pAsst->bAutoNameQueued = false;
        TriggerAsstAutoRename(a_sAsstId);   // 非运行态 → 立即发射（空素材防御性忽略）
    }
    // 2026.09.26 名称仍是原始名（助手N）→ 收尾自动触发重命名（用户需求：工作结束时
    //   自动给当前助手改名；复用右键「自动重命名」链路）。防重入/防循环见函数头注。
    MaybeAutoNameOnFinish(a_sAsstId);
    // 2026-10-02 外档幽灵结束：不再运行且仍属外档 → 延迟一拍通知宿主落盘除名
    //   （singleShot 让自动续跑/改名子轮等同步链先走完；若链上已起新轮，bRunning
    //   已被 sigAdhocRunning 重置，此检不成立，幽灵继续跟随）。
    {
        const SAsst* p = AsstById(a_sAsstId);
        if (p != nullptr && AsstIsForeign(*p) && !p->bRunning) {
            QTimer::singleShot(0, this, [this, a_sAsstId]() {
                const SAsst* p2 = AsstById(a_sAsstId);
                if (p2 != nullptr && AsstIsForeign(*p2) && !p2->bRunning) {
                    emit sigAsstForeignFinished(a_sAsstId);
                }
            });
        }
    }
}

// 2026.09.26 判「原始名」：新建助手的缺省名 = 助手1..助手N（3281 行 QStringLiteral("助手%1")）。
//   严格全匹配（前后无多余字符/空格）；用户改过名（含自动改过）即不再自动触发。
static bool IsDefaultAsstName(const QString& a_sName) {
    static const QRegularExpression k_reDefault(
        QStringLiteral("^助手\\d{1,4}$"));
    return k_reDefault.match(a_sName.trimmed()).hasMatch();
}

void CExecutionPanel::MaybeAutoNameOnFinish(const QString& a_sAsstId) {
    // 触发条件：目标助手在席 + 名称仍是原始名（助手N）+ 本轮未尝试过 + 有素材。
    //   防循环关键：命名子轮收尾也发 sigAdhocFinished（借用执行会话退出运行态），
    //   命名失败时名称仍是原始名 → 靠 bAutoNameTried 拦住重入；用户再发提示词时
    //   （DoAdhocSend）清零，下一轮收尾可重试。手动右键「自动重命名」不受此限。
    //   2026-10-03 素材组装抽 BuildAsstAutoNameMaterial（与右键/挂起命名三处单源）。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr || pAsst->bAutoNameTried) {
        return;
    }
    if (!IsDefaultAsstName(pAsst->sName)) {
        return;
    }
    const QString sMaterial = BuildAsstAutoNameMaterial(pAsst);
    if (sMaterial.isEmpty()) {
        return;   // 没收到过提示词：无素材（理论不到这，收尾必有提问；防御）
    }
    pAsst->bAutoNameTried = true;   // 先置位再发射：发射链同步（无重入风险，防御式置位）
    emit sigAsstAutoRename(pAsst->sId, sMaterial);
}

QString CExecutionPanel::BuildAsstAutoNameMaterial(SAsst* a_pAsst) const {
    // 2026-10-03 命名素材组装（原 MaybeAutoNameOnFinish/右键菜单两处内联同款合并为
    //   单源）：Event/user 块、去时间戳前缀、按时间顺序编号拼接、总量截尾跟最近
    //   6000 字（旧提示词太长时保最新语义，防提示词膨胀）。返回空串 = 无素材。
    if (a_pAsst == nullptr || !a_pAsst->pStream) {
        return QString();
    }
    static const QRegularExpression k_reStamp(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\]\\s*"
                       "|^\\[\\d{2}:\\d{2}:\\d{2}\\]\\s*"));
    QStringList aPrompts;
    for (const SBlockData& b : a_pAsst->pStream->Blocks()) {
        if (b.eKind == CMessageBlock::Event
                && b.sEventKind == QStringLiteral("user")) {
            QString s = b.sText;
            s.remove(k_reStamp);
            if (!s.trimmed().isEmpty()) {
                aPrompts.append(s.trimmed());
            }
        }
    }
    if (aPrompts.isEmpty()) {
        return QString();
    }
    QStringList aKeep;
    int iTotal = 0;
    for (int i = aPrompts.size() - 1; i >= 0; --i) {
        const QString sLine = QStringLiteral("%1. %2")
                                  .arg(i + 1)
                                  .arg(aPrompts.at(i).left(300));
        iTotal += sLine.size();
        aKeep.prepend(sLine);
        if (iTotal >= 6000) { break; }
    }
    return aKeep.join(QLatin1Char('\n'));
}

bool CExecutionPanel::TriggerAsstAutoRename(const QString& a_sAsstId) {
    // 2026-10-03 用户请求「自动重命名」统一入口（右键菜单 + 运行中挂起收尾执行）：
    //   收集素材 → 助手运行中则置 bAutoNameQueued 挂起并在流内留事件行（用户需求：
    //   运行期间请求的自动命名放到当前指令完成后进行——不在运行中并发跑命名子轮：
    //   借用其他会话会换走该助手自己的粘性后端/链路，全忙时旧路径还直接失败）；
    //   空闲则置 bAutoNameTried 后立即发射（同原右键路径）。
    //   返回 false = 无素材/无该助手（调用方提示「没有素材」）。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return false;
    }
    const QString sMaterial = BuildAsstAutoNameMaterial(pAsst);
    if (sMaterial.isEmpty()) {
        return false;
    }
    if (pAsst->bRunning) {
        pAsst->bAutoNameQueued = true;
        OnAdhocEvent(a_sAsstId, QStringLiteral("auto_name_deferred"),
            QStringLiteral("⚙ 助手正在执行指令，自动重命名将在本轮完成后进行"));
        return true;
    }
    // 2026.09.26 手动路径同置尝试标记：命名轮收尾 sigAdhocFinished 早于改名生效，
    //   不置位会再触发一轮自动命名（双跑+双改名）。
    pAsst->bAutoNameTried = true;
    emit sigAsstAutoRename(a_sAsstId, sMaterial);
    return true;
}


void CExecutionPanel::SetAdhocContext(const QString& a_sAsstId, qint64 a_iFill) {
    // <0 表示撤销后未知；否则显示 当前占用 / 窗口。记录 fill，交由 UpdateContextButton 统一出文案。
    //   2026.09.05 per-助手：快照与进度条都是该助手自己的（非激活助手的更新也直刷其页内进度条）。
    SAsst* pOwner = AsstById(a_sAsstId);
    if (pOwner == nullptr) {
        return;
    }
    // 2026.08.15 旧档脏值钳制：历史存档可能带有远超窗口的假占用（result 累计兜底/中转脏 usage
    // 产物，7ce53172 实测 1.9M）。还原或收到此类值时按未知（—）处理，不再显示「1.9M（异常）」
    // 满档——否则每次落盘又把脏值写回 assistant.json，形成还原→显示→再落盘的黏住循环。
    if (a_iFill > 0 && a_iFill > AsstContextLimit(*pOwner)) {
        a_iFill = -1;
    }
    pOwner->iCtxFill = a_iFill;
    UpdateContextButton(*pOwner);
}

void CExecutionPanel::ShowContextMenu(const QString& a_sAsstId, const QPoint& a_stGlobalPos) {
    // 2026.08.29 上下文进度条菜单（右键 customContextMenuRequested 弹出；
    //   2026.09.11 左键单击入口已按用户要求取消，右键为唯一入口）：
    //   立即压缩（编排器「摘要→重播种」两步）+ 设置上下文窗口上限。
    //   2026.09.05 per-助手：菜单作用于目标助手自己的会话/进度条。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr || pAsst->pBtnContext == nullptr) {
        return;
    }
    // 2026.09.09 崩溃加固：exec() 为模态事件循环，期间用户可增删/关闭助手令 pAsst
    //   悬空——链前快照基准值，链后不再解引用 pAsst。
    const qint64 iLimitBase = AsstContextLimit(*pAsst);
    QMenu stMenu(pAsst->pBtnContext);
    QAction* pActCompact = stMenu.addAction(MenuIcon(QStringLiteral("ow-compress")), QStringLiteral("立即压缩上下文"));
    // 该助手运行中置灰「立即压缩」（压缩作用于空闲会话，运行中由任务编排自行管理）；
    //   bRunning 在 exec() 前取快照，模态循环内不解引用 pAsst（同下方 iLimitBase 口径）。
    pActCompact->setEnabled(!pAsst->bRunning);
    QAction* pActSet = stMenu.addAction(MenuIcon(QStringLiteral("ow-size")), QStringLiteral("设置上下文大小…"));
    // 2026-10-07 子菜单「选择模型」改为对话级覆盖（用户需求：不改缺省模型配置，仅
    //   本对话换模型）：列共享模型表全部条目，勾选 = 该助手生效模型（覆盖优先，无
    //   覆盖 = 全局当前项）；点选 = 设置/更换本助手覆盖（ApplyAsstModelOverride 落
    //   SAsst + 落盘 + sigAsstModelOverride 上抛编排器会话快照；下一轮即用新模型）。
    //   已设覆盖时首项「跟随全局设置…」一键清除。全局四张节/静态激活零改动——设置页
    //   仍是全局切换的唯一入口。claude 粘性助手不参与共享表（模型自报）不出子菜单。
    //   共享表未配置（空）时置灰提示去设置页；动作只捕 id/下标不捕 pAsst（exec 模态
    //   循环期间助手可被增删，同 iLimitBase 快照口径）。
    const QString sSnapId = a_sAsstId;
    const QString sSnapBe = pAsst->sBackend;
    // 2026-10-08 claude 后端同样开放（用户需求：设置里选的模型 claude 也能用；
    //   runner 侧仅 anthropic-messages 兼容条目实际注入，其余跳过并留痕）。
    QMenu* pMenuModels = stMenu.addMenu(QStringLiteral("选择模型（仅本助手）"));
    if (sSnapBe == QStringLiteral("claude")) {
        QAction* pActAnthropicHint = pMenuModels->addAction(
            QStringLiteral("（Claude 需 Anthropic 兼容端点的条目）"));
        pActAnthropicHint->setEnabled(false);
    }
    {
        if (m_aSharedModels.isEmpty()) {
            QAction* pActNone = pMenuModels->addAction(
                QStringLiteral("（未配置，请到 设置→「模型」页 添加）"));
            pActNone->setEnabled(false);
        } else {
            int iEff = m_iSharedActiveModel;   // 该助手生效下标：覆盖优先
            if (pAsst->bModelOv) {
                iEff = SharedModelIndexByIdentity(m_aSharedModels, pAsst->stModelOv);
                const QString sOvName = pAsst->stModelOv.sDisp.isEmpty()
                    ? pAsst->stModelOv.sModelId : pAsst->stModelOv.sDisp;
                QAction* pActFollow = pMenuModels->addAction(
                    iEff < 0
                        ? QStringLiteral("⚠ 覆盖条目「%1」已不在列表——跟随全局设置").arg(sOvName)
                        : QStringLiteral("跟随全局设置…"));
                connect(pActFollow, &QAction::triggered, this, [this, sSnapId]() {
                    ApplyAsstModelOverride(sSnapId, -1);
                });
                pMenuModels->addSeparator();
            }
            QActionGroup* pGrpModels = new QActionGroup(pMenuModels);
            for (int i = 0; i < m_aSharedModels.size(); ++i) {
                const SDshModelEntry& stE = m_aSharedModels.at(i);
                QString sTxt = stE.sDisp.isEmpty() ? stE.sModelId : stE.sDisp;
                if (!stE.sModelId.isEmpty() && stE.sDisp != stE.sModelId) {
                    sTxt = QStringLiteral("%1（%2）").arg(sTxt, stE.sModelId);
                }
                QAction* pActM = pGrpModels->addAction(sTxt);
                pActM->setCheckable(true);
                pActM->setChecked(i == iEff);
                const int iIdx = i;
                connect(pActM, &QAction::triggered, this, [this, sSnapId, iIdx]() {
                    ApplyAsstModelOverride(sSnapId, iIdx);
                });
            }
            pMenuModels->addActions(pGrpModels->actions());
        }
    }
    QAction* pSel = stMenu.exec(a_stGlobalPos);
    if (pSel == pActSet) {
        // 设置模型上下文窗口上限（千 token），覆盖按模型自动探测；持久化到 config.ini。
        bool bOk = false;
        const int iK = QInputDialog::getInt(
            this, QStringLiteral("设置上下文大小"),
            QStringLiteral("上下文窗口上限（千 token，范围 4~2000）："),
            static_cast<int>(qBound(qint64(4), iLimitBase / 1000, qint64(2000))),
            4, 2000, 1, &bOk);
        if (bOk && iK > 0) {
            SetUserContextLimit(static_cast<qint64>(iK) * 1000);
            emit sigAdhocContextLimitChanged(m_iUserContextLimit);
        }
    } else if (pSel == pActCompact) {
        emit sigAdhocCompact(a_sAsstId);
    }
}

// 2026.09.05 该助手窗口分母：用户设的全局上限优先，未设按该助手最近模型探测。
qint64 CExecutionPanel::AsstContextLimit(const SAsst& a_stAsst) const {
    return (m_iUserContextLimit > 0) ? m_iUserContextLimit
                                     : ContextLimitForModel(a_stAsst.sModel);
}

// 2026-10-07 显示口径：模型名去掉前面的 provider/进程名前缀（pi/DSH/Codex 回报的
//   model 字段形如 "autocode-2/glm-4.6"、"codex/gpt-5"，条上只显示模型本体）。
//   仅影响展示；上下文窗口探测（contains 匹配，前缀无关）/落档/回报仍用完整串。
static QString DisplayModelName(const QString& a_sModel) {
    const int iPos = a_sModel.lastIndexOf(QLatin1Char('/'));
    return iPos >= 0 ? a_sModel.mid(iPos + 1) : a_sModel;
}

void CExecutionPanel::UpdateContextButton(const SAsst& a_stAsst) {
    // 进度条显示「当前占用 / 模型上下文窗口」占比（2026.09.05 per-助手：入参助手的
    // 快照/模型/自己页内的进度条）；本体只画进度（绿≤70% / 红>70%），当前值/最大值
    // 走 tooltip。脏数据（fill>窗口）钳到满档红色并标注异常。
    QProgressBar* pBar = a_stAsst.pBtnContext;
    if (pBar == nullptr) {
        return;
    }
    const qint64 iLimit = AsstContextLimit(a_stAsst);
    const qint64 iFill = a_stAsst.iCtxFill;
    const QString sModel = a_stAsst.sModel;
    // #5 合理性校验：fill 应 ≤ 当前模型上下文窗口。GLM 等中转层偶发返回异常巨大的
    //   cache_read（实测一轮暴涨到 1.4M，远超 200K 窗口），超过窗口上限视为脏数据。
    const bool bUnknown = (iFill < 0);
    const bool bBogus = (iFill > iLimit);
    int iPct = 0;
    double dRatio = 0.0;
    QString sFillTxt;
    if (bUnknown) {
        sFillTxt = QStringLiteral("—");
    } else if (bBogus) {
        iPct = 100;
        dRatio = 1.0;
        sFillTxt = FormatTokens(iFill) + QStringLiteral("（异常）");
    } else {
        dRatio = iLimit > 0 ? static_cast<double>(iFill) / iLimit : 0.0;
        iPct = static_cast<int>(dRatio * 100.0);
        sFillTxt = FormatTokens(iFill);
    }
    // 2026.09.11 百分比 <10% 时保留 1 位小数（原整数截断致 0.7% 显示成 0%，观感像坏了）。
    const QString sPct = (iPct > 0 && iPct < 10)
                             ? QString::number(dRatio * 100.0, 'f', 1)
                             : QString::number(iPct);
    pBar->setValue(iPct);
    const bool bRed = bBogus || dRatio > 0.7;   // 超 70%（或脏数据）显红
    pBar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #d1d5db; border-radius: 3px;"
        " background: #f3f4f6; min-height: 20px; }"
        "QProgressBar::chunk { background: %1; }")
        .arg(bRed ? QStringLiteral("#ef4444") : QStringLiteral("#22c55e")));
    // 2026-10-05 本体显示当前模型名（用户需求）：优先该助手最近实际所用/刚切换的目标
    //   模型（sModel）；未跑过一轮且非 claude 后端 → 共享表当前条目（下一轮即将用的，
    //   主窗口 SetSharedModels 推送）；claude 后端未回报前未知 → 回落百分比（避免张冠李戴）。
    // 2026-10-07 对话级覆盖最高优先（右键「选择模型」仅本助手生效，下一轮即用）。
    QString sBarModel = DisplayModelName(
        a_stAsst.bModelOv ? a_stAsst.stModelOv.sModelId : sModel);
    if (sBarModel.isEmpty()) {
        const QString sEffBackend = a_stAsst.sBackend.isEmpty()
            ? CAgentRunnerFactory::BackendName() : a_stAsst.sBackend;
        if (sEffBackend != QStringLiteral("claude")
            && m_iSharedActiveModel >= 0 && m_iSharedActiveModel < m_aSharedModels.size()) {
            sBarModel = m_aSharedModels.at(m_iSharedActiveModel).sModelId;
        }
    }
    if (!sBarModel.isEmpty()) {
        // 宽度自适应（150~240）：典型模型名完整显示；高 DPI/超长名（如
        //   deepseek-v4-flash@150% 缩放）才截中段。计量用字体实测宽（DPI 安全），
        //   不依赖控件当前 geometry（未布局时 width() 还是默认值）。
        const int iNeed = qBound(150, pBar->fontMetrics().horizontalAdvance(sBarModel) + 18, 240);
        if (pBar->width() != iNeed) {
            pBar->setFixedWidth(iNeed);
        }
        pBar->setFormat(pBar->fontMetrics().elidedText(
            sBarModel, Qt::ElideMiddle, iNeed - 14));
    } else {
        if (pBar->width() != 150) {
            pBar->setFixedWidth(150);
        }
        pBar->setFormat(QStringLiteral("%p%"));
    }
    QString sTip = QStringLiteral("上下文占用：%1 / %2（%3%）\n（最后一条消息发出时的对话总 token，区别于统计栏的累计输入）")
                       .arg(sFillTxt, FormatTokens(iLimit), sPct);
    if (a_stAsst.bModelOv) {
        sTip += QStringLiteral("  ·  模型: %1（仅本助手，右键可换/恢复全局）")
                    .arg(a_stAsst.stModelOv.sModelId);
    } else if (!sModel.isEmpty()) {
        sTip += QStringLiteral("  ·  模型: %1").arg(DisplayModelName(sModel));
    }
    // 2026.09.26 上下文为 0 排障锚点：tooltip 直接亮出该助手实际生效的粘性后端——
    //   全局设置切了 Claude Code 不代表本助手在用（粘性后端 v2.4.193），用户对着
    //   0 值排查时无需再悬停 Tab 或翻事件日志。
    if (!a_stAsst.sBackend.isEmpty()) {
        sTip += QStringLiteral("\n后端：%1（粘性，全局切换不影响本助手）")
                    .arg(CAgentRunnerFactory::DisplayNameOf(a_stAsst.sBackend));
    }
    // 2026.08.29 可发现性：tooltip 注明菜单入口（此前从不提示，右键入口形同虚设）。
    //   2026.09.11 入口提示只写右键（左键菜单已取消）；仅文案变化时才 setToolTip
    //   （流式期间高频刷新，否则悬停中的 tooltip 会被反复重置导致显示闪烁）。
    sTip += QStringLiteral("\n右键：立即压缩上下文 / 设置上下文大小 / 选择模型");
    if (pBar->toolTip() != sTip) {
        pBar->setToolTip(sTip);
    }
}

void CExecutionPanel::OnAdhocModel(const QString& a_sAsstId, const QString& a_sModel) {
    // 该助手本轮所用模型名 → 更新其快照 + 上下文窗口分母（用户设了上限则覆盖探测）。
    //   2026.09.05 per-助手：直接刷该助手页内进度条（无需激活判断）。
    SAsst* pOwner = AsstById(a_sAsstId);
    if (pOwner == nullptr) {
        return;
    }
    pOwner->sModel = a_sModel;
    UpdateContextButton(*pOwner);
}

void CExecutionPanel::SetUserContextLimit(qint64 a_iLimit) {
    // 用户右键设的窗口上限：>0 生效（覆盖按模型探测），0/负=回退自动探测；改后即时刷新
    //   各助手页内的进度条（2026.09.05 per-助手：全局配置，逐助手重算分母）。
    m_iUserContextLimit = a_iLimit > 0 ? a_iLimit : 0;
    for (const SAsst& st : std::as_const(m_aAssts)) {
        UpdateContextButton(st);
    }
}

bool CExecutionPanel::ApplyAsstModelOverride(const QString& a_sAsstId, int a_iIdx) {
    // 2026-10-07 对话级模型覆盖唯一入口（右键菜单动作/单测直调）：a_iIdx 越界或 <0 =
    //   清除（跟随全局）。2026-10-08 起 claude 后端同样支持（env 注入，仅
    //   anthropic 兼容条目实际路由）。
    //   v3.3.273 崩溃加固（2026.09.09 同类雷，dump 实证）：函数链含三段同步重入
    //   （OnAdhocEvent→Append→落盘信号 / sigAsstLayoutChanged→SaveAssistantState /
    //   sigAsstModelOverride→编排器），任一链上助手数组搬家（载档/撤销/关闭重开）
    //   都会令 pAsst 悬空——此前「链后再读 pAsst->stModelOv 上抛」读到 freed 内存
    //   （QString d=-1）在 SetAsstModelOverride→QHash 插键拷贝时崩溃。守卫口径与
    //   AppendAdhocBlock 一致：**所有 pAsst 解引用集中在链前**，事件行/信号上抛
    //   只用 id 与链前取好的值快照，链后零解引用。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return false;
    }
    const bool bOn = (a_iIdx >= 0 && a_iIdx < m_aSharedModels.size());
    SDshModelEntry stE;
    if (bOn) {
        stE = m_aSharedModels.at(a_iIdx);
    }
    // —— 链前：全部字段写入 + 快照（此后不再碰 pAsst）——
    const bool bHadOv = pAsst->bModelOv;
    pAsst->bModelOv = bOn;
    pAsst->stModelOv = bOn ? stE : SDshModelEntry();
    if (bOn) {
        pAsst->sModel = stE.sModelId;   // 进度条模型名即时跟上（下一轮实际生效）
    } else if (m_iSharedActiveModel >= 0 && m_iSharedActiveModel < m_aSharedModels.size()) {
        // 2026-10-07 清除覆盖回跟全局：显示切到全局当前项（下一轮即将用的模型）。
        pAsst->sModel = m_aSharedModels.at(m_iSharedActiveModel).sModelId;
    }
    UpdateContextButton(*pAsst);        // 纯展示，无重入链
    const SDshModelEntry stOvOut = bOn ? stE : SDshModelEntry();
    const QString sOvName = stE.sDisp.isEmpty() ? stE.sModelId : stE.sDisp;
    // —— 链后：仅用 id 与快照 ——
    if (bOn) {
        OnAdhocEvent(a_sAsstId, QStringLiteral("model_override"),
            bHadOv
                ? QStringLiteral("⚙ 本助手模型覆盖已切换：%1（仅本助手生效，下一轮起用）").arg(sOvName)
                : QStringLiteral("⚙ 已设置本助手模型覆盖：%1（仅本助手生效，下一轮起用；全局缺省配置不变）").arg(sOvName));
    } else if (bHadOv) {
        OnAdhocEvent(a_sAsstId, QStringLiteral("model_override"),
            QStringLiteral("⚙ 已恢复跟随全局当前模型（本助手覆盖已清除）。"));
    }
    emit sigAsstLayoutChanged();   // 落盘（assistant.json modelOv 键）
    emit sigAsstModelOverride(a_sAsstId, stOvOut, bOn);
    return true;
}

QPair<SDshModelEntry, bool> CExecutionPanel::AsstModelOverride(const QString& a_sAsstId) const {
    // 2026-10-07 LoadOrchFromPanel 档恢复拉取（面板是覆盖的持久化正本）。
    const SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr || !pAsst->bModelOv) {
        return qMakePair(SDshModelEntry(), false);
    }
    return qMakePair(pAsst->stModelOv, true);
}

void CExecutionPanel::SetSharedModels(const QList<SDshModelEntry>& a_aModels, int a_iActive) {
    // 2026-10-05 主窗口推送共享模型表（启动/热重载/设置保存/右键切换后回推）：
    //   记表供右键「选择模型」菜单与进度条模型名兜底；非 claude 助手的模型名随当前项
    //   即时刷新（下一轮即用新模型，展示口径先行；claude 后端模型自报，不参与共享表）。
    m_aSharedModels = a_aModels;
    m_iSharedActiveModel = a_iActive;
    QString sNewModel;
    if (a_iActive >= 0 && a_iActive < a_aModels.size()) {
        sNewModel = a_aModels.at(a_iActive).sModelId;
    }
    for (int i = 0; i < m_aAssts.size(); ++i) {
        SAsst& st = m_aAssts[i];
        const QString sEffBackend = st.sBackend.isEmpty()
            ? CAgentRunnerFactory::BackendName() : st.sBackend;
        if (sEffBackend != QStringLiteral("claude") && !st.bModelOv
            && !sNewModel.isEmpty() && st.sModel != sNewModel) {
            st.sModel = sNewModel;   // 进度条模型名即时跟上切换（实际下一轮生效）
        }
        UpdateContextButton(st);   // 2026-10-07 有覆盖的助手显示自己的覆盖模型不被冲掉
    }
}

void CExecutionPanel::UpdateTaskBarsVisibility() {
    // 2026-10-03 任务侧底栏显隐统一入口：介入输入栏与费用栏均为当前任务专属控件，
    //   无任务选中时整体隐藏（不占纵向空间）；选任务即恢复（可用性仍由
    //   UpdateTaskInjectState 按运行态、UpdateCostLabel 按统计另行刷新）。
    const bool bShow = !m_sCurId.isEmpty();
    if (m_pTaskBarWidget != nullptr) {
        m_pTaskBarWidget->setVisible(bShow);
    }
    if (m_pCostBarWidget != nullptr) {
        m_pCostBarWidget->setVisible(bShow);
    }
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

void CExecutionPanel::UpdateAdhocInputState(const QString& a_sAsstId) {
    // 2026.09.05 per-助手输入栏：逐助手刷新其页内控件可用性/按钮态（空 id = 全量刷新）。
    //   该助手在跑：可输入追加（注入/续发）、撤销禁用、按钮「➤ 追加指令」或「⏹ 停止」；
    //   其空闲：全部可用、正常「发送」立即执行（其他助手在跑与否不影响本助手）。
    //   2026.09.11 上下文进度条恒可用（不再随运行禁用）：Qt 禁用控件不弹 tooltip、
    //   不发右键菜单，而进度条恰在运行中才最有观测价值——「运行中右键菜单消失/
    //   tooltip 不显示」即此因；进度条本体无输入语义，恒使能无害。
    //   构造期半初始化守卫：控件未建（已关闭助手 pPage=null）直接跳过。
    for (SAsst& st : m_aAssts) {
        if (!a_sAsstId.isEmpty() && st.sId != a_sAsstId) {
            continue;
        }
        if (st.pInput == nullptr || st.pBtnContext == nullptr
            || st.pBtnSend == nullptr || st.pBtnUndo == nullptr) {
            continue;
        }
        const bool bRun = st.bRunning;
        // 2026-10-02 外档幽灵：输入/发送/撤销全禁（其归属档不在当前视图，操作应回其
        //   任务下进行；点击 Tab 标题切回）。日志照常浏览。
        const bool bForeign = AsstIsForeign(st);
        const bool bOk = m_bAdhocEnabled && !bRun && !bForeign;
        st.pInput->setEnabled(m_bAdhocEnabled && !bForeign);
        st.pBtnUndo->setEnabled(bOk);
        if (bForeign) {
            st.pInput->setPlaceholderText(QStringLiteral(
                "该助手属于其他任务（正在后台运行）——点击上方 Tab 标题切回归属任务后操作"));
            st.pBtnSend->setText(QStringLiteral("发送"));
            st.pBtnSend->setToolTip(QString());
            st.pBtnSend->setEnabled(false);
            st.pBtnSend->setStyleSheet(QString());
            continue;
        }
        const bool bHasText = !st.pInput->toPlainText().trimmed().isEmpty();
        if (bRun) {
            st.pInput->setPlaceholderText(QStringLiteral(
                "向正在运行的助手追加指令（Ctrl+Enter 立即注入/排队续发；清空可停止）"));
        } else {
            st.pInput->setPlaceholderText(
                QStringLiteral("向助手输入指令（Ctrl+Enter 发送）"));
        }
        if (bRun && bHasText) {
            st.pBtnSend->setText(QStringLiteral("➤ 追加指令"));
            st.pBtnSend->setToolTip(
                QStringLiteral("向正在执行的助手轮追加指令（立即注入/排队续发）"));
            st.pBtnSend->setEnabled(true);
            st.pBtnSend->setStyleSheet(
                QStringLiteral("QPushButton{background:#2563eb;color:white;}"));
        } else if (bRun) {
            st.pBtnSend->setText(QStringLiteral("⏹ 停止"));
            st.pBtnSend->setToolTip(QStringLiteral("停止当前助手轮"));
            st.pBtnSend->setEnabled(true);
            st.pBtnSend->setStyleSheet(QStringLiteral("QPushButton{background:#dc2626;color:white;}"));
        } else {
            st.pBtnSend->setText(QStringLiteral("发送"));
            st.pBtnSend->setToolTip(QString());
            st.pBtnSend->setEnabled(bOk);
            st.pBtnSend->setStyleSheet(QString());
        }
    }
}

void CExecutionPanel::SetAdhocEnabled(bool a_bEnabled) {
    m_bAdhocEnabled = a_bEnabled;
    UpdateAdhocInputState();
}

void CExecutionPanel::AppendToAdhocInput(const QString& a_sText) {
    // FR-3：把资源管理器选中的路径追加到【激活助手】的输入框末尾（与已有内容之间留一个空格）。
    //   2026.09.05 per-助手输入栏：写入该助手自己的输入框并切到其 Tab。
    SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pInput == nullptr || a_sText.isEmpty()) {
        return;
    }
    m_pTabs->setCurrentWidget(pAsst->pPage);
    QString sCur = pAsst->pInput->toPlainText();
    if (!sCur.isEmpty() && !sCur.endsWith(QLatin1Char('\n')) &&
        !sCur.endsWith(QLatin1Char(' '))) {
        sCur += QLatin1Char(' ');
    }
    pAsst->pInput->setPlainText(sCur + a_sText);
    // 光标移到末尾并聚焦，便于直接续写指令。
    QTextCursor stCur = pAsst->pInput->textCursor();
    stCur.movePosition(QTextCursor::End);
    pAsst->pInput->setTextCursor(stCur);
    pAsst->pInput->setFocus();
}

void CExecutionPanel::SetAsstRunning(const QString& a_sAsstId, bool a_bRunning) {
    // 2026.09.03 多助手并发：运行态 per-助手（SAsst::bRunning），置/清互不影响其他助手。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return;
    }
    const bool bChanged = (pAsst->bRunning != a_bRunning);
    pAsst->bRunning = a_bRunning;
    if (a_bRunning) {
        // 2026.09.19 重锚本轮提示词起点：编排器每轮 StartAdhocStage 都广播
        //   sigAdhocRunning（自动续发/压缩时 bRunning 已 true、无状态跳变也要重锚），
        //   done 事件的「本轮耗时」以此为准——排队续发的第 2、3 条各自独立计时。
        pAsst->tRound.start();
    }
    if (!bChanged) {
        return;
    }
    // 助手耗时实时刷新（2026.09.05 per-助手钟）：起跑记该助手本轮起始钟（跨轮续发不
    //   重置钟——已跑时间经 AddAsstUsage 并入 iElapsedMs，钟停时收尾归集，防双计；
    //   并发助手各自计时，不再共用单钟互相污染）。
    if (a_bRunning) {
        if (!pAsst->tClock.isValid()) {
            pAsst->tClock.start();
        }
    } else {
        UpdateAsstCostLabel(*pAsst);   // 停跑即时冻结该助手耗时显示
    }
    SyncRunTimer();   // 任务或任一助手在跑 → 1s 定时器保持运转
    // 2026.09.05 v2.0.120 该助手页内执行态指示器随其起停（底部任务侧指示器只看任务，
    //   助手运行态不再驱动它——防「助手跑、任务闲」时底部转圈误读为任务在跑）。
    if (pAsst->pBusy != nullptr) {
        pAsst->pBusy->SetBusy(a_bRunning);
    }
    // #4 助手 Tab 标签提示运行态（●运行中）：统一走 RefreshAsstTabTexts。
    RefreshAsstTabTexts();
    UpdateAdhocInputState();
}

bool CExecutionPanel::AsstRunning(const QString& a_sAsstId) const {
    const SAsst* p = AsstById(a_sAsstId);
    return p != nullptr && p->bRunning;
}

bool CExecutionPanel::AnyAsstRunning() const {
    for (const SAsst& st : std::as_const(m_aAssts)) {
        if (st.bRunning) {
            return true;
        }
    }
    return false;
}

QStringList CExecutionPanel::RunningAsstIds() const {
    // 2026-10-02 孤儿 busy 看门狗：面板自认为运行中的助手 id——与编排器
    //   IsAsstAdhocRunning（会话存在且 runner 活着/宽限/重试）交叉核对，
    //   不一致即运行态孤儿（结束信号丢失），自动恢复输入栏。
    QStringList aIds;
    for (const SAsst& st : std::as_const(m_aAssts)) {
        if (st.bRunning) {
            aIds.append(st.sId);
        }
    }
    return aIds;
}



QString CExecutionPanel::AsstTabText(const SAsst& a_st) const {
    // 2026.09.19 ● 运行标记迁出文本：文本无法只给点单独着色，改为标题旁彩色闪点
    //   （SyncAsstTabDot 挂 QTabBar tabButton）；标题恒纯名，运行/空闲宽度不再跳变。
    return a_st.sName;
}

void CExecutionPanel::RefreshAsstTabTexts() {
    for (SAsst& st : m_aAssts) {
        const int iIdx = m_pTabs->indexOf(st.pPage);   // 2026.09.05 Tab 页本体=pPage 容器
        if (iIdx >= 0) {
            m_pTabs->setTabText(iIdx, AsstTabText(st));
            SyncAsstTabDot(st, iIdx);   // 2026.09.19 运行→挂彩色闪点 / 空闲→拆点
            // 2026-10-02 外档幽灵：栏内粗体 + tooltip 首行任务归属（点击切回）。
            const bool bForeign = AsstIsForeign(st);
            if (m_pAsstBar != nullptr) {
                m_pAsstBar->SetPageForeign(st.pPage, bForeign);
            }
            // 2026.09.20 粘性后端提示（Tab tooltip）；2026-09-22 自动重命名生成的说明置首行。
            QString sTip;
            if (bForeign) {
                sTip = (st.sOwnerName.isEmpty()
                            ? QStringLiteral("⚠ 该助手属于其他存档——正在后台运行，点击本页标题可切回")
                            : QStringLiteral("⚠ 任务《%1》的助手——正在后台运行，点击本页标题切回该任务")
                                  .arg(st.sOwnerName))
                     + QStringLiteral("\n");
            }
            if (!st.sDesc.isEmpty()) {
                sTip += st.sDesc + QStringLiteral("\n");
            }
            sTip += st.sBackend.isEmpty()
                ? QStringLiteral("后端：跟随全局设置（首次发送时绑定）")
                : QStringLiteral("后端：%1（粘性——全局切换不影响本助手；新建助手用新后端）")
                      .arg(CAgentRunnerFactory::DisplayNameOf(st.sBackend));
            m_pTabs->setTabToolTip(iIdx, sTip);
        }
    }
    // 改名/自动重命名等文本变化 → 多行栏重排分行（Qt6 QTabBar 无文本变化钩子，显式通知）
    if (m_pAsstBar != nullptr) {
        m_pAsstBar->NotifyTabsChanged();
    }
    // 闪点时钟由 CAsstTabBar 自管（SetPageRunning 内启停），此处无需再管
}

// —— 2026.09.19 Tab 运行态闪点（2026-09-22 随多行 Tab 栏改为栏内自绘）——
// 色序/节拍不变（绿→青→蓝→紫，500ms/拍）：由 CAsstTabBar 自绘并自管时钟（旧
//   QLabel 经 setTabButton 挂点方案在多行布局下会错位，已废）。

void CExecutionPanel::SyncAsstTabDot(SAsst& a_stAsst, int a_iIdx) {
    Q_UNUSED(a_iIdx);   // 栏以页指针为键，无需索引（拖动换位后仍稳定）
    if (m_pAsstBar != nullptr) {
        m_pAsstBar->SetPageRunning(a_stAsst.pPage, a_stAsst.bRunning);
    }
}

void CExecutionPanel::UpdateBusyIndicator() {
    // 2026.09.05 v2.0.120 任务侧指示器只看【任务】运行态：底部栏自 v2.0.118 起为任务侧
    //   专用（统计与指示器成对，用户口径「任务统计信息放在状态图标后面」），助手运行时
    //   此处转圈会误导「任务在跑」——各助手运行态由其页内自己的指示器呈现（SetAsstRunning）。
    if (m_pBusy == nullptr) {
        return;
    }
    m_pBusy->SetBusy(m_bTaskRunning);
    m_pBusy->SetIdleColor(m_colIdle);
}

// 2026.09.05 v2.0.120 执行态指示器观察口（测试/诊断）：per-助手页内指示器与任务侧底部指示器。
QWidget* CExecutionPanel::AsstBusyIndicator(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return (p != nullptr) ? p->pBusy : nullptr;
}

QWidget* CExecutionPanel::TaskBusyIndicator() const {
    return m_pBusy;
}

bool CExecutionPanel::BusyIndicatorSpinning(const QWidget* a_pW) {
    return a_pW != nullptr && static_cast<const CBusyIndicator*>(a_pW)->IsBusy();
}

void CExecutionPanel::DoAdhocSend(const QString& a_sAsstId) {
    // 发送目标助手的当前输入（Ctrl+Enter 或「发送」按钮调用）。
    // 2026.09.03 多助手并发：提交目标锥死「该助手」——其在跑 → 同助手追加（注入运行轮/
    //   排队续发，编排器内按后端分流）；其空闲 → 立即提交该助手自己的会话。
    // 2026.09.05 per-助手输入栏：id 显式传入（空 = 激活助手，测试兼容）；文本/只读开关
    //   均取自该助手自己的输入框与按钮。
    SAsst* pAsst = a_sAsstId.isEmpty() ? ActiveAsst() : AsstById(a_sAsstId);
    if (pAsst == nullptr || pAsst->pInput == nullptr) {
        return;
    }
    const QString sText = pAsst->pInput->toPlainText().trimmed();
    if (sText.isEmpty()) {
        return;
    }
    // 2026.09.26 用户发新提示词：清本轮自动改名尝试标记（若上轮自动改名失败，
    //   本轮收尾可再试一次；链后 pAsst 悬空风险同下，先置再走）。
    //   2026-10-08 命名挂起期间（bSendAfterAutoName）不清：防命名收尾 MaybeAutoNameOnFinish
    //   双跑改名（追加提示词不是新一轮，首条挂起的命名仍待生效）。
    if (!pAsst->bSendAfterAutoName) {
        pAsst->bAutoNameTried = false;
    }
    const QString a_sId = pAsst->sId;
    // 先本地渲染「用户提问」块（❯），与编排器撤销栈一一对应：撤销=移除本块及之后所有块。
    // 用户提示词带日期+时间前缀（Agent 输出只带时间，见 OnAssistantText）；
    //   距上个时间戳 <1 分钟不输出时间（StampPrefix，键按助手 id 区分）。
    SBlockData stUser;
    stUser.eKind      = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    const QString sStamp = StampPrefix(AsstStampKey(a_sId), /*a_bFullDate=*/true);
    const QString sDisplay = sStamp.isEmpty()
        ? sText : QStringLiteral("[%1] %2").arg(sStamp, sText);
    stUser.sText = sDisplay;
    // 2026.09.09 崩溃修复（发送提示词即闪退）：下方 AppendAdhocBlock 内含 fnDirty →
    //   SaveAssistantState 同步重入链，链上助手数组重排（载档/增删）会使 pAsst 悬空。
    //   链前把链后要用的控件/状态全部捕获（QPointer 判活 + 值拷贝），链后不再解引用 pAsst。
    QPointer<QWidget> pPage = pAsst->pPage;
    QPointer<QPlainTextEdit> pInput = pAsst->pInput;
    const int iPerm = (pAsst->pComboPerm != nullptr)
                          ? pAsst->pComboPerm->currentIndex() : 0;
    // 2026-10-08 首条提示词「先改名后执行」：助手名仍是缺省名（助手N）且从未收到过
    //   提示词 → 先按本条提示词触发自动命名（sigAsstAutoRename 同步链起命名轮），
    //   提示词只入暂存框，命名轮收尾（OnAdhocFinished）再统一转正补发——取代旧的
    //   「先执行、收尾才改名」。命名轮没起来（工程未开/切档拦截等）→ 退回旧路径。
    if (!pAsst->bRunning && !pAsst->bSendAfterAutoName
            && IsDefaultAsstName(pAsst->sName) && !AsstHasUserPrompt(*pAsst)) {
        pAsst->bAutoNameTried = true;   // 本轮命名已触发（拦收尾 MaybeAutoNameOnFinish 重跑）
        // 素材格式同 BuildAsstAutoNameMaterial 单行（此时提示词未入流，手工拼首行）。
        emit sigAsstAutoRename(a_sId, QStringLiteral("1. %1").arg(sText.left(300)));
        // 发射链（AutoNameAsstNow→StartAdhocStage→sigAdhocRunning）同步置运行态，
        //   且链上 fnDirty 可能重排助手数组（载档/增删）→ 重取后再用，防旧指针悬空。
        pAsst = AsstById(a_sId);
        if (pAsst == nullptr) {
            return;   // 链上助手被删：放弃本次提交（输入框未清，文本不丢）
        }
        if (pAsst->bRunning) {
            // 命名轮已起：挂起本条——暂存框可见、不提交（防注入污染命名轮）。
            pAsst->bSendAfterAutoName = true;
            pAsst->iSendAfterPerm     = iPerm;
            pAsst->aStaged.append(qMakePair(sDisplay, sText));
            RefreshQueueBox(a_sId);
            if (pPage != nullptr) {
                m_pTabs->setCurrentWidget(pPage);
            }
            if (pInput != nullptr) {
                pInput->clear();
            }
            return;
        }
        // 命名轮未起（无监听/同步失败）：退回旧路径照常执行；本轮收尾不再补命名
        //   （bAutoNameTried 已置），下次发送清零后按旧规则收尾重试。
    }
    // 2026.09.18 运行中发送的提示词不再直接上屏：先入该助手右下角暂存折叠框（缓存态），
    //   实际开始处理时才渲染正式用户块——注入立即生效（OnAdhocEvent "injected" 末条转正）
    //   或排队轮启动（sigAdhocPendingStarted 按文本匹配转正）；提交被拒（warn 稍候）
    //   末条退回输入框。空闲路径行为不变（立即上屏）。
    //   2026-10-08 命名挂起期间追加（bSendAfterAutoName）同样只暂存不提交——
    //   补发统一走命名收尾（OnAdhocFinished），不注入命名轮。
    const bool bDeferSend = pAsst->bSendAfterAutoName;   // 命名挂起：暂存后不走提交
    if (!pAsst->bRunning && !bDeferSend) {
        AppendAdhocBlock(stUser, a_sId);
    } else {
        pAsst->aStaged.append(qMakePair(sDisplay, sText));
        RefreshQueueBox(a_sId);
    }
    // #2 发送后自动切到该助手 Tab，让用户立即看到日志。
    if (pPage != nullptr) {
        m_pTabs->setCurrentWidget(pPage);
    }
    if (bDeferSend) {
        if (pInput != nullptr) {
            pInput->clear();
        }
        return;   // 运行态由命名轮持有；提交延迟到命名收尾
    }
    // #4 先进入运行态（助手 Tab 标 ●、发送变「停止」），再 emit 提交：
    // 若提交在同步链中即失败（如 claude 启动失败），OnAdhocFinished 的 SetAsstRunning(id,false)
    // 会落在 SetAsstRunning(id,true) 之后，最终态正确归零。
    SetAsstRunning(a_sId, true);
    emit sigAdhocSubmit(a_sId, sText, iPerm);
    if (pInput != nullptr) {
        pInput->clear();   // 清空 → textChanged → 高度回到单行
    }
}

void CExecutionPanel::AdjustInputHeight(QPlainTextEdit* a_pEdit, int a_iMaxLines) {
    // #3 输入框高度自适应通用版（介入栏 / 助手栏共用，保证两框初始高度与增长行为一致）：
    // 按软换行视觉行数从单行向上扩展，封顶 a_iMaxLines 行；超 a_iMaxLines 固定该高度
    // 并交由滚动条（ScrollBarAsNeeded）。清空回单行。
    // 2026.09.26 修「添加到聊天框后高度不足」：行数改按软换行视觉行估算——旧版按
    //   blockCount()（回车逻辑行）计，无回车的长文本（右键注入路径/长段）恒算 1 行
    //   → 框高不涨，文字堆在滚动条后面；且若只在外部注入后临时撑高，下一次回车
    //   blockCountChanged 会按逻辑行缩回 → 必须在本函数内统一按视觉行计。
    //   逐块 ceil(advance/可用宽) 手工估算，不依赖 documentLayout 布局时机
    //   （同旧注释口径，documentSize 在 QPlainTextEdit 上曾测高失效）；
    //   宽度不可用（≤0，未布局）时退回逻辑行数（每块 1 行）。
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
    const int iDocMargin = static_cast<int>(dDocMargin * 2.0 + 0.5);   // 上下各一份（左右同值）
    const int iSingle = iLineH + iFrame + iMargin + iDocMargin;
    const int iMax    = iSingle + (a_iMaxLines - 1) * iLineH;
    // 可用文本宽（左右：框 + 边距 + docMargin×2）——软换行每块可容纳的像素宽度
    const int iUsableW = a_pEdit->width() - iFrame - iMargin - iDocMargin;
    int iLines = 0;
    if (pDoc != nullptr) {
        for (QTextBlock stBlk = pDoc->begin(); stBlk.isValid(); stBlk = stBlk.next()) {
            int iBlkLines = 1;
            if (iUsableW > 0) {
                const int iAdv = fm.horizontalAdvance(stBlk.text());
                if (iAdv > 0) {
                    iBlkLines = qMax(1, (iAdv + iUsableW - 1) / iUsableW);
                }
            }
            iLines += iBlkLines;
            if (iLines > a_iMaxLines) {
                break;   // 早停（封顶后交滚动条）
            }
        }
    }
    if (iLines < 1) { iLines = 1; }
    int iH = iSingle + (qMin(iLines, a_iMaxLines) - 1) * iLineH;
    if (iH < iSingle) { iH = iSingle; }
    if (iH > iMax)    { iH = iMax; }
    a_pEdit->setFixedHeight(iH);
}

// 2026.08.25 判定助手是否“有用”：对话流中存在用户发送的提示词（Event/user 块）。
//   没有任何用户提示词的助手（只有系统/事件行或完全无内容）视为空助手。
bool CExecutionPanel::AsstHasUserPrompt(const SAsst& a_st) {
    // 2026.09.03 数据在流对象内（未建流=无块=空助手）
    if (!a_st.pStream) {
        return false;
    }
    const QList<SBlockData>& aBlocks = a_st.pStream->Blocks();
    return std::any_of(aBlocks.cbegin(), aBlocks.cend(),
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
    const QList<SBlockData>& aBlocks = pAsst->pStream ? pAsst->pStream->Blocks()
                                                       : QList<SBlockData>();
    for (int i = 0; i < aBlocks.size(); ++i) {
        if (aBlocks[i].eKind == CMessageBlock::Event &&
            aBlocks[i].sEventKind == QStringLiteral("user")) {
            m_aPromptIdx.append(i);
        }
    }
}

void CExecutionPanel::OnPromptPicked(int a_iBlockIdx) {
    // FR-3：点击下拉中某条提示词 → 把对应 User 提问块滚到助手日志最顶 + 横纵滚动条归零。
    // 反向分页：目标块可能早于前沿尚未建件 → 流内 EnsureCoverage 分轮补建后滚动到位。
    SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pView == nullptr) {
        return;
    }
    // 切到该助手 Tab，确保用户看到滚动结果。
    m_pTabs->setCurrentWidget(pAsst->pPage);
    CMessageView* pView = pAsst->pView;   // 流回调时捕获视图（不捕获 SAsst 指针，防搬移悬垂）
    AsstStream(*pAsst)->EnsureCoverage(a_iBlockIdx, [pView](CMessageBlock* a_pBlock) {
        if (a_pBlock != nullptr) {
            pView->ScrollBlockToTop(a_pBlock);
        }
    });
}


void CExecutionPanel::ShowFileTab(const QString& a_sAbsPath, bool a_bSwitchTab, int a_iLine) {
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
        if (m_pAsstBar != nullptr) {
            m_pAsstBar->NotifyTabsChanged();   // 换预览文件标题变宽/窄 → 多行重排
        }
    }
    m_pFileView->ShowFile(a_sAbsPath);
    // 2026-09-30 代码审查定位：加载后滚动到指定行并选中。
    if (a_iLine > 0) {
        m_pFileView->GotoLine(a_iLine);
    }
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

    // 2026.09.18 运行中暂存提示词折叠框（日志区右下角）：运行中发送的提示词先进这里，
    //   实际开始处理才渲染为正式用户块。缺省折叠只显示第一条；左侧 8×8 展开/折叠
    //   按钮（qbox/q-expand ▸ / q-collapse ▾）切换全部显示。无暂存时隐藏。
    QFrame* pQBox = new QFrame(a_stAsst.pView);
    pQBox->setObjectName(QStringLiteral("queueBox"));
    pQBox->setStyleSheet(QStringLiteral(
        "QFrame#queueBox { background: rgba(255,255,255,235);"
        " border: 1px solid #d1d5db; border-radius: 6px; }"));
    auto* pQLay = new QVBoxLayout(pQBox);
    pQLay->setContentsMargins(6, 4, 6, 4);
    pQLay->setSpacing(2);
    auto* pHeadRow = new QWidget(pQBox);
    auto* pHLay = new QHBoxLayout(pHeadRow);
    pHLay->setContentsMargins(0, 0, 0, 0);
    pHLay->setSpacing(4);
    QToolButton* pQBtn = new QToolButton(pHeadRow);
    pQBtn->setObjectName(QStringLiteral("queueExpandBtn"));   // 2026.09.20 测试观察口
    pQBtn->setFixedSize(14, 14);
    pQBtn->setIcon(QIcon(QStringLiteral(":/icons/qbox/q-expand8.png")));
    pQBtn->setIconSize(QSize(8, 8));
    pQBtn->setToolTip(QStringLiteral("展开/折叠全部暂存提示词"));
    pQBtn->setAutoRaise(true);
    QLabel* pQHead = new QLabel(pHeadRow);
    pQHead->setWordWrap(false);
    pHLay->addWidget(pQBtn, 0, Qt::AlignVCenter);
    pHLay->addWidget(pQHead, 1);
    QWidget* pQList = new QWidget(pQBox);
    auto* pQListLay = new QVBoxLayout(pQList);
    pQListLay->setContentsMargins(18, 0, 0, 0);   // 与首条文本列对齐（按钮 14 + 间距 4）
    pQListLay->setSpacing(1);
    pQList->hide();
    pQLay->addWidget(pHeadRow);
    pQLay->addWidget(pQList);
    pQBox->hide();
    a_stAsst.pQueueBox  = pQBox;
    a_stAsst.pQueueBtn   = pQBtn;
    a_stAsst.pQueueHead  = pQHead;
    a_stAsst.pQueueList  = pQList;
    connect(pQBtn, &QToolButton::clicked, this, [this, sAsstId]() {
        if (SAsst* pA = AsstById(sAsstId)) {
            pA->bQueueExpanded = !pA->bQueueExpanded;
            RefreshQueueBox(sAsstId);
        }
    });
    // 2026.09.20 折叠态头部右键：编辑/删除第一条（不必先展开；一次性连接，随框存活）。
    pQHead->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pQHead, &QWidget::customContextMenuRequested,
            this, [this, sAsstId, pQHead](const QPoint& a_stPos) {
        const SAsst* pA = AsstById(sAsstId);
        if (pA == nullptr || pA->aStaged.isEmpty() || pA->bQueueExpanded) {
            return;
        }
        QMenu stMenu(pQHead);
        QAction* pActEdit = stMenu.addAction(QStringLiteral("编辑第一条…"));
        QAction* pActDel  = stMenu.addAction(QStringLiteral("删除第一条"));
        QAction* pSel = stMenu.exec(pQHead->mapToGlobal(a_stPos));
        if (pSel == pActEdit) {
            EditStagedPrompt(sAsstId, 0);
        } else if (pSel == pActDel) {
            RemoveStagedPrompt(sAsstId, 0);
        }
    });
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
    // 2026-10-08 条目 tooltip 首行加提问日期：从原文提取时间戳前缀——带日期的直接用；
    //   仅 [HH:mm:ss] 按今天拼（跨零点回退一天，同 StampPrefix 启发）；无前缀（距上次
    //   日志 <60s 未打时间）不显示日期行。
    static const QRegularExpression k_reStampFull(
        QStringLiteral("^\\[(\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})\\]"));
    static const QRegularExpression k_reStampTime(
        QStringLiteral("^\\[(\\d{2}:\\d{2}:\\d{2})\\]"));
    const QDateTime tNow = QDateTime::currentDateTime();
    for (int i = 0; i < m_aPromptIdx.size(); ++i) {
        const int iBlock = m_aPromptIdx.at(i);
        const QList<SBlockData>& aBlk = pAsst->pStream ? pAsst->pStream->Blocks()
                                                       : QList<SBlockData>();
        if (iBlock >= 0 && iBlock < aBlk.size()) {
            QString sPrompt = aBlk[iBlock].sText;   // 拷贝后去时间戳（pAsst 为 const）
            QString sStamp;
            QRegularExpressionMatch mSt = k_reStampFull.match(sPrompt);
            if (mSt.hasMatch()) {
                sStamp = mSt.captured(1);
            } else if ((mSt = k_reStampTime.match(sPrompt)).hasMatch()) {
                QDateTime tSt(tNow.date(),
                              QTime::fromString(mSt.captured(1), QStringLiteral("HH:mm:ss")));
                if (tSt.isValid() && tSt > tNow) {
                    tSt = tSt.addDays(-1);
                }
                if (tSt.isValid()) {
                    sStamp = tSt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                }
            }
            m_pPromptFlyout->AddItem(i, sPrompt.remove(k_reStamp), sStamp);
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
    if (m_pFlyoutHideTimer != nullptr) {
        m_pFlyoutHideTimer->stop();   // 刚弹出：取消挂起的收起判定
    }
}

bool CExecutionPanel::IsCursorInsideFlyoutRegion() {
    // 区域 = 浮层窗体 ∪ 触发按钮 ∪ 当前可见 QToolTip（qtooltip_lbl 顶层标签，贴光标
    //   在浮层外沿展开，读全文时鼠标就在它上面）∪ 各 8px 容差（防快速斜穿缝隙误判）。
    const QPoint ptCursor = QCursor::pos();
    auto fnContains = [&ptCursor](QRect a_rc, int a_iSlack) {
        return a_rc.isValid()
            && a_rc.adjusted(-a_iSlack, -a_iSlack, a_iSlack, a_iSlack).contains(ptCursor);
    };
    if (fnContains(m_pPromptFlyout->frameGeometry(), 8)) {
        return true;
    }
    // 触发按钮（激活助手；浮层弹出期间锚定它）
    const SAsst* pAsst = ActiveAsst();
    if (pAsst != nullptr && pAsst->pBtnPrompts != nullptr
            && fnContains(pAsst->pBtnPrompts->frameGeometry(), 8)) {
        return true;
    }
    // 自建条目全文 tip 窗（鼠标穿透，读全文时鼠标就停在它上面）
    if (QWidget* pTipW = m_pPromptFlyout->TipWindow(); pTipW != nullptr && pTipW->isVisible()
            && fnContains(pTipW->frameGeometry(), 8)) {
        return true;
    }
    // 当前可见的系统 Tooltip 窗口（QTipLabel 顶层，其他控件的常规提示；
    //   frameGeometry 即屏坐标）
    for (QWidget* pW : QApplication::topLevelWidgets()) {
        if (pW != nullptr && pW->isVisible()
                && pW->objectName() == QLatin1String("qtooltip_label")) {
            if (fnContains(pW->frameGeometry(), 8)) {
                return true;
            }
        }
    }
    return false;
}

void CExecutionPanel::OnPromptBtnLeaved() {
    // FR-3：鼠标离开按钮 → 恢复 50% 透明；浮层改由收起定时器统一判定（离开按钮
    //   但未进浮层时旧规则会永久挂起不收，新规则 500ms 后按鼠标位收起）。
    const SAsst* pAsst = ActiveAsst();
    if (pAsst == nullptr || pAsst->pBtnPrompts == nullptr) {
        return;
    }
    if (m_pFlyoutHideTimer != nullptr && m_pPromptFlyout != nullptr
            && m_pPromptFlyout->isVisible()) {
        m_pFlyoutHideTimer->start();   // 鼠标可能正前往浮层/Tooltip：延迟 500ms 后按位置判定
    }
    if (auto* pEff = qobject_cast<QGraphicsOpacityEffect*>(pAsst->pBtnPrompts->graphicsEffect())) {
        pEff->setOpacity(0.5);
    }
}

bool CExecutionPanel::eventFilter(QObject* a_pWatched, QEvent* a_pEvent) {
    // 2026.09.26 输入框宽变重算高度：软换行视觉行数随宽度变化（窗口/分割条拖动后
    //   长文本可能需要更多/更少行）；上限行数经 "acMaxLines" 属性携带（任务框/各助手框
    //   创建处设置）。幂等：高度未变时不重设。
    if (a_pEvent->type() == QEvent::Resize) {
        if (auto* pEdit = qobject_cast<QPlainTextEdit*>(a_pWatched)) {
            const int iMaxLines = pEdit->property("acMaxLines").toInt();
            if (iMaxLines >= 1) {
                AdjustInputHeight(pEdit, iMaxLines);
            }
        }
    }
    // 2026.09.11 上下文进度条左键菜单已取消（用户反馈干扰右键使用、且右键才是唯一入口），
    //   相关 MouseButtonRelease 分支一并移除；进度条交互只剩右键 customContextMenuRequested。
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
    // FR-3：浮层 Enter/Leave → 维护收起定时器（2026.09.13 旧规则 Leave 即收改为
    //   500ms 后按鼠标位置判定，鼠标移到条目 Tooltip 上不再连带消失）。
    if (a_pWatched == m_pPromptFlyout
            && (a_pEvent->type() == QEvent::Enter || a_pEvent->type() == QEvent::Leave)) {
        if (m_pFlyoutHideTimer != nullptr) {
            if (a_pEvent->type() == QEvent::Enter) {
                m_pFlyoutHideTimer->stop();
            } else {
                m_pFlyoutHideTimer->start();   // 离开浮层：500ms 后鼠标仍不在区域内才收
            }
        }
    }
    // FR-3：助手视图 resize/show/hide → 半透明按钮贴到右上角；仅该助手 Tab 可见时显示按钮。
    if (a_pEvent->type() == QEvent::Resize || a_pEvent->type() == QEvent::Show ||
        a_pEvent->type() == QEvent::Hide) {
        for (SAsst& st : m_aAssts) {
            if (a_pWatched == st.pView && st.pBtnPrompts != nullptr) {
                const bool bCurTab =
                    (m_pTabs != nullptr && m_pTabs->currentWidget() == st.pPage);
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
                // 2026.09.18 暂存折叠框随视图尺寸钉在右下角（无暂存时 RefreshQueueBox 已隐藏）。
                if (st.pQueueBox != nullptr && !st.aStaged.isEmpty()) {
                    st.pQueueBox->move(
                        st.pView->width() - st.pQueueBox->width() - 11,
                        st.pView->height() - st.pQueueBox->height() - 8);
                    st.pQueueBox->setVisible(bCurTab && a_pEvent->type() != QEvent::Hide);
                    st.pQueueBox->raise();
                }
                break;
            }
        }
    }
    // #3 助手输入框（2026.09.05 per-助手）：Ctrl+Enter 发送该助手自己的输入
    //   （Enter 保留默认换行）；命中哪个助手的输入框就发哪个助手。
    if (a_pEvent->type() == QEvent::KeyPress) {
        for (SAsst& st : m_aAssts) {
            if (a_pWatched == st.pInput && st.pInput != nullptr) {
                auto* pKey = static_cast<QKeyEvent*>(a_pEvent);
                const int iKey = pKey->key();
                if ((iKey == Qt::Key_Return || iKey == Qt::Key_Enter) &&
                    (pKey->modifiers() & Qt::ControlModifier)) {
                    DoAdhocSend(st.sId);
                    return true;   // 吃掉，避免再插入换行
                }
                break;
            }
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


// 2026.08.22 多助手持久化：全部助手（含各自会话态快照）→ JSON 数组。
// 2026-10-07 对话级模型覆盖持久化（assistant.json "modelOv" 键；旧档无键 = 未设覆盖）。
static QJsonObject ModelOvToJson(const SDshModelEntry& a_stE) {
    QJsonObject o;
    o[QStringLiteral("disp")] = a_stE.sDisp;
    o[QStringLiteral("url")]  = a_stE.sBaseURL;
    o[QStringLiteral("key")]  = a_stE.sApiKey;
    o[QStringLiteral("api")]  = a_stE.sApi;
    o[QStringLiteral("id")]   = a_stE.sModelId;
    o[QStringLiteral("ctx")]  = static_cast<double>(a_stE.iCtx);
    o[QStringLiteral("max")]  = a_stE.iMaxTok;
    return o;
}
static SDshModelEntry ModelOvFromJson(const QJsonObject& a_o) {
    SDshModelEntry stE;
    stE.sDisp    = a_o.value(QStringLiteral("disp")).toString();
    stE.sBaseURL = a_o.value(QStringLiteral("url")).toString();
    stE.sApiKey  = a_o.value(QStringLiteral("key")).toString();
    stE.sApi     = a_o.value(QStringLiteral("api")).toString();
    stE.sModelId = a_o.value(QStringLiteral("id")).toString();
    stE.iCtx     = static_cast<qint64>(a_o.value(QStringLiteral("ctx")).toDouble());
    stE.iMaxTok  = a_o.value(QStringLiteral("max")).toInt();
    return stE;
}

QJsonArray CExecutionPanel::AssistantsToJson() const {
    QJsonArray aArr;
    // 2026.09.09 崩溃修复（UAF）：QList<SAsst> 迭代期间数组被增删/重排（同步重入链）
    //   会使迭代器悬空——先取 COW 快照，遍历期间原数组任何变更都会令快照独立（detach），
    //   始终遍历稳定副本。
    const QList<SAsst> aSnap = m_aAssts;
    for (const SAsst& st : aSnap) {
        if (st.sOwnerKey != m_sViewKey) {
            continue;   // 2026-10-02 外档幽灵不写入本档（各归各档，分组落盘由 AssistantsJsonOf）
        }
        QJsonObject o;
        o[QStringLiteral("id")]        = st.sId;
        o[QStringLiteral("name")]      = st.sName;
        o[QStringLiteral("desc")]      = st.sDesc;      // 2026-09-22 助手说明（自动重命名，tooltip）
        o[QStringLiteral("backend")]   = st.sBackend;   // 2026.09.20 粘性后端（v2.4.193）
        o[QStringLiteral("sid")]       = st.sSid;
        o[QStringLiteral("guideHash")] = st.sGuideHash;
        o[QStringLiteral("stack")]     = st.aStack;
        o[QStringLiteral("ctxFill")]   = static_cast<double>(st.iCtxFill);
        o[QStringLiteral("closed")]    = st.bClosed;   // 2026.08.23 已关闭（可从「▾」菜单恢复）
        // 2026-09-11 权限三态持久化（旧档无 perm 键 → 加载回 0＝自动审批，兼容）
        o[QStringLiteral("perm")]      = st.iPerm;
        // 2026-10-07 对话级模型覆盖（仅本助手生效的模型快照；未设覆盖不写键，旧档兼容）
        if (st.bModelOv) {
            o[QStringLiteral("modelOv")] = ModelOvToJson(st.stModelOv);
        }
        // 2026.08.27 助手独立流量记录（未选任务时累计，随档持久化）
        QJsonObject oUsage;
        oUsage[QStringLiteral("cost")]   = st.dCost;
        oUsage[QStringLiteral("in")]     = static_cast<double>(st.iInTok);
        oUsage[QStringLiteral("out")]    = static_cast<double>(st.iOutTok);
        oUsage[QStringLiteral("reqs")]   = st.iReqs;
        oUsage[QStringLiteral("ms")]     = static_cast<double>(st.iElapsedMs);
        o[QStringLiteral("usage")]     = oUsage;
        QJsonArray aBlocks;
        if (st.pStream) {   // 2026.09.03 数据在流对象内（未建流=无块）
            for (const SBlockData& stB : st.pStream->Blocks()) {
                aBlocks.append(BlockToJson(stB));
            }
        }
        o[QStringLiteral("blocks")] = aBlocks;
        aArr.append(o);
    }
    return aArr;
}

QJsonArray CExecutionPanel::AssistantsJsonOf(const QString& a_sOwnerKey) const
{
    // 2026-10-02 分组落盘：指定归属 key 的助手序列化（跨档幽灵的最新块回写其任务档）。
    //   与 AssistantsToJson 同构，仅过滤条件不同；不嵌 owner 字段（档文件即归属）。
    QJsonArray aArr;
    const QList<SAsst> aSnap = m_aAssts;
    for (const SAsst& st : aSnap) {
        if (st.sOwnerKey != a_sOwnerKey) {
            continue;
        }
        QJsonObject o;
        o[QStringLiteral("id")]        = st.sId;
        o[QStringLiteral("name")]      = st.sName;
        o[QStringLiteral("desc")]      = st.sDesc;
        o[QStringLiteral("backend")]   = st.sBackend;
        o[QStringLiteral("sid")]       = st.sSid;
        o[QStringLiteral("guideHash")] = st.sGuideHash;
        o[QStringLiteral("stack")]     = st.aStack;
        o[QStringLiteral("ctxFill")]   = static_cast<double>(st.iCtxFill);
        o[QStringLiteral("closed")]    = st.bClosed;
        o[QStringLiteral("perm")]      = st.iPerm;
        // 2026-10-07 对话级模型覆盖（仅本助手生效的模型快照；未设覆盖不写键，旧档兼容）
        if (st.bModelOv) {
            o[QStringLiteral("modelOv")] = ModelOvToJson(st.stModelOv);
        }
        QJsonObject oUsage;
        oUsage[QStringLiteral("cost")] = st.dCost;
        oUsage[QStringLiteral("in")]   = static_cast<double>(st.iInTok);
        oUsage[QStringLiteral("out")]  = static_cast<double>(st.iOutTok);
        oUsage[QStringLiteral("reqs")] = st.iReqs;
        oUsage[QStringLiteral("ms")]   = static_cast<double>(st.iElapsedMs);
        o[QStringLiteral("usage")]     = oUsage;
        if (st.pStream != nullptr) {
            QJsonArray aBlocks;
            for (const SBlockData& stB : st.pStream->Blocks()) {
                aBlocks.append(BlockToJson(stB));
            }
            o[QStringLiteral("blocks")] = aBlocks;
        }
        aArr.append(o);
    }
    return aArr;
}

QList<CExecutionPanel::SAsst> CExecutionPanel::TakeRunningForReload()
{
    // 2026-10-02 档重建前摘下全部运行中助手（跨档幽灵 + 本档运行者）：块在 pStream
    //   （shared）随值拷贝存活，页控件拆除重建（重开助手同口径——数据不随控件丢）。
    QList<SAsst> aOut;
    for (int i = m_aAssts.size() - 1; i >= 0; --i) {
        SAsst& st = m_aAssts[i];
        if (!st.bClosed && st.bRunning) {
            if (st.pPage != nullptr) {
                const int iIdx = m_pTabs->indexOf(st.pPage);
                if (iIdx >= 0) {
                    m_pTabs->removeTab(iIdx);
                }
                st.pPage->deleteLater();
                st.pPage = nullptr;
                st.pView = nullptr;
                st.pInput = nullptr;
                st.pComboPerm = nullptr;
                st.pBtnContext = nullptr;
                st.pBtnSend = nullptr;
                st.pBtnUndo = nullptr;
                st.pLblCost = nullptr;
                st.pLblLive = nullptr;
                st.pBusy = nullptr;
                st.pBtnPrompts = nullptr;
                st.pBtnBottom = nullptr;
                st.pQueueBox = nullptr;
                st.pQueueBtn = nullptr;
                st.pQueueHead = nullptr;
                st.pQueueList = nullptr;
            }
            aOut.prepend(st);   // prepend 保序（倒序遍历）
            m_aAssts.removeAt(i);
        }
    }
    UpdateTabCloseButtons();
    return aOut;
}

void CExecutionPanel::AttachForeignAssistants(QList<SAsst> a_aList, const QString& a_sTaskNameOf)
{
    // 2026-10-02 重建后挂回外档幽灵：块已在流内，页控件懒建（CreateAsstTab 建容器）。
    //   a_sTaskNameOf：新建幽灵（本次切换产生）补任务名；既有幽灵保留原名。
    for (SAsst& stIn : a_aList) {
        if (stIn.sOwnerName.isEmpty()) {
            stIn.sOwnerName = a_sTaskNameOf;
        }
        if (AsstById(stIn.sId) != nullptr) {
            continue;   // 同 id 已在（切回归属档同批载入）：本档版优先，不重入
        }
        m_aAssts.append(stIn);
        CreateAsstTab(m_aAssts.last());
    }
    UpdateTabCloseButtons();
    RefreshAsstTabTexts();
    UpdateAdhocInputState();
}

void CExecutionPanel::RemoveAsstEntry(const QString& a_sAsstId)
{
    // 2026-10-02 幽灵结束除名：宿主已把最终块落盘，拆 Tab+除项。
    for (int i = m_aAssts.size() - 1; i >= 0; --i) {
        if (m_aAssts.at(i).sId != a_sAsstId) {
            continue;
        }
        SAsst& st = m_aAssts[i];
        if (st.pPage != nullptr) {
            const int iIdx = m_pTabs->indexOf(st.pPage);
            if (iIdx >= 0) {
                m_pTabs->removeTab(iIdx);
            }
            st.pPage->deleteLater();
        }
        m_aAssts.removeAt(i);
    }
    UpdateTabCloseButtons();
}

void CExecutionPanel::DropAsstEntries(const QStringList& a_aIds)
{
    // 2026-10-02 档重载去重：存活版（TakeRunningForReload 摘下待挂回）与文件版同 id
    //   时，文件版让位（防重复 Tab/覆盖活动流）。
    for (const QString& sId : a_aIds) {
        for (int i = m_aAssts.size() - 1; i >= 0; --i) {
            if (m_aAssts.at(i).sId != sId) {
                continue;
            }
            SAsst& st = m_aAssts[i];
            if (st.pPage != nullptr) {
                const int iIdx = m_pTabs->indexOf(st.pPage);
                if (iIdx >= 0) {
                    m_pTabs->removeTab(iIdx);
                }
                st.pPage->deleteLater();
            }
            m_aAssts.removeAt(i);
        }
    }
}

bool CExecutionPanel::AsstForeignById(const QString& a_sAsstId, QString* a_psOwnerKey) const
{
    const SAsst* p = AsstById(a_sAsstId);
    if (p == nullptr || p->sOwnerKey == m_sViewKey) {
        return false;
    }
    if (a_psOwnerKey != nullptr) {
        *a_psOwnerKey = p->sOwnerKey;
    }
    return true;
}

void CExecutionPanel::LoadAssistantsFromJson(const QJsonArray& a_aAssts, const QString& a_sActiveId) {
    // 恢复：清掉现有助手 Tab，按数组逐个建助手 + Tab；激活 a_sActiveId（缺省第一个）。
    //   非激活助手的视图惰性重建（切到该 Tab 时 RebuildAdhocView 按数据补建控件）。
    //   用 ClearAsstTabs（不建默认助手）——旧实现走 ResetAssistants 会多出一个空「助手1」，
    //   每次点任务切档累积一个，档文件随之越长越大。
    ClearAsstTabs();
    bool bSetActive = false;
    QString sFirstId;
    bool bPurged = false;   // 2026.09.10 是否丢弃过遗留空关闭助手（丢弃 → 净化档回写）
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
        st.sBackend = o.value(QStringLiteral("backend")).toString();   // 2026.09.20 粘性后端
        st.sOwnerKey = m_sViewKey;   // 2026-10-02 归属本次载入的存档 key（外档幽灵经 Attach 另挂）
        st.sOwnerName.clear();
        st.sDesc = o.value(QStringLiteral("desc")).toString();   // 2026-09-22 助手说明（旧档无键回空）
        st.bClosed    = o.value(QStringLiteral("closed")).toBool(false);
        // 2026-09-11 权限三态恢复（旧档缺键回 0；越界值钳位）
        st.iPerm      = qBound(0, o.value(QStringLiteral("perm")).toInt(0), 2);
        // 2026-10-07 对话级模型覆盖恢复（旧档无键 = 未设；键存在且模型 id 非空才视为有效）
        {
            const QJsonObject oOv = o.value(QStringLiteral("modelOv")).toObject();
            st.stModelOv = ModelOvFromJson(oOv);
            st.bModelOv  = !st.stModelOv.sModelId.trimmed().isEmpty();
        }
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
        if (!aBlocks.isEmpty()) {   // 2026.09.03 块数据入流对象（AsstStream 懒建；这里直接建好再填）
            st.pStream = std::make_shared<CStreamPresenter>(
                [this]() { emit sigAdhocBlocksChanged(); });
            QList<SBlockData>& aDst = st.pStream->Blocks();
            for (const QJsonValue& vb : aBlocks) {
                if (vb.isObject()) {
                    aDst.append(BlockFromJson(vb.toObject()));
                }
            }
        }
        // 2026.09.10 旧档自愈：已关闭且无用户提示词的空助手（2026.08.25「关闭即舍弃」
        //   规则生效前的遗留/异常退出残档）在「▾」菜单里不可见（空助手被过滤），却一直
        //   占着编号——用户只见 助手1，点「+」却新增出 助手7。加载即丢弃，编号回归连续。
        if (st.bClosed && !AsstHasUserPrompt(st)) {
            bPurged = true;
            continue;
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
        if (bPurged) {
            emit sigAsstLayoutChanged();   // 档里全是遗留空项：重置结果同样回写净化档
        }
        return;
    }
    UpdateTabCloseButtons();
    // 激活指定助手（无指定/不存在/已关闭 → 第一个打开的，满足「旧任务打开第一个助手显示日志」）。
    const QString sAct = (bSetActive ? a_sActiveId : sFirstId);
    const SAsst* pAct = AsstById(sAct);
    if (pAct != nullptr) {
        m_pTabs->setCurrentWidget(pAct->pPage);   // → currentChanged → OnAsstTabCurrentChanged
    }
    // 2026.08.23 各助手独立日志控件：加载后对全部打开助手预建视图（分帧渐进，不卡 UI），
    //   切换 Tab 即时呈现，不再依赖首次切入时的同步重建（此前切换数百块历史会明显卡顿）。
    for (const SAsst& st : m_aAssts) {
        const bool bHasBlocks = st.pStream && !st.pStream->Blocks().isEmpty();
        if (!st.bClosed && bHasBlocks) {
            RebuildAsstView(st.sId);
        }
    }
    if (bPurged) {
        // 自愈落盘：净化后的助手列表立即回写档（sigAsstLayoutChanged → SaveAssistantState，
        //   写入目标 = 刚载入的档），遗留空关闭项自此永久出局，无需等下一次变更才清档。
        emit sigAsstLayoutChanged();
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

void CExecutionPanel::UpdateAsstBackend(const QString& a_sId, const QString& a_sBackend) {
    // 2026.09.20 粘性后端镜像回填（编排器为权威，存档用）；Tab tooltip 同步提示。
    if (SAsst* p = AsstById(a_sId)) {
        if (p->sBackend != a_sBackend) {
            p->sBackend = a_sBackend;
            RefreshAsstTabTexts();
        }
    }
}

QString CExecutionPanel::AsstBackend(const QString& a_sId) const {
    const SAsst* p = AsstById(a_sId);
    return p != nullptr ? p->sBackend : QString();
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
    const bool bWasAsst = (pKeep != nullptr) && (AsstByPage(pKeep) != nullptr);
    ClearAsstTabs();
    SAsst st;
    st.sId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    st.sName = QStringLiteral("助手1");
    st.sOwnerKey = m_sViewKey;   // 2026-10-02 归属当前视图档
    m_aAssts.append(st);
    CreateAsstTab(m_aAssts.last());
    m_sActiveAsst = st.sId;
    UpdateTabCloseButtons();
    if (bWasAsst || m_pTabs->currentWidget() == nullptr) {
        m_pTabs->setCurrentWidget(AsstById(m_sActiveAsst)->pPage);
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
    m_aPromptIdx.clear();
    m_bAsstTabsResetting = false;
    UpdateTabCloseButtons();   // 无助手页 → 顺手摘除「+」占位页
}

QJsonArray CExecutionPanel::TaskBlocksToJson(const QString& a_sTaskId) const {
    QJsonArray aArr;
    // 2026.09.03 数据在流对象内（const 路径不建流；无流=无块）
    if (const CStreamPresenter* pS = m_aStreams.value(a_sTaskId, nullptr)) {
        for (const SBlockData& st : pS->Blocks()) {
            aArr.append(BlockToJson(st));
        }
    }
    return aArr;
}

void CExecutionPanel::LoadTaskBlocksFromJson(const QString& a_sTaskId, const QJsonArray& a_aBlocks) {
    QList<SBlockData> aBlocks;
    for (const QJsonValue& v : a_aBlocks) {
        if (v.isObject()) {
            aBlocks.append(BlockFromJson(v.toObject()));
        }
    }
    EnsureStream(a_sTaskId)->SetBlocks(aBlocks);
    if (a_sTaskId == m_sCurId && m_pCurStream != nullptr) {
        RebuildView();   // 当前任务：重建视图呈现恢复的对话流
    }
}

void CExecutionPanel::ClearAllBlocks() {
    // 切工程：彻底清空内存对话流（任务 + 各助手），避免新旧工程数据混淆。
    //   2026.09.03 任务流整体销毁（Unbind 清件+作废在途帧）；助手侧随 ResetAssistants 一并重建。
    if (m_pCurStream != nullptr) {
        m_pCurStream->Unbind();   // 清共享视图上的块 + 断线（析构前先弃件）
    }
    qDeleteAll(m_aStreams);
    m_aStreams.clear();
    m_pCurStream = nullptr;
    m_sCurId.clear();
    m_aDirtyTaskIds.clear();
    if (m_pChatSaveTimer != nullptr) {
        m_pChatSaveTimer->stop();
    }
    m_pView->Clear();   // 双保险（Unbind 已清；保险无块残留）
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
    // 2026-09-22 多行 Tab 栏：「x」由栏自绘，哪些页可关由本面板裁决（仅助手页；
    //   日志/产物/「+」/预览无——基类内置关闭钮在栏 tabInserted 里已全部摘除）。
    if (m_pAsstBar != nullptr) {
        m_pAsstBar->SetClosableChecker([this](int a_iIndex) {
            return m_pTabs != nullptr && a_iIndex >= 0 && a_iIndex < m_pTabs->count()
                   && AsstByPage(m_pTabs->widget(a_iIndex)) != nullptr;
        });
    }
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
    // 2026.09.09 右键助手 Tab → 彻底删除菜单（eventFilter 只管滚轮，不吞右键）。
    m_pTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTabs->tabBar(), &QTabBar::customContextMenuRequested,
            this, &CExecutionPanel::OnAsstTabContextMenu);
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
        if (AsstByPage(m_pTabs->widget(i)) != nullptr) {
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
            if (pW != m_pAddTabPage && AsstByPage(pW) != nullptr) {
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
    SAsst* pAsst = AsstByPage(m_pTabs->widget(a_iIdx));
    if (pAsst != nullptr) {
        CloseAsst(pAsst->sId);
    }
}

CExecutionPanel::SAsst* CExecutionPanel::AsstByPage(QWidget* a_pPage) {
    // 2026.09.05 Tab 页本体 = pPage 容器（日志+输入栏+统计栏）；按页反查助手。
    for (SAsst& st : m_aAssts) {
        if (st.pPage != nullptr && st.pPage == a_pPage) {
            return &st;
        }
    }
    return nullptr;
}

CMessageView* CExecutionPanel::CreateAsstTab(SAsst& a_stAsst, int a_iPos) {
    // 建助手页（容器 = 日志视图 + 输入栏 + 统计栏）+ 右上角提示词按钮 + 插 Tab
    // （默认插在最后一个助手 Tab 右侧）。2026.09.05 per-助手控件：Tab 页本体是 pPage
    // 容器，输入框/按钮/统计标签全部随页生灭，切换 Tab 即切换整套实例。
    CreateAsstWidgets(a_stAsst);
    int iIdx = (a_iPos >= 0) ? a_iPos : m_pTabs->count();
    if (a_iPos < 0) {
        // 默认：插到最后一个助手 Tab 之后（文件预览页保持在最右）。
        for (int i = m_pTabs->count() - 1; i >= 0; --i) {
            if (AsstByPage(m_pTabs->widget(i)) != nullptr) {
                iIdx = i + 1;
                break;
            }
        }
    }
    m_pTabs->insertTab(iIdx, a_stAsst.pPage, AsstTabText(a_stAsst));   // 标题恒纯名；运行闪点统一 SyncAsstTabDot
    UpdateTabCloseButtons();
    SyncAsstTabDot(a_stAsst, m_pTabs->indexOf(a_stAsst.pPage));   // 2026.09.19 新建/重开的助手 Tab 若已在跑立即画闪点
    return a_stAsst.pView;
}

// 2026.09.05 per-助手控件组建页（用户需求：提示词/发送按钮/统计信息全部放助手 Tab 页内，
//   切换助手一并切换、独立实例）：结构 = [日志视图(拉伸)] + [输入框|只读|上下文|发送|撤销]
//   + [统计栏|本轮实时]。信号按助手 id 捕获路由（助手删除/关闭后控件销毁即断线）。
void CExecutionPanel::CreateAsstWidgets(SAsst& a_stAsst) {
    const QString sId = a_stAsst.sId;
    a_stAsst.pPage = new QWidget(this);
    auto* pV = new QVBoxLayout(a_stAsst.pPage);
    pV->setContentsMargins(0, 0, 0, 0);
    pV->setSpacing(4);

    // —— 日志视图（占主体）——
    a_stAsst.pView = new CMessageView(a_stAsst.pPage);
    // 2026.09.03 流呈现器接管接线（到顶补建/丢块同步等在流内，与任务日志同码）：
    //   建视图即绑流（重开助手复绑同一流，Bind 内先解旧）；字体设置在绑后不影响。
    AsstStream(a_stAsst)->Bind(a_stAsst.pView);
    QFont fView = a_stAsst.pView->font();
    fView.setPointSize(11);
    a_stAsst.pView->setFont(fView);
    CreatePromptButton(a_stAsst);
    pV->addWidget(a_stAsst.pView, 1);

    // —— 输入栏：输入框 + 权限下拉 + 上下文进度条 + 发送 + 撤销 ——
    auto* pBar = new QHBoxLayout();
    pBar->setContentsMargins(0, 0, 0, 0);
    a_stAsst.pInput = new QPlainTextEdit(a_stAsst.pPage);
    a_stAsst.pInput->setPlaceholderText(QStringLiteral("向助手输入指令（Ctrl+Enter 发送）"));
    a_stAsst.pInput->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);   // 超 10 行显示滚动条
    a_stAsst.pInput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    a_stAsst.pInput->setTabChangesFocus(true);   // Tab 切焦点而非缩进，避免偷键盘焦点
    a_stAsst.pInput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    { QFont fIn = a_stAsst.pInput->font(); fIn.setPointSize(11); a_stAsst.pInput->setFont(fIn); }
    a_stAsst.pInput->installEventFilter(this);   // Ctrl+Enter 发送（Enter 留作换行）
    // 高度自适应（#3）：blockCountChanged 回车必触发、值即时准确；textChanged 刷按钮态
    //（运行中输入有字 → 「➤ 追加指令」）。
    constexpr int k_iAdhocMaxLines = 10;
    QPlainTextEdit* const pEdit = a_stAsst.pInput;
    pEdit->setProperty("acMaxLines", k_iAdhocMaxLines);   // Resize 重算上限（见 eventFilter）
    connect(pEdit, &QPlainTextEdit::blockCountChanged, this,
            [pEdit](int) { AdjustInputHeight(pEdit, k_iAdhocMaxLines); });
    connect(pEdit, &QPlainTextEdit::textChanged, this, [this, sId, pEdit]() {
        UpdateAdhocInputState(sId);
        // 2026.09.26 软换行视觉行计高：右键「添加到聊天框」注入无回车长文本后长高
        //   （旧版只接 blockCountChanged——无回车则高度不涨，文字堆在滚动条后面）。
        AdjustInputHeight(pEdit, k_iAdhocMaxLines);
    });
    AdjustInputHeight(pEdit, k_iAdhocMaxLines);   // 初始单行高度

    // 2026-09-11 权限下拉（替代原「只读」勾选钮，三态见 CTaskOrchestrator::EAdhocPerm）：
    //   缺省自动审批；选择随助手持久化（assistant.json "perm"），切页/重开/重启回显。
    a_stAsst.pComboPerm = new QComboBox(a_stAsst.pPage);
    a_stAsst.pComboPerm->addItem(QStringLiteral("自动审批"));
    a_stAsst.pComboPerm->addItem(QStringLiteral("手工审批"));
    a_stAsst.pComboPerm->addItem(QStringLiteral("只读"));
    a_stAsst.iPerm = qBound(0, a_stAsst.iPerm, 2);
    a_stAsst.pComboPerm->setCurrentIndex(a_stAsst.iPerm);
    a_stAsst.pComboPerm->setToolTip(QStringLiteral(
        "本轮提交给助手的权限模式（随助手记忆）：\n"
        "自动审批（缺省）：工具全开、不询问，助手直接读写执行；\n"
        "手工审批：允许改动，但任何改工程/有影响的操作前须先征得你同意"
        "（所有后端都会弹窗：Claude Code 走 ask_user；DSH/Pi/Codex/Hermes 助手运行 "
        "--ask 弹窗命令，窗口由本程序弹出）；\n"
        "只读：仅查询分析，禁改工程（Claude/Pi 后端工具硬禁，"
        "DSH/Codex/Hermes 由提示词约束），且不拍快照/不沉淀经验"));
    { QFont fPm = a_stAsst.pComboPerm->font(); fPm.setPointSize(11); a_stAsst.pComboPerm->setFont(fPm); }
    QComboBox* const pComboPm = a_stAsst.pComboPerm;
    connect(pComboPm, &QComboBox::currentIndexChanged, this, [this, sId](int a_iIdx) {
        SAsst* pA = AsstById(sId);   // 关闭重开链中控件重建不会触发（setCurrentIndex 不变不发射）
        if (pA != nullptr && pA->iPerm != a_iIdx) {
            pA->iPerm = a_iIdx;
            emit sigAsstLayoutChanged();   // 权限选择落盘 assistant.json（同重命名口径）
        }
    });

    // 上下文占比进度条：本体只画进度（绿≤70% / 红>70%），当前值/最大值走 tooltip；
    // 右键「立即压缩 / 设置大小」（2026.09.05 per-助手：作用于该助手自己的会话；
    //   2026.09.11 左键菜单取消，右键为唯一入口，不再装 eventFilter）。
    a_stAsst.pBtnContext = new QProgressBar(a_stAsst.pPage);
    a_stAsst.pBtnContext->setRange(0, 100);
    // 2026-10-05 本体显示当前模型名（用户需求：模型名直接可见，不再只藏 tooltip）：
    //   文本由 UpdateContextButton 统一出（模型名优先，未知回落百分比）；宽度
    //   110→150 容常见模型 id，超长经 elidedText 截中段。
    a_stAsst.pBtnContext->setTextVisible(true);
    a_stAsst.pBtnContext->setFormat(QStringLiteral("%p%"));
    // 2026-10-07 文字纵向居中（用户反馈：模型名偏上）：QSS min-height 拉高控件后
    //   默认样式文字按内容区顶部对齐，显式 AlignCenter 纵横居中。
    a_stAsst.pBtnContext->setAlignment(Qt::AlignCenter);
    a_stAsst.pBtnContext->setFixedWidth(150);
    { QFont fCx = a_stAsst.pBtnContext->font(); fCx.setPointSize(11); a_stAsst.pBtnContext->setFont(fCx); }
    a_stAsst.pBtnContext->setContextMenuPolicy(Qt::CustomContextMenu);
    QProgressBar* const pCtxBar = a_stAsst.pBtnContext;
    connect(pCtxBar, &QWidget::customContextMenuRequested, this,
            [this, sId, pCtxBar](const QPoint& a_stPos) {
        ShowContextMenu(sId, pCtxBar->mapToGlobal(a_stPos));
    });

    a_stAsst.pBtnUndo = new QPushButton(QStringLiteral("↶ 撤销"), a_stAsst.pPage);
    a_stAsst.pBtnUndo->setToolTip(QStringLiteral("撤销最后一条提问"));
    { QFont fUn = a_stAsst.pBtnUndo->font(); fUn.setPointSize(11); a_stAsst.pBtnUndo->setFont(fUn); }
    a_stAsst.pBtnSend = new QPushButton(QStringLiteral("发送"), a_stAsst.pPage);
    { QFont fSd = a_stAsst.pBtnSend->font(); fSd.setPointSize(11); a_stAsst.pBtnSend->setFont(fSd); }
    pBar->addWidget(a_stAsst.pInput, 1);
    pBar->addWidget(a_stAsst.pComboPerm);
    pBar->addWidget(a_stAsst.pBtnContext);
    // 2026.08.23 布局口径：发送在前、↶ 撤销在后。
    pBar->addWidget(a_stAsst.pBtnSend);
    pBar->addWidget(a_stAsst.pBtnUndo);
    pV->addLayout(pBar);

    // 发送：Ctrl+Enter（eventFilter 拦截）或点击「发送」；该助手运行中点击 = 停止或追加指令。
    QPushButton* const pSend = a_stAsst.pBtnSend;
    connect(pSend, &QPushButton::clicked, this, [this, sId]() {
        SAsst* pA = AsstById(sId);
        if (pA == nullptr) {
            return;
        }
        if (pA->bRunning) {
            if (pA->pInput != nullptr
                && !pA->pInput->toPlainText().trimmed().isEmpty()) {
                DoAdhocSend(sId);   // 同助手运行中追加：编排器按后端注入/排队续发
                return;
            }
            // 2026-10-08 命名挂起期间点「停止」：撤挂起，暂存提示词退回输入框（多条合
            //   并为一段，用户可改后重发），再停命名轮——收尾补发因挂起位已清不会触发。
            if (pA->bSendAfterAutoName) {
                pA->bSendAfterAutoName = false;
                if (!pA->aStaged.isEmpty()) {
                    QStringList aBack;
                    for (const QPair<QString, QString>& stP : pA->aStaged) {
                        aBack << stP.second;
                    }
                    pA->aStaged.clear();
                    pA->bQueueExpanded = false;
                    RefreshQueueBox(sId);
                    if (pA->pInput != nullptr) {
                        pA->pInput->setPlainText(aBack.join(QStringLiteral("\n")));
                    }
                }
            }
            emit sigAdhocStop(sId);   // 该助手运行中无输入：按钮即「停止」
            return;
        }
        DoAdhocSend(sId);
    });
    // 撤销作用于该助手的编排器会话栈。
    QPushButton* const pUndo = a_stAsst.pBtnUndo;
    connect(pUndo, &QPushButton::clicked, this, [this, sId]() {
        emit sigAdhocUndo(sId);
    });

    // —— 统计栏：[执行态指示器][该助手自己的费用/token/请求/耗时][本轮实时 ↑↓] ——
    // 2026.09.05 v2.0.120 per-助手执行态指示器（与任务侧底部栏同款 CBusyIndicator）：
    //   该助手运行 → 转圈；空闲 → 灰点（助手无任务态色，不复用 m_colIdle）；
    //   SetAsstRunning 统一起停，与任务侧底部指示器互不干涉。
    auto* pCostBar = new QHBoxLayout();
    pCostBar->setContentsMargins(0, 0, 0, 0);
    a_stAsst.pBusy = new CBusyIndicator(a_stAsst.pPage);
    a_stAsst.pBusy->SetBusy(a_stAsst.bRunning);
    pCostBar->addWidget(a_stAsst.pBusy);
    a_stAsst.pLblCost = new QLabel(QStringLiteral("费用: $0.00 | 输入:0 输出:0 | 请求:0"),
                                   a_stAsst.pPage);
    { QFont fLc = a_stAsst.pLblCost->font(); fLc.setPointSize(11); a_stAsst.pLblCost->setFont(fLc); }
    pCostBar->addWidget(a_stAsst.pLblCost, 1);
    a_stAsst.pLblLive = new QLabel(a_stAsst.pPage);
    a_stAsst.pLblLive->hide();
    { QFont fLv = a_stAsst.pLblLive->font(); fLv.setPointSize(11); a_stAsst.pLblLive->setFont(fLv); }
    pCostBar->addWidget(a_stAsst.pLblLive);
    pV->addLayout(pCostBar);

    // 初始态：进度条/统计栏按该助手持久化快照还原（usage/ctxFill/model 随档走）。
    UpdateContextButton(a_stAsst);
    UpdateAsstCostLabel(a_stAsst);
    UpdateAdhocInputState(sId);
}

void CExecutionPanel::RemoveAsstTab(SAsst& a_stAsst) {
    // 摘 Tab + 删助手页（日志视图/提示词按钮/输入栏/统计栏均为页内子控件随删）。
    // 摘除前先预切走当前页：QTabWidget 摘掉当前页时会自动激活相邻页——相邻常是「+」占位页，
    // 会被 OnAsstTabCurrentChanged 当成点「+」而新增助手（重置场景即死循环根因，也是
    // 手动关闭助手时的幽灵新增）。预切到另一个打开的助手，无则日志页。
    if (m_pTabs->currentWidget() == a_stAsst.pPage) {
        QWidget* pNext = nullptr;
        for (SAsst& st : m_aAssts) {
            if (&st != &a_stAsst && !st.bClosed && st.pPage != nullptr) {
                pNext = st.pPage;
                break;
            }
        }
        m_pTabs->setCurrentWidget(pNext != nullptr ? pNext
                                                   : static_cast<QWidget*>(m_pView));
    }
    const int iIdx = m_pTabs->indexOf(a_stAsst.pPage);
    if (iIdx >= 0) {
        // 2026-09-22 先撤运行闪点再摘 Tab：栏以页指针为键，页即将销毁，防集合悬垂。
        if (m_pAsstBar != nullptr) {
            m_pAsstBar->SetPageRunning(a_stAsst.pPage, false);
        }
        m_pTabs->removeTab(iIdx);   // 预切后不再是当前页，Qt 不再自动激活「+」页
    }
    // 2026.09.03 流解绑（断线+弃件保数据+作废在途帧）先于视图销毁，防销毁后回调悬垂。
    if (a_stAsst.pStream) {
        a_stAsst.pStream->Unbind();
    }
    delete a_stAsst.pPage;   // 页容器销毁 = 日志视图/输入栏/统计栏/浮层按钮整组释放
    a_stAsst.pPage       = nullptr;
    a_stAsst.pView       = nullptr;
    a_stAsst.pBtnPrompts = nullptr;
    a_stAsst.pBtnBottom  = nullptr;
    a_stAsst.pQueueBox   = nullptr;   // 2026.09.18 暂存折叠框随页销毁（重开重建；暂存队列非持久化，随行作废）
    a_stAsst.pQueueBtn   = nullptr;
    a_stAsst.pQueueHead  = nullptr;
    a_stAsst.pQueueList  = nullptr;
    a_stAsst.pInput        = nullptr;
    a_stAsst.pComboPerm    = nullptr;   // 权限选择已存 iPerm，重开 CreateAsstWidgets 回显
    a_stAsst.pBtnContext   = nullptr;
    a_stAsst.pBtnUndo      = nullptr;
    a_stAsst.pBtnSend      = nullptr;
    a_stAsst.pLblCost      = nullptr;
    a_stAsst.pLblLive      = nullptr;
    a_stAsst.pBusy         = nullptr;   // 2026.09.05 页内执行态指示器随页释放（重开重建）
    a_stAsst.tClock.invalidate();
    m_aPendingAsstLive.remove(a_stAsst.sId);   // 本轮实时暂存随页丢弃
    UpdateTabCloseButtons();
    // 2026.09.19 摘掉最后一个运行助手时全停 → 闪点时钟停（现由栏内 SetPageRunning 自管）
}

void CExecutionPanel::CloseAsst(const QString& a_sId) {
    // 点「x」：关闭该助手——Tab 摘除、视图控件销毁，但对话/会话数据保留（bClosed=true），
    //   可经「▾」菜单随时恢复（2026.08.23 由「删除」改为「关闭」语义）。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr || pAsst->bClosed) {
        return;
    }
    if (pAsst->bRunning) {
        QMessageBox::information(this, QStringLiteral("关闭助手"),
            QStringLiteral("该助手正在执行指令，请先停止再关闭。"));
        return;
    }
    // （2026.08.27 跨助手排队挡已删：排队机制随多助手并发移除；同助手排队注入
    //   的指令由编排器会话持有，关闭/删除前已要求空闲，无静默丢失。）
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
    // 控件已随视图销毁：流解绑弃件保数据（RemoveAsstTab 已调，此处幂等兜底），
    //   数据在流对象内保留，重开时 Rebuild 按需重建。
    if (pAsst->pStream) {
        pAsst->pStream->Unbind();
    }
    pAsst->bClosed = true;
    if (bEmpty) {
        m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });
    } else {
    // 移到列表末尾（m_aAssts 顺序 = Tab 顺序，已关闭的排在后面）。
    const SAsst stKeep = *pAsst;
    m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });
    m_aAssts.append(stKeep);
    }
    // QTabWidget 摘页时已自动切到相邻页 → OnAsstTabCurrentChanged 已更新 m_sActiveAsst；
    // 兜底：若当前页不是助手页，激活第一个打开的助手。
    if (AsstById(m_sActiveAsst) == nullptr || AsstById(m_sActiveAsst)->bClosed) {
        const SAsst* pFirst = nullptr;
        for (const SAsst& st : m_aAssts) {
            if (!st.bClosed) { pFirst = &st; break; }
        }
        if (pFirst != nullptr) {
            m_pTabs->setCurrentWidget(pFirst->pPage);
        }
    }
    UpdateAdhocInputState();
    emit sigAsstLayoutChanged();   // 关闭状态入档 → 落盘
}

void CExecutionPanel::OnAsstTabContextMenu(const QPoint& a_stPos) {
    // 右键助手 Tab：「重命名」+「自动重命名」+「彻底删除」（与原生「x」软关闭区分：
    //   数据不保留、不可恢复，进入 DeleteAsst 二次确认）；非助手页（日志/产物/「+」/预览）
    //   不弹菜单（保持原生行为）。
    if (m_pTabs == nullptr) {
        return;
    }
    QTabBar* pBar = m_pTabs->tabBar();
    const int iIdx = m_pAsstBar != nullptr ? m_pAsstBar->TabAtPos(a_stPos)   // 2026-09-22 多行布局命中（基类 tabAt 是单行口径会错行）
                                           : pBar->tabAt(a_stPos);
    if (iIdx < 0) {
        return;
    }
    if (SAsst* pCtx = AsstByPage(m_pTabs->widget(iIdx));
            pCtx != nullptr && AsstIsForeign(*pCtx)) {
        return;   // 2026-10-02 外档幽灵：不弹本档操作菜单（改名/删除回归属任务下做）
    }
    SAsst* pAsst = AsstByPage(m_pTabs->widget(iIdx));
    if (pAsst == nullptr) {
        return;   // 非助手 Tab：无菜单
    }
    QMenu menu(this);
    menu.setToolTipsVisible(true);   // 菜单项说明（Qt 默认不显示 action tooltip）
    QAction* pActRename = menu.addAction(
        MenuIcon(QStringLiteral("ow-rename")),
        QStringLiteral("重命名「%1」…").arg(pAsst->sName));
    pActRename->setToolTip(QStringLiteral("修改助手名（也可双击 Tab 直接改名）"));
    // 2026-09-22 自动重命名：该助手全部用户提示词交 Agent 总结（名称+说明，说明进 tooltip）。
    QAction* pActAuto = menu.addAction(
        MenuIcon(QStringLiteral("ow-autoname")),
        QStringLiteral("自动重命名「%1」…").arg(pAsst->sName));
    pActAuto->setToolTip(QStringLiteral(
        "根据该助手收到的全部提示词，请大模型总结生成新名称（≤10 字）与一句说明（悬停 Tab 可见）"));
    menu.addSeparator();
    QAction* pActDel = menu.addAction(
        MenuIcon(QStringLiteral("ow-delete")),
        QStringLiteral("彻底删除「%1」…").arg(pAsst->sName));
    pActDel->setToolTip(QStringLiteral("从当前档永久移除该助手（对话/会话链/统计，不可恢复）"));
    QAction* pAct = menu.exec(pBar->mapToGlobal(a_stPos));
    if (pAct == pActRename) {
        OnAsstTabRenamed(iIdx);   // 复用双击改名（输入框 + Tab 标题刷新 + 落盘）
    } else if (pAct == pActAuto) {
        // 2026-10-03 统一入口 TriggerAsstAutoRename（素材收集 + 运行中挂起/空闲立即
        //   发射，见函数头注）；仅无素材（没收到过提示词）时提示。
        if (!TriggerAsstAutoRename(pAsst->sId)) {
            QMessageBox::information(this, QStringLiteral("自动重命名"),
                QStringLiteral("该助手还没有收到过任何提示词，没有素材可总结。"));
        }
    } else if (pAct == pActDel) {
        DeleteAsst(pAsst->sId);
    }
}

void CExecutionPanel::DeleteAsst(const QString& a_sId) {
    // 彻底删除（右键菜单）UI 层：守卫 + 二次确认弹窗，确认后交 DeleteAsstCore 执行
    //   （核心无 UI，供回归测试直调）。与 CloseAsst（点「x」软关闭，数据保留可「▾」
    //   恢复）不同——会话链/对话/统计永久移除，落盘后不可恢复。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr) {
        return;
    }
    if (pAsst->bRunning) {
        QMessageBox::information(this, QStringLiteral("删除助手"),
            QStringLiteral("该助手正在执行指令，请先停止再删除。"));
        return;
    }
    if (std::count_if(m_aAssts.begin(), m_aAssts.end(),
                      [](const SAsst& st) { return !st.bClosed; }) <= 1) {
        QMessageBox::information(this, QStringLiteral("删除助手"),
            QStringLiteral("至少需要保留一个打开的助手，请先新增一个再删除。"));
        return;
    }
    const int iMsgs = pAsst->pStream ? pAsst->pStream->Count() : 0;
    const QMessageBox::StandardButton eBtn = QMessageBox::question(this,
        QStringLiteral("彻底删除助手"),
        QStringLiteral("确认彻底删除助手「%1」？\n\n该助手共 %2 条对话记录，其对话内容、"
                       "会话链与消耗统计将从当前档中永久移除，删除后不可恢复。\n"
                       "（如只需暂时收起，请点 Tab 上的「x」关闭，之后可经「▾」恢复）")
            .arg(pAsst->sName).arg(iMsgs),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (eBtn != QMessageBox::Yes) {
        return;
    }
    DeleteAsstCore(a_sId);
}

void CExecutionPanel::DeleteAsstCore(const QString& a_sId) {
    // 彻底删除核心（无弹窗，DeleteAsst 确认后调用；测试直调本层）：
    //   Tab/控件销毁 + 对话流/统计随 SAsst 移除 + 编排器会话链释放（sigAsstRemoved →
    //   主窗口 RemoveAsstAdhoc）+ 落盘后从 assistant.json 永久出局。
    SAsst* pAsst = AsstById(a_sId);
    if (pAsst == nullptr) {
        return;
    }
    RemoveAsstTab(*pAsst);   // 摘 Tab + 释放页控件 + 流解绑（预切相邻助手页，防「+」页误增）
    m_aLastStampSecs.remove(AsstStampKey(a_sId));   // 时间戳哨兵随删
    m_aAssts.removeIf([&a_sId](const SAsst& st) { return st.sId == a_sId; });   // pStream 末引用随成释放
    // 激活校正（RemoveAsstTab 预切已走 sigAsstSwitched；此处兜底非当前页删除/异常序）：
    //   激活 id 已不存在或指向已关闭助手 → 切到首个打开助手。
    if (AsstById(m_sActiveAsst) == nullptr || AsstById(m_sActiveAsst)->bClosed) {
        const SAsst* pFirst = nullptr;
        for (const SAsst& st : m_aAssts) {
            if (!st.bClosed) { pFirst = &st; break; }
        }
        if (pFirst != nullptr && pFirst->pPage != nullptr) {
            m_pTabs->setCurrentWidget(pFirst->pPage);
        }
    }
    UpdateAdhocInputState();
    emit sigAsstRemoved(a_sId, m_sActiveAsst);   // 主窗口：RemoveAsstAdhoc 清编排器会话
    emit sigAsstLayoutChanged();                 // 剩余助手落盘（被删者自此出局）
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
        const int iCount = st.pStream ? st.pStream->Count() : 0;
        const QString sLabel = QStringLiteral("%1（%2 条对话）").arg(st.sName).arg(iCount);
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
    m_pTabs->setCurrentWidget(pNew->pPage);   // → sigAsstSwitched（主窗口换载该助手会话）
    emit sigAsstLayoutChanged();              // 恢复入档 → 落盘
}

void CExecutionPanel::OnAsstAddClicked() {
    // 点「+」：新增助手（名字自动编号「助手N」：取已有编号（打开的 + 可见的已关闭、
    //   未被改名）中缺失的最小编号，如已有 1、2、10 → 新增为「助手3」，避开重名），自动激活。
    QSet<int> aUsed;
    for (const SAsst& st : m_aAssts) {
        if (st.bClosed && !AsstHasUserPrompt(st)) {
            continue;   // 2026.09.10 隐形空助手（已关闭无提示词，「▾」菜单不显示）不占号
        }
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
    st.sOwnerKey = m_sViewKey;   // 2026-10-02 新建归属当前视图档
    m_aAssts.append(st);   // 先入列表再建 Tab（UpdateTabCloseButtons 需按视图反查助手）
    CreateAsstTab(m_aAssts.last());
    UpdateTabCloseButtons();
    m_pTabs->setCurrentWidget(AsstById(st.sId)->pPage);   // → sigAsstSwitched（主窗口换会话）
    emit sigAsstAdded(st.sId);
}

void CExecutionPanel::OnAsstTabRenamed(int a_iIdx) {
    // 双击助手 Tab → 改名（Tab 标题即助手名）。
    SAsst* pAsst = AsstByPage(m_pTabs->widget(a_iIdx));
    if (pAsst == nullptr) {
        return;
    }
    if (AsstIsForeign(*pAsst)) {
        return;   // 2026-10-02 外档幽灵：改名回归属任务下做（防跨档写脏名）
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
    if (m_pAsstBar != nullptr) {
        m_pAsstBar->NotifyTabsChanged();   // 改名变宽/窄 → 多行重排
    }
    emit sigAsstLayoutChanged();   // 名字入档 → 落盘
}

void CExecutionPanel::ApplyAsstAutoName(const QString& a_sAsstId, bool a_bOk,
                                        const QString& a_sName, const QString& a_sDesc) {
    // 2026-09-22 助手自动重命名回填：成功改 sName/sDesc（Tab 标题 + tooltip 首行说明）
    //   + 事件行留痕 + 落盘；失败仅记事件行（原名/原说明保持不变）。目标助手可能已被
    //   关闭（右键后到回填期间）：数据照改（重开即见），Tab 刷新由 RefreshAsstTabTexts
    //   跳过无 Tab 项自然兼容。
    SAsst* pAsst = AsstById(a_sAsstId);
    if (pAsst == nullptr) {
        return;
    }
    if (!a_bOk) {
        OnAdhocEvent(a_sAsstId, QStringLiteral("auto_name_failed"),
            QStringLiteral("✗ 助手自动重命名失败：%1（名称保持不变）")
                .arg(a_sDesc.left(120)));
        return;
    }
    const QString sOld = pAsst->sName;
    pAsst->sName = a_sName;
    pAsst->sDesc = a_sDesc;
    RefreshAsstTabTexts();   // 标题 + tooltip（说明行 + 后端行）统一刷
    OnAdhocEvent(a_sAsstId, QStringLiteral("auto_named"),
        QStringLiteral("⚙ 已根据历史提示词自动重命名：「%1」→「%2」%3")
            .arg(sOld, a_sName,
                 a_sDesc.isEmpty() ? QString() : QStringLiteral("（") + a_sDesc + QStringLiteral("）")));
    emit sigAsstLayoutChanged();   // 名称/说明入档 → 落盘
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
        SAsst* p = AsstByPage(m_pTabs->widget(i));
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
    // 2026.09.29 修「阶段产物页陈旧/空白」：切到产物 Tab 即按当前选中任务重扫——
    //   旧口径只在「选中任务的 stage_done」时刷新，任务中途被取消选中（点列表空白/
    //   切助手页）后列表被清空且不再更新，回来看到的永远是空页（实测 MeloTTS 任务
    //   全部产物在盘却显示零产物）。Tab 激活重扫兑住所有漏刷场景，成本=一次目录扫描。
    if (m_pArtifactsPage != nullptr && m_pTabs->widget(a_iIdx) == m_pArtifactsPage) {
        RefreshStageOutputs(m_pCurTask);
        return;
    }
    // 2026-10-02 外档幽灵页被点击：通知宿主切回归属任务/工程档（面板随后整体重建，
    //   幽灵转正；本页日志在切换前照常展示）。
    if (SAsst* pForeign = AsstByPage(m_pTabs->widget(a_iIdx));
            pForeign != nullptr && AsstIsForeign(*pForeign)) {
        emit sigAsstForeignActivated(pForeign->sOwnerKey);
    }
    // 「+」页被选中：新增助手并弹回（OnAsstAddClicked 末尾会把当前页切到新助手 Tab）。
    if (m_pTabs->widget(a_iIdx) == m_pAddTabPage) {
        const QString sBack = (AsstById(m_sActiveAsst) && !AsstById(m_sActiveAsst)->bClosed)
                                  ? m_sActiveAsst : QString();
        OnAsstAddClicked();
        if (!sBack.isEmpty() && AsstById(m_sActiveAsst) == nullptr) {
            m_pTabs->setCurrentWidget(AsstById(sBack)->pPage);
        }
        return;
    }
    // 切 Tab：当前页是助手页 → 激活切换（非助手页如日志/产物/预览不改激活助手）。
    SAsst* pAsst = AsstByPage(m_pTabs->widget(a_iIdx));
    if (pAsst == nullptr || pAsst->sId == m_sActiveAsst) {
        return;
    }
    const QString sOld = m_sActiveAsst;
    m_sActiveAsst = pAsst->sId;
    // 首次切到该助手（或其视图件尚未建）→ 按数据重建其视图（恢复档时惰性补建）。
    //   2026.09.03 件状态在流内：未建流/无件 且有数据 → 重建（旧：aWidgets 空且 aBlocks 非空）。
    const bool bNeedsBuild = (pAsst->pView != nullptr)
        && (!pAsst->pStream || (pAsst->pStream->WidgetCount() == 0 && pAsst->pStream->Count() > 0));
    if (bNeedsBuild) {
        RebuildAdhocView();
    } else {
        RebuildPromptList();
    }
    // 2026.09.05 per-助手控件：上下文进度条/统计栏/输入栏均随助手页存在，切换 Tab
    //   即切换实例，无需再回填共享控件快照（旧：m_iLastContextFill/m_sAdhocModel/
    //   m_iContextLimit 三面板级镜像已删）；提示词下拉列表已在上方 if/else 按新激活
    //   助手重建（RebuildAsstView 内部亦会 RebuildPromptList）。
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
            // 2026.09.18 暂存折叠框：仅当前页显示，钉右下角。
            if (st.pQueueBox != nullptr && !st.aStaged.isEmpty()) {
                const bool bCurQ = (st.pView == pAsst->pView);
                st.pQueueBox->setVisible(bCurQ);
                if (bCurQ) {
                    st.pQueueBox->move(
                        st.pView->width() - st.pQueueBox->width() - 11,
                        st.pView->height() - st.pQueueBox->height() - 8);
                    st.pQueueBox->raise();
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


