#include "CRecentChipsPanel.h"
// ============================================================================
// 「最近打开」按钮面板实现（设计定稿见头注；2026.09.26 v3.1.239）。
// 自绘按钮 CRecentChipButton 定义在本文件（AUTOMOC：文末 #include .moc）。
// ============================================================================
#include <QDir>
#include <QEvent>
#include <QApplication>
#include <QKeyEvent>   // 2026.10.12 键盘导航（方向键/回车/空格）
#include <climits>     // INT_MAX（导航最近邻搜索）
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>   // 2026-10-11 启动页搜索行
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <QFontMetrics>
#include <QGuiApplication>

#include "ui/CMenuIcon.h"   // 2026-10-09 右键删除菜单图标（与资源管理器右键菜单同源）

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // 2026-10-09 持久化 profile API

// —— 面板几何常量（11 号字体口径）——
static constexpr int kChipH      = 30;   // 按钮高
static constexpr int kChipHPad   = 10;   // 按钮左右内边距
static constexpr int kSpacing    = 6;    // 按钮间 / 行间距
static constexpr int kHostMargin = 8;    // 滚动内容区边距
static constexpr int kStatusH    = 26;   // 状态栏高
static constexpr int kMaxVisRows = 9;    // 可视行数封顶（每行 4~6 个 → 约 40~54 可视）
static constexpr int kPanelW     = 560;  // 面板自动宽（钳到屏宽-40）
// —— 2026-10-09 边缘拖拽改尺寸 ——
static constexpr int kChipMinW   = 360;  // 用户尺寸下限（能容纳 2~3 个按钮列）
static constexpr int kChipMinH   = 240;  // 下限（状态栏 + 约 6 行）
static constexpr int kEdgeRight  = 1;    // 边掩码（与 EdgeAt/拖拽分支对应）
static constexpr int kEdgeBottom = 2;

// ============================================================================
// CRecentChipButton —— 自绘按钮：平时底色=打开次数渐变色，悬停=蓝底白字 +
//   假粗体（2x2 四次绘制，字号/字宽不变 → 布局零抖动，用户方案 4）。
// ============================================================================
class CRecentChipButton : public QWidget {
    Q_OBJECT
public:
    CRecentChipButton(const QString& a_sName, const QString& a_sPath,
                      int a_iOpens, bool a_bMore, QWidget* a_pParent)
        : QWidget(a_pParent), m_sName(a_sName), m_sPath(a_sPath),
          m_iOpens(a_iOpens), m_bMore(a_bMore) {
        setFixedHeight(kChipH);
        setCursor(Qt::PointingHandCursor);
        // 2026.10.12 键盘导航：芯片可聚焦（启动页搜索框 ↓ 进入芯片区，方向键
        //   选择、回车打开——焦点框见 paintEvent；事件处理在面板 qApp 过滤器）。
        setFocusPolicy(Qt::StrongFocus);
        if (a_bMore) {
            setToolTip(QStringLiteral("打开完整列表（搜索 / 多选删除 / 打开计数）"));
        } else {
            setToolTip(QStringLiteral("路径：%1\n已打开 %2 次（每次成功打开工程时累加 1）")
                           .arg(a_sPath)
                           .arg(a_iOpens));
        }
    }

signals:
    void Clicked();
    void HoverIn(const QString& sPath);
    void HoverOut();
    // 2026-10-09 右键删除：携带工程路径与全局坐标（面板弹上下文菜单用）。
    void RightClicked(const QString& sPath, const QPoint& ptGlobal);

    // —— 2026.10.12 键盘导航观察口（面板导航/测试用）——
public:
    bool    IsMore() const { return m_bMore; }
    QString ChipName() const { return m_sName; }
    QString ChipPath() const { return m_sPath; }

protected:
    void focusInEvent(QFocusEvent* a_pEv) override {
        QWidget::focusInEvent(a_pEv);
        m_bFocused = true;
        update();
    }
    void focusOutEvent(QFocusEvent* a_pEv) override {
        QWidget::focusOutEvent(a_pEv);
        m_bFocused = false;
        update();
    }
    void enterEvent(QEnterEvent* a_pEv) override {
        QWidget::enterEvent(a_pEv);
        m_bHover = true;
        update();
        emit HoverIn(m_sPath);
    }
    void leaveEvent(QEvent* a_pEv) override {
        QWidget::leaveEvent(a_pEv);
        m_bHover = false;
        update();
        emit HoverOut();
    }
    void mousePressEvent(QMouseEvent* a_pEv) override {
        QWidget::mousePressEvent(a_pEv);
        // 2026-10-09 右键删除：右键按压发信号（面板弹上下文菜单）；「更多...」
        //   首项无路径（sPath 空）不参与。坐标取全局——弹菜单位置与嵌套弹窗无关。
        if (a_pEv->button() == Qt::RightButton && !m_sPath.isEmpty()) {
            emit RightClicked(m_sPath, a_pEv->globalPosition().toPoint());
        }
    }
    void mouseReleaseEvent(QMouseEvent* a_pEv) override {
        QWidget::mouseReleaseEvent(a_pEv);
        // 2026-10-09 仅左键触发打开（旧版不区分按键：右键释放也会误开工程）。
        if (a_pEv->button() == Qt::LeftButton && rect().contains(a_pEv->pos())) {
            emit Clicked();   // 菜单语义：释放触发（按压拖出可取消）
        }
    }
    void paintEvent(QPaintEvent* a_pEv) override {
        QWidget::paintEvent(a_pEv);
        QPainter stP(this);
        stP.setRenderHint(QPainter::TextAntialiasing, true);
        stP.setPen(Qt::NoPen);
        stP.setBrush(m_bHover ? QColor(0x25, 0x63, 0xeb)                // 悬停蓝（全局主色）
                              : CRecentChipsPanel::ChipColorForCount(m_iOpens));
        stP.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 4, 4);
        // 2026.10.12 键盘焦点框：蓝色实线（与悬停同主色系；悬停蓝底会盖住底色，
        //   焦点框恒画——悬停+焦点叠加时白底上的蓝框仍可辨）。
        if (m_bFocused) {
            stP.setBrush(Qt::NoBrush);
            stP.setPen(QPen(QColor(0x25, 0x63, 0xeb), 2));
            stP.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 4, 4);
        }
        stP.setPen(m_bHover ? Qt::white : QColor(0x1f, 0x29, 0x37));
        // -2：假粗体 2x2 偏移（右/下各 +1px）的绘制余量
        const QString sTxt = stP.fontMetrics().elidedText(
            m_sName, Qt::ElideRight, width() - 2 * kChipHPad - 2);
        const QRect rcTxt = rect().adjusted(kChipHPad, 0, -kChipHPad - 2, 0);
        if (m_bHover) {
            // 假粗体（用户方案 4）：2x2 范围绘制四次——视觉加粗而字号/字宽不变，
            //   悬停不引起任何布局抖动（真 setFont(Bold) 会变宽挤压同行）。
            for (int iDx = 0; iDx <= 1; ++iDx) {
                for (int iDy = 0; iDy <= 1; ++iDy) {
                    stP.drawText(rcTxt.translated(iDx, iDy),
                                 Qt::AlignVCenter | Qt::AlignLeft, sTxt);
                }
            }
        } else {
            stP.drawText(rcTxt, Qt::AlignVCenter | Qt::AlignLeft, sTxt);
        }
    }

private:
    QString m_sName;
    QString m_sPath;
    int     m_iOpens = 1;
    bool    m_bMore  = false;
    bool    m_bHover = false;
    bool    m_bFocused = false;   // 2026.10.12 键盘焦点框
};

// ============================================================================
// CRecentChipsPanel
// ============================================================================
CRecentChipsPanel::CRecentChipsPanel(FOpenProject a_fnOpen, FMore a_fnMore,
                                     QWidget* a_pParent, const FCfgPath& a_fnCfgPath,
                                     const FRemoveProject& a_fnRemove)
    : QWidget(a_pParent), m_fnOpen(std::move(a_fnOpen)), m_fnMore(std::move(a_fnMore)),
      m_fnCfgPath(a_fnCfgPath), m_fnRemove(a_fnRemove) {
    QFont stF = font();
    stF.setPointSize(11);   // 全局 UI 11 号字体
    setFont(stF);

    m_pScroll = new QScrollArea(this);
    m_pScroll->setFrameShape(QFrame::NoFrame);
    m_pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScroll->setWidgetResizable(true);
    m_pHost = new QWidget();
    m_pHost->setContentsMargins(kHostMargin, kHostMargin, kHostMargin, kHostMargin);
    m_pHost->installEventFilter(this);   // 视口宽变化（滚动条出现/消失）→ 重排
    m_pScroll->setWidget(m_pHost);

    m_pLblStat = new QLabel(this);
    m_pLblStat->setFixedHeight(kStatusH);
    m_pLblStat->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_pLblStat->setTextInteractionFlags(Qt::NoTextInteraction);
    m_pLblStat->setStyleSheet(QStringLiteral(
        "background:#f3f4f6;border-top:1px solid #e0e3e8;"
        "padding:1px 8px;color:#4b5563;"));

    auto* pLay = new QVBoxLayout(this);
    pLay->setContentsMargins(0, 0, 0, 0);
    pLay->setSpacing(0);
    // 2026-10-09 拖拽改尺寸 qApp 级接管（子控件级过滤器看不到发往 QMenu 的
    //   chrome 区事件，详见头文件注释；QObject 析构自动从过滤器链摘除）。
    qApp->installEventFilter(this);
    // 2026-10-11 启动页搜索行：顶部固定一行搜索框（默认隐藏，SetFilterBar 启用）。
    //   单独占一行、占满整宽；输入实时过滤工程芯片（见 OnFilterChanged）。
    m_pEditFilter = new QLineEdit(this);
    {
        QFont stF = m_pEditFilter->font();
        stF.setPointSize(11);   // 全局 UI 11 号字体
        m_pEditFilter->setFont(stF);
        m_pEditFilter->setPlaceholderText(QStringLiteral("搜索工程（名称 / 路径）…"));
        m_pEditFilter->setClearButtonEnabled(true);
        m_pEditFilter->setFixedHeight(kChipH);
        m_pEditFilter->setStyleSheet(QStringLiteral(
            "QLineEdit{border:1px solid #c9ced6;border-radius:4px;padding:0 8px;"
            "background:#ffffff;selection-background-color:#2563eb;"
            "selection-color:#ffffff;}"));
        m_pEditFilter->hide();
        connect(m_pEditFilter, &QLineEdit::textChanged,
                this, &CRecentChipsPanel::OnFilterChanged);
    }
    pLay->addWidget(m_pEditFilter);

    pLay->addWidget(m_pScroll, 1);
    pLay->addWidget(m_pLblStat);
}

void CRecentChipsPanel::SetFilterBar(bool a_bOn) {
    // 2026-10-11 启动页搜索行开关：启用后搜索框可见、Refresh 不再建「更多...」
    //   芯片（全量列表入口由宿主场景自行提供，启动页以过滤替代）。数据不变，
    //   已有芯片布局不变——形态差异在下次 Refresh 生效（启动页流程=先开关再首刷）。
    m_bFilterBar = a_bOn;
    if (m_pEditFilter != nullptr) {
        m_pEditFilter->setVisible(a_bOn);
        if (a_bOn) {
            // 2026-10-12 缺省焦点（用户需求：启动即焦点在搜索框，可直接打字）：
            //   窗口未显示时 setFocus 记为初始焦点（显示后生效）；已显示则立即聚焦。
            m_pEditFilter->setFocus(Qt::OtherFocusReason);
        }
    }
}

void CRecentChipsPanel::Refresh(const QStringList& a_aPaths, const QList<int>& a_aOpens) {
    for (CRecentChipButton* pChip : m_aChips) {
        // 2026.09.28 deleteLater 替代 delete：Refresh 可能被芯片 Clicked 的同步调用链
        //   （点芯片→开工程→MRU 回写→刷新）触发，直接 delete 会销毁正在发射信号的
        //   sender（mouseReleaseEvent 仍在栈上），返回后访问已释放内存（UAF/崩溃）。
        pChip->deleteLater();
    }
    m_aChips.clear();
    m_aNames.clear();
    m_aPaths.clear();   // 2026-10-11 过滤匹配数据源（与芯片对齐）

    if (!m_bFilterBar) {   // 2026-10-11 搜索行模式不建「更多...」（启动页以过滤替代）
    auto* pMore = new CRecentChipButton(QStringLiteral("更多..."), QString(), 0, true, m_pHost);
    connect(pMore, &CRecentChipButton::Clicked, this, [this]() {
        if (m_fnMore) { m_fnMore(); }
    });
    connect(pMore, &CRecentChipButton::HoverIn, this, [this](const QString&) {
        m_pLblStat->setText(QStringLiteral("打开完整列表（搜索 / 多选删除 / 打开计数）"));
        m_pLblStat->setToolTip(QString());
    });
    connect(pMore, &CRecentChipButton::HoverOut, this, [this]() {
        m_pLblStat->setText(m_sStatDefault);
        m_pLblStat->setToolTip(QString());
    });
    m_aChips.append(pMore);
    m_aNames.append(QStringLiteral("更多..."));
    m_aPaths.append(QString());   // 「更多...」占位无路径
    // 2026-10-09 运行中刷新可见性：右键删除后 Refresh 发生在面板已显示期间，
    //   Qt 中已可见父部件里 new 出的子部件默认隐藏——必须显式 show（否则删除
    //   一个工程后所有按钮全部消失，用户实害）。
    pMore->show();
    }

    for (int i = 0; i < a_aPaths.size(); ++i) {
        const QString& sPath = a_aPaths.at(i);
        const int iOpens = (i < a_aOpens.size() && a_aOpens.at(i) > 0)
                               ? a_aOpens.at(i) : 1;
        const QString sName = QDir(sPath).dirName();
        auto* pChip = new CRecentChipButton(sName, sPath, iOpens, false, m_pHost);
        connect(pChip, &CRecentChipButton::Clicked, this, [this, sPath]() {
            if (m_fnOpen) { m_fnOpen(sPath); }
        });
        connect(pChip, &CRecentChipButton::RightClicked, this,
                [this](const QString& sP, const QPoint& ptG) {
            OnChipRightClick(sP, ptG);   // 2026-10-09 右键删除入口
        });
        connect(pChip, &CRecentChipButton::HoverIn, this, [this](const QString& sP) {
            m_pLblStat->setText(m_pLblStat->fontMetrics().elidedText(
                sP, Qt::ElideMiddle, m_pLblStat->width() - 16));
            m_pLblStat->setToolTip(sP);   // 状态栏省略时 tooltip 兜底全文
        });
        connect(pChip, &CRecentChipButton::HoverOut, this, [this]() {
            m_pLblStat->setText(m_sStatDefault);
            m_pLblStat->setToolTip(QString());
        });
        m_aChips.append(pChip);
        m_aNames.append(sName);
        m_aPaths.append(sPath);   // 2026-10-11 过滤匹配数据源
        pChip->show();   // 同 pMore：运行中刷新新建子部件须显式显示
    }

    m_sStatDefault = QStringLiteral("共 %1 个工程 · 悬停按钮显示路径 · 滚轮查看更多 · 边缘可拖拽改大小")
                         .arg(a_aPaths.size());
    m_pLblStat->setText(m_sStatDefault);

    // 2026-10-09 首次弹出读档：用户拖拽过的尺寸优先（宽/高钳屏可用区），
    //   未拖拽/无档保持自动尺寸（宽 kPanelW / 高按行数封顶）。
    LoadUserSize();
    const int iAvailScrW = (screen() != nullptr)
        ? screen()->availableGeometry().width() - 40 : kPanelW;
    int iW = (m_iUserW > 0) ? qBound(kChipMinW, m_iUserW, iAvailScrW)
                            : qMin(kPanelW, iAvailScrW);
    int iH;
    if (m_iUserH > 0) {
        iH = qBound(kChipMinH, m_iUserH,
                    (screen() != nullptr ? screen()->availableGeometry().height() - 40
                                         : m_iUserH));
    } else {
        const int iRows = LayoutChips(m_aNames, QFontMetrics(font()),
                                      iW - 2 * kHostMargin, kSpacing, kChipHPad).size();
        const int iNatH = 2 * kHostMargin + iRows * kChipH
                          + (iRows > 0 ? (iRows - 1) * kSpacing : 0) + kStatusH;
        const int iCapH = 2 * kHostMargin + kMaxVisRows * kChipH
                          + (kMaxVisRows - 1) * kSpacing + kStatusH;
        iH = qMin(iNatH, iCapH);
    }
    // 2026-10-11 搜索行模式：自动高度补搜索框行（搜索框 + 间距）。
    if (m_bFilterBar && m_iUserH <= 0) {
        iH += kChipH + kSpacing;
    }
    setFixedSize(iW, iH);
    Relayout();
    OnFilterChanged();   // 重建芯片后按当前搜索词恢复过滤态（空词=全显）
}

QString CRecentChipsPanel::StatusText() const {
    return m_pLblStat != nullptr ? m_pLblStat->text() : QString();
}

QWidget* CRecentChipsPanel::ChipAt(int a_iIdx) const {
    return m_aChips.value(a_iIdx, nullptr);
}

// 2026-10-09 右键芯片 → 上下文菜单（「从最近打开删除」）。嵌套 exec：面板经
//   QWidgetAction 内嵌在「最近打开」子菜单中，exec 的新菜单成为 activePopupWidget，
//   返回后父菜单链保持（面板仍可见）→ 删除后由回调刷新。
void CRecentChipsPanel::OnChipRightClick(const QString& a_sPath, const QPoint& a_ptGlobal) {
    if (a_sPath.isEmpty() || !m_fnRemove) {
        return;   // 「更多...」无路径 / 未注入回调：不弹
    }
    m_pCtxMenu = std::make_unique<QMenu>(this);
    QFont stF = m_pCtxMenu->font();
    stF.setPointSize(11);   // 全局 UI 11 号字体
    m_pCtxMenu->setFont(stF);
    QAction* pActDel = m_pCtxMenu->addAction(
        MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("从最近打开删除(&D)"));
    pActDel->setToolTip(a_sPath);
    const QString sPath = a_sPath;
    connect(pActDel, &QAction::triggered, this, [this, sPath]() {
        DoRemoveChip(sPath);
    });
    // exec（触发时 pActDel==返回值走连接与返回双路其一；动作连接保证回调必达）。
    m_pCtxMenu->exec(a_ptGlobal);
    m_pCtxMenu.reset();   // exec 返回即析（下次右键重建；测试观察口同步置空）
}

// 删除执行链：回调 CMainWindow（PM RemoveRecent 落盘 + 面板 Refresh 重排）。
void CRecentChipsPanel::DoRemoveChip(const QString& a_sPath) {
    if (m_fnRemove) {
        m_fnRemove(a_sPath);
    }
}

bool CRecentChipsPanel::eventFilter(QObject* a_pObj, QEvent* a_pEv) {
    if (a_pObj == m_pHost && a_pEv->type() == QEvent::Resize) {
        Relayout();   // 视口宽变化（滚动条出现/消失）→ 按实宽重排
    }
    // —— 2026.10.12 键盘导航（启动页键盘选工程）——
    //   ① 搜索框内 ↓：焦点跳到首个可见芯片（↑/←/→ 保留光标编辑语义不拦）；
    //   ② 芯片上：回车/空格 = 打开（与点击同语义；事件在此消费，不会落到
    //     QDialog 缺省按钮「跳过」）；方向键 = 芯片间网格导航。事件过滤器
    //     （qApp 级）先于 receiver 处理，消费即拦住原分发路径。
    if (a_pEv->type() == QEvent::KeyPress) {
        auto* pKEv = static_cast<QKeyEvent*>(a_pEv);
        if (a_pObj == m_pEditFilter) {
            if (pKEv->key() == Qt::Key_Down && NavigateChips(Qt::Key_Down)) {
                a_pEv->accept();
                return true;
            }
        } else {
            auto* pChip = qobject_cast<CRecentChipButton*>(a_pObj);
            if (pChip != nullptr) {
                const int iKey = pKEv->key();
                if (iKey == Qt::Key_Return || iKey == Qt::Key_Enter
                        || iKey == Qt::Key_Space) {
                    OpenChip(pChip);
                    a_pEv->accept();
                    return true;
                }
                if (iKey == Qt::Key_Left || iKey == Qt::Key_Right
                        || iKey == Qt::Key_Up || iKey == Qt::Key_Down) {
                    NavigateChips(iKey);
                    a_pEv->accept();
                    return true;
                }
            }
        }
    }
    // —— 2026-10-09 边缘拖拽改尺寸（qApp 级接管，右/下/右下，含菜单 chrome 带）——
    //   过滤器装在 qApp 上：发往面板子树与所在 QMenu（含 chrome 区、滚动条、
    //   状态栏等一切 receiver）的鼠标事件全部可见；判定用全局坐标，不依赖事件
    //   发给谁。拖拽期间吃掉范围内鼠标事件，菜单 hover/点击不受干扰；菜单收起
    //   （Hide）兑底中止并存盘，防止菜单被外部关闭后停留在拖拽态。
    const QEvent::Type t = a_pEv->type();
    if (t == QEvent::Hide) {
        if (m_bResizing && DragScopeContains(a_pObj)) {
            m_bResizing = false;
            UpdateHoverCursor(0);
            SaveUserSize();   // 菜单被关（Esc/程序化）：当前尺寸落盘
        }
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    // NC（非客户区）三类与普通三类统一处理：菜单最外圈 3~4px 是非客户区（实测
    //   probe：chrome 区 WM_LBUTTONDOWN 变为 NonClientAreaMouse*，receiver=QMenu），
    //   用户贴视觉菜单边缘按下的正是这里——必须一并接管才能贴边拖。
    const bool bMousePress   = t == QEvent::MouseButtonPress
                            || t == QEvent::NonClientAreaMouseButtonPress;
    const bool bMouseMove    = t == QEvent::MouseMove
                            || t == QEvent::NonClientAreaMouseMove;
    const bool bMouseRelease = t == QEvent::MouseButtonRelease
                            || t == QEvent::NonClientAreaMouseButtonRelease;
    if (t == QEvent::Leave) {
        // Leave 非 QMouseEvent（不可 cast 取坐标）：仅光标复位。
        if (!m_bResizing && DragScopeContains(a_pObj)) {
            UpdateHoverCursor(0);
        }
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    if (!bMousePress && !bMouseMove && !bMouseRelease) {
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    // 拖拽中：Release 无论落在哪都收尾（鼠标被菜单窗口 capture，正常都在范围内；
    //   兼容极端情况）；Move 只处理范围内。
    if (t == QEvent::MouseButtonRelease) {
        if (!m_bResizing) {
            return QWidget::eventFilter(a_pObj, a_pEv);
        }
        m_bResizing = false;
        UpdateHoverCursor(0);
        SaveUserSize();
        a_pEv->accept();
        return true;   // 吃掉：不给菜单（防当作菜单内点击）
    }
    if (!DragScopeContains(a_pObj)) {
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    auto* pMEv = static_cast<QMouseEvent*>(a_pEv);
    const int iEdge = EdgeAtGlobal(pMEv->globalPosition().toPoint());
    if (bMousePress) {
        if (!m_bResizing && pMEv->button() == Qt::LeftButton && iEdge != 0) {
            m_bResizing = true;
            m_iRsEdge   = iEdge;
            m_ptRsStart = pMEv->globalPosition().toPoint();
            m_iRsStartW = width();
            m_iRsStartH = height();
            UpdateHoverCursor(iEdge);
            a_pEv->accept();
            return true;   // 吃掉：不落入子控件/菜单
        }
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    // Move
    if (m_bResizing) {
        const QPoint ptD = pMEv->globalPosition().toPoint() - m_ptRsStart;
        const int iW = (m_iRsEdge & kEdgeRight)
                           ? m_iRsStartW + ptD.x() : m_iRsStartW;
        const int iH = (m_iRsEdge & kEdgeBottom)
                           ? m_iRsStartH + ptD.y() : m_iRsStartH;
        ApplyUserSize(iW, iH);
        a_pEv->accept();
        return true;
    }
    UpdateHoverCursor(iEdge);   // hover 边缘带光标提示 / 离开恢复
    return QWidget::eventFilter(a_pObj, a_pEv);
}

// receiver 是否在拖拽监听范围：面板自身/子孙，或所在 QMenu（chrome 区 receiver 是
//   菜单自身；菜单内非面板子孙的其他子控件理论上不存在，一并算入保险）。
bool CRecentChipsPanel::DragScopeContains(QObject* a_pObj) const {
    if (a_pObj == this) {
        return true;
    }
    auto* pW = qobject_cast<QWidget*>(a_pObj);
    if (pW == nullptr) {
        return false;
    }
    if (isAncestorOf(pW)) {
        return true;
    }
    QMenu* pM = qobject_cast<QMenu*>(window());
    return pM != nullptr && (pW == pM || pM->isAncestorOf(pW));
}

// hover 边缘带光标（0=恢复）：override 栈配对（m_bCursorSet 防重复入栈）。
void CRecentChipsPanel::UpdateHoverCursor(int a_iEdge) {
    if (a_iEdge == 0) {
        if (m_bCursorSet) {
            QGuiApplication::restoreOverrideCursor();
            m_bCursorSet = false;
        }
        return;
    }
    const QCursor cur = (a_iEdge == kEdgeRight)  ? Qt::SizeHorCursor
                      : (a_iEdge == kEdgeBottom) ? Qt::SizeVerCursor
                                                 : Qt::SizeFDiagCursor;
    if (!m_bCursorSet) {
        QGuiApplication::setOverrideCursor(cur);
        m_bCursorSet = true;
    } else {
        QGuiApplication::changeOverrideCursor(cur);
    }
}

// 全局坐标 → 边掩码（0=不在边缘；右=1，下=2，右下=3）。
//   面板右/下 6px 带，另扩到所在菜单的右/下外缘（chrome 实测 4px/3px）——
//   用户贴视觉菜单边缘按下时 receiver 是 QMenu，必须把这些区域算入命中带。
//   仅右/下/右下（左/上被子菜单锚定，拖动破坏菜单定位）；超出面板 y/x 范围的
//   菜单边缘（面板外的菜单区）不算——那不是面板的边。
int CRecentChipsPanel::EdgeAtGlobal(const QPoint& a_ptGlobal) const {
    const QRect rcP = QRect(mapToGlobal(QPoint(0, 0)), size());   // 面板全局矩形
    const QMenu* pM = qobject_cast<QMenu*>(window());
    // 命中带外沿：菜单右/下外缘（菜单存在时），但不小于面板自身边缘+2（offscreen
    //   等场景菜单布局可能比面板窄——命中带必须至少覆盖面板自己的边缘）。
    const int iHitR = pM != nullptr ? qMax(pM->x() + pM->width(), rcP.right() + 3)
                  : (rcP.x() + rcP.width() + 2);   // 菜单外 +2px 容差（超出即非本面板边）
    const int iHitB = pM != nullptr ? qMax(pM->y() + pM->height(), rcP.bottom() + 3)
                  : (rcP.y() + rcP.height() + 2);
    // 面板内侧 10px 带宽（2026-10-09 二次修：6px 偏窄且被滚动条/状态栏分担后
    //   余量不足；10px 贴近 Windows 常规感知，配合光标提示易命中）。
    //   交叉区（右下角 chrome 斜角）点在面板 x/y 范围之外——iR 的 y 界扩到菜单底、
    //   iB 的 x 界扩到菜单右（边缘带沿菜单边缘延伸，与面板边对齐而非严格相交）。
    const int iRTop = rcP.top();
    const int iRBot = pM != nullptr ? iHitB : rcP.bottom();
    const int iBLeft = rcP.left();
    const int iBRight = pM != nullptr ? iHitR : rcP.right();
    const int iR = (a_ptGlobal.x() >= rcP.right() - 9 && a_ptGlobal.x() <= iHitR
                    && a_ptGlobal.y() >= iRTop && a_ptGlobal.y() <= iRBot)
                       ? kEdgeRight : 0;
    const int iB = (a_ptGlobal.y() >= rcP.bottom() - 9 && a_ptGlobal.y() <= iHitB
                    && a_ptGlobal.x() >= iBLeft && a_ptGlobal.x() <= iBRight)
                       ? kEdgeBottom : 0;
    if (iR == 0 && iB == 0) {
        return 0;
    }
    if (iR != 0 && iB == 0) {
        return kEdgeRight;
    }
    if (iR == 0 && iB != 0) {
        return kEdgeBottom;
    }
    return kEdgeRight | kEdgeBottom;
}

// 钳位后落尺寸：宽/高各自限幅，同步所在 QMenu 重排（菜单尺寸随内嵌件固定尺寸走）。
//   QMenu::sizeHint 有缓存（内嵌件 setFixedSize 不使 itemsDirty），resize(sizeHint())
//   无效（实测菜单恒田尺寸）；改按当前边框补偿（菜单尺寸-面板尺寸，拖拽期间稳定）
//   直接叠加同增量，菜单框恒包住面板。
void CRecentChipsPanel::ApplyUserSize(int a_iW, int a_iH) {
    QScreen* pSc = screen() != nullptr ? screen() : QGuiApplication::primaryScreen();
    const int iMaxW = (pSc != nullptr) ? pSc->availableGeometry().width() - 40 : 4096;
    const int iMaxH = (pSc != nullptr) ? pSc->availableGeometry().height() - 40 : 4096;
    QMenu* pM = qobject_cast<QMenu*>(window());
    const int iChromeW = (pM != nullptr) ? pM->width() - width() : 0;
    const int iChromeH = (pM != nullptr) ? pM->height() - height() : 0;
    m_iUserW = qBound(kChipMinW, a_iW, iMaxW);
    m_iUserH = qBound(kChipMinH, a_iH, iMaxH);
    setFixedSize(m_iUserW, m_iUserH);
    if (pM != nullptr) {
        pM->resize(m_iUserW + iChromeW, m_iUserH + iChromeH);
    }
}

// 读档（首次 Refresh 时一次）：[Layout] RecentChipsSize="w,h"；坏档/无档=保持自动尺寸。
void CRecentChipsPanel::LoadUserSize() {
    if (m_bUserLoaded) {
        return;
    }
    m_bUserLoaded = true;
    if (!m_fnCfgPath || m_fnCfgPath().isEmpty()) {
        return;
    }
    wchar_t szBuf[64] = {0};
    if (GetPrivateProfileStringW(L"Layout", L"RecentChipsSize", L"", szBuf, 64,
                                 (LPCWSTR)m_fnCfgPath().utf16()) <= 0) {
        return;
    }
    const QStringList aParts = QString::fromWCharArray(szBuf).split(QLatin1Char(','));
    if (aParts.size() == 2) {
        bool bOkW = false, bOkH = false;
        const int iW = aParts.at(0).toInt(&bOkW);
        const int iH = aParts.at(1).toInt(&bOkH);
        if (bOkW && bOkH && iW >= kChipMinW && iH >= kChipMinH) {
            m_iUserW = iW;
            m_iUserH = iH;
        }
    }
}

// 落盘：拖拽释放时写 [Layout] RecentChipsSize="w,h"（含 profile 写缓存冲刷）。
void CRecentChipsPanel::SaveUserSize() {
    if (m_iUserW <= 0 || m_iUserH <= 0) {
        return;
    }
    if (!m_fnCfgPath || m_fnCfgPath().isEmpty()) {
        return;
    }
    const QString sVal = QStringLiteral("%1,%2").arg(m_iUserW).arg(m_iUserH);
    WritePrivateProfileStringW(L"Layout", L"RecentChipsSize",
                               (LPCWSTR)sVal.utf16(), (LPCWSTR)m_fnCfgPath().utf16());
    // 冲刷 profile 写缓存（防下一次打开读到空）。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)m_fnCfgPath().utf16());
}

void CRecentChipsPanel::Relayout() {
    if (m_pHost == nullptr) {
        return;
    }
    // 2026-10-11 只排未隐藏芯片（搜索过滤隐藏的不占位）：收集可见子集 → 子集布局
    //   → 按子集序 setGeometry。无过滤时全量可见，行为与旧版一致（iFirst 语义改为
    //   子集内序号，几何赋值按收集顺序对位）。
    QList<CRecentChipButton*> aVis;
    QStringList aVisNames;
    for (int i = 0; i < m_aChips.size() && i < m_aNames.size(); ++i) {
        CRecentChipButton* pChip = m_aChips.at(i);
        if (!pChip->isHidden()) {
            aVis.append(pChip);
            aVisNames.append(m_aNames.at(i));
        }
    }
    const int iAvail = m_pHost->width() - m_pHost->contentsMargins().left()
                                        - m_pHost->contentsMargins().right();
    const QList<SChipRow> aRows = LayoutChips(aVisNames, QFontMetrics(font()),
                                              iAvail, kSpacing, kChipHPad);
    const int iML = m_pHost->contentsMargins().left();
    int iY = m_pHost->contentsMargins().top();
    for (const SChipRow& stRow : aRows) {
        int iX = iML;
        for (int k = 0; k < stRow.aBtnW.size(); ++k) {
            CRecentChipButton* pChip = aVis.value(stRow.iFirst + k, nullptr);
            if (pChip != nullptr) {
                pChip->setGeometry(iX, iY, stRow.aBtnW.at(k), kChipH);
            }
            iX += stRow.aBtnW.at(k) + kSpacing;
        }
        iY += kChipH + kSpacing;
    }
    m_pHost->setMinimumHeight(iY - kSpacing + m_pHost->contentsMargins().bottom());
}

void CRecentChipsPanel::OnFilterChanged() {
    // 2026-10-11 启动页搜索行：实时过滤工程芯片。口径同完整列表搜索框——名称/路径
    //   不区分大小写包含命中；「更多...」占位（无路径）在非搜索模式下恒显示。
    if (m_pEditFilter == nullptr) {
        return;
    }
    const QString sKey = m_pEditFilter->text().trimmed();
    int iShown = 0;
    if (sKey.isEmpty()) {
        for (CRecentChipButton* pChip : m_aChips) {
            pChip->setVisible(true);
        }
        iShown = m_aChips.size();
        m_pLblStat->setText(m_sStatDefault);
        m_pLblStat->setToolTip(QString());
    } else {
        const QString sKeyLower = sKey.toLower();
        for (int i = 0; i < m_aChips.size(); ++i) {
            const bool bHit = (i < m_aPaths.size() && m_aPaths.at(i).isEmpty())
                || (i < m_aNames.size()
                    && m_aNames.at(i).toLower().contains(sKeyLower))
                || (i < m_aPaths.size()
                    && m_aPaths.at(i).toLower().contains(sKeyLower));
            m_aChips.at(i)->setVisible(bHit);
            if (bHit) { ++iShown; }
        }
        m_pLblStat->setText(QStringLiteral("匹配 %1 / %2 个工程")
                                 .arg(iShown)
                                 .arg(qMax(0, m_aChips.size()
                                           - (m_bFilterBar ? 0 : 1))));
        m_pLblStat->setToolTip(sKey);
    }
    Relayout();
}

// 2026.10.12 键盘回车/空格打开芯片（与鼠标点击同语义）：首项「更多...」走完整
//   列表回调，其余打开工程回调；未注入回调静默忽略（与 Clicked 连接口径一致）。
void CRecentChipsPanel::OpenChip(CRecentChipButton* a_pChip) {
    if (a_pChip == nullptr) {
        return;
    }
    if (a_pChip->IsMore()) {
        if (m_fnMore) {
            m_fnMore();
        }
    } else if (m_fnOpen) {
        m_fnOpen(a_pChip->ChipPath());
    }
}

// 键盘方向导航（启动页键盘选工程）：从当前焦点芯片按 a_iKey 移动焦点。
//   ←→ 同行（中心 y 差 < 半高）内相邻；↑↓ 跨行取目标行（行 y 严格分层，
//   |dy| 最小即最近行）内 x 最接近；焦点不在任何可见芯片时 ↓/→ 落首个、
//   ↑/← 落末个。搜索过滤隐藏的芯片不参与。返回 true=已处理（边界：↑ 首行 /
//   ↓ 末行回搜索框、←→ 行端原地不动，防键落到父级被误用）；false=无可聚焦芯片。
bool CRecentChipsPanel::NavigateChips(int a_iKey) {
    const bool bDown  = a_iKey == Qt::Key_Down;
    const bool bUp    = a_iKey == Qt::Key_Up;
    const bool bLeft  = a_iKey == Qt::Key_Left;
    const bool bRight = a_iKey == Qt::Key_Right;
    if (!bDown && !bUp && !bLeft && !bRight) {
        return false;
    }
    QList<CRecentChipButton*> aVis;
    for (CRecentChipButton* pC : m_aChips) {
        if (!pC->isHidden()) {   // 与 Relayout 同口径：isHidden（不受面板显示态影响）
            aVis.append(pC);
        }
    }
    if (aVis.isEmpty()) {
        return false;
    }
    CRecentChipButton* pCur =
        qobject_cast<CRecentChipButton*>(QApplication::focusWidget());
    if (pCur == nullptr || !aVis.contains(pCur)) {
        CRecentChipButton* pT = (bUp || bLeft) ? aVis.last() : aVis.first();
        pT->setFocus(Qt::OtherFocusReason);
        m_pScroll->ensureWidgetVisible(pT, 0, 0);   // 焦点芯片滚入可视区
        return true;
    }
    const QPoint cCur = pCur->geometry().center();
    CRecentChipButton* pBest = nullptr;
    if (bLeft || bRight) {
        int iBest = INT_MAX;
        for (CRecentChipButton* pC : aVis) {
            if (pC == pCur) {
                continue;
            }
            const QPoint c = pC->geometry().center();
            if (qAbs(c.y() - cCur.y()) > kChipH / 2) {
                continue;   // 不同行
            }
            const int d = c.x() - cCur.x();
            if ((bRight && d > 0 && d < iBest)
                    || (bLeft && d < 0 && -d < iBest)) {
                iBest = bRight ? d : -d;
                pBest = pC;
            }
        }
    } else {
        int iBestRow = INT_MAX;
        int iBestDx  = INT_MAX;
        for (CRecentChipButton* pC : aVis) {
            if (pC == pCur) {
                continue;
            }
            const QPoint c = pC->geometry().center();
            const int dy = c.y() - cCur.y();
            if ((bDown && dy <= 0) || (bUp && dy >= 0)) {
                continue;   // 方向不符（同行 dy=0 也不动）
            }
            const int ady = qAbs(dy);
            const int adx = qAbs(c.x() - cCur.x());
            if (ady < iBestRow || (ady == iBestRow && adx < iBestDx)) {
                iBestRow = ady;
                iBestDx  = adx;
                pBest    = pC;
            }
        }
    }
    if (pBest != nullptr) {
        pBest->setFocus(Qt::OtherFocusReason);
        m_pScroll->ensureWidgetVisible(pBest, 0, 0);
    } else if ((bUp || bDown) && m_pEditFilter != nullptr
               && !m_pEditFilter->isHidden()) {
        // 2026.10.12 边界回搜索框（用户需求两轮：首行按 ↑、末行按 ↓ 均回输入
        //   框）——与搜索框 ↓ 跳入芯片区构成完整导航循环（↑↓ 越界即回框，
        //   ←→ 行端仍原地不动）。搜索行隐藏（菜单内嵌形态）不回退焦点原地不动。
        m_pEditFilter->setFocus(Qt::OtherFocusReason);
    }
    return true;   // 已在芯片区：方向键消费掉（边界处按上方规则处置）
}

// 首个芯片聚焦入口（宿主方向键转发；语义=NavigateChips(Down)）。
bool CRecentChipsPanel::FocusFirstChip() {
    return NavigateChips(Qt::Key_Down);
}

// —— 键盘导航观察口（芯片类型为 cpp 内部类，测试经面板下标取属性）——
int CRecentChipsPanel::FocusedChipIndex() const {
    return m_aChips.indexOf(
        qobject_cast<CRecentChipButton*>(QApplication::focusWidget()));
}

QString CRecentChipsPanel::ChipNameAt(int a_iIdx) const {
    return (a_iIdx >= 0 && a_iIdx < m_aNames.size()) ? m_aNames.at(a_iIdx)
                                                     : QString();
}

QString CRecentChipsPanel::ChipPathAt(int a_iIdx) const {
    return (a_iIdx >= 0 && a_iIdx < m_aPaths.size()) ? m_aPaths.at(a_iIdx)
                                                     : QString();
}

int CRecentChipsPanel::ChipYAt(int a_iIdx) const {
    const CRecentChipButton* pC = m_aChips.value(a_iIdx, nullptr);
    return pC != nullptr ? pC->y() : -1;
}

QList<CRecentChipsPanel::SChipRow> CRecentChipsPanel::LayoutChips(
        const QStringList& a_aNames, const QFontMetrics& a_stFm,
        int a_iAvailW, int a_iSpacing, int a_iHPad) {
    QList<SChipRow> aRows;
    if (a_iAvailW <= 0) {
        return aRows;
    }
    const int iCap = qMax(1, a_iAvailW / 2);   // 用户规则 3：单按钮宽上限=客户区一半
    const int iN = a_aNames.size();
    int i = 0;
    while (i < iN) {
        // 1) 贪心装行（保持 MRU 顺序）：+2 = 假粗体 2x2 偏移余量；
        //    iSumPure 只计最小宽（不含行内间距，间距在预算侧统一扣）
        QList<int> aMinW;
        int iSumPure = 0;
        while (i + aMinW.size() < iN) {
            const int iMinW = a_stFm.horizontalAdvance(a_aNames.at(i + aMinW.size()))
                              + 2 * a_iHPad + 2;
            if (!aMinW.isEmpty()
                    && iSumPure + iMinW + a_iSpacing * aMinW.size() > a_iAvailW) {
                break;
            }
            iSumPure += iMinW;
            aMinW.append(iMinW);
        }
        const int n = aMinW.size();
        // 2) 行内富余按最小宽比例分配（标题完整显示 + 行满占满整宽）
        const int iInner = a_iAvailW - a_iSpacing * (n - 1);   // 扣行内间距后的内容预算
        const int iExtra = qMax(0, iInner - iSumPure);
        QList<int> aW;
        int iAlloc = 0;
        int iMaxIdx = 0;
        for (int k = 0; k < n; ++k) {
            const int iWk = aMinW.at(k) + (iSumPure > 0 ? iExtra * aMinW.at(k) / iSumPure : 0);
            aW.append(iWk);
            iAlloc += iWk;
            if (aMinW.at(k) > aMinW.at(iMaxIdx)) { iMaxIdx = k; }
        }
        if (n > 0 && iAlloc < iInner) {
            aW[iMaxIdx] += iInner - iAlloc;   // 整数化零头归最宽者（保证行满）
        }
        // 3) 单按钮宽钳到上限（触发时行尾留白，用户规则 3）；超长独行仍可省略号兜底
        for (int k = 0; k < n; ++k) {
            aW[k] = qBound(1, qMin(aW.at(k), iCap), a_iAvailW);
        }
        SChipRow stRow;
        stRow.iFirst = i;
        stRow.aBtnW = aW;
        aRows.append(stRow);
        i += n;
    }
    return aRows;
}

QColor CRecentChipsPanel::ChipColorForCount(int a_iCount) {
    // 用户规则 7：1~5 基色不变 → (5,100] R 升 G/B 降，100 次=(255,0,0) →
    //   (100,255] R 降至 (100,0,0)，255 次=(100,0,0) → >255 恒 (100,0,0)。
    static const QColor k_clBase(225, 230, 238);   // 基色：中性浅灰蓝
    if (a_iCount <= 5) {
        return k_clBase;
    }
    if (a_iCount <= 100) {
        const double fT = double(a_iCount - 5) / 95.0;
        return QColor(k_clBase.red() + int((255 - k_clBase.red()) * fT),
                      int(double(k_clBase.green()) * (1.0 - fT)),
                      int(double(k_clBase.blue()) * (1.0 - fT)));
    }
    if (a_iCount <= 255) {
        const double fT = double(a_iCount - 100) / 155.0;
        return QColor(255 - int(155 * fT), 0, 0);
    }
    return QColor(100, 0, 0);
}

#include "CRecentChipsPanel.moc"
