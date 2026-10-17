#include "CRecentProjectsDialog.h"
#include "core/CVersion.h"   // 2026.10.11 启动页标题栏版本号显示
#include "ui/CMenuIcon.h"
#include "ui/CRecentChipsPanel.h"   // 2026.10.11 启动页按钮面板主体

#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>   // 2026.10.11 启动页「打开项目...」选择工程目录
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QCloseEvent>
#include <QHideEvent>
#include <QKeyEvent>   // 2026.10.12 键盘导航（搜索框 ↓ / 启动页方向键）
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRect>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>

// 2026.09.20 几何持久化：profile 读写（同 CRollbackDialog 单一写入者家族）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// 2026.10.09 启动页品牌 Title 横幅（文件局部）：浅色高科技背景图
//   （:/icons/launcher-title.png，等比放大铺满 + 居中裁剪）+ 深色 AutoCode
//   主字/英文副字垂直居中。高恒 kTitleBannerH DIP ≈ 三行 11 号按钮高度；文字用
//   QLabel 叠加（非位图），任意缩放/DPI 下恒清晰。
static constexpr int kTitleBannerH = 100;
namespace {
class CTitleBanner : public QWidget {
public:
    explicit CTitleBanner(QWidget* a_pParent = nullptr) : QWidget(a_pParent) {
        setFixedHeight(kTitleBannerH);
        m_PmBg.load(QStringLiteral(":/icons/launcher-title.png"));
        auto* pLay = new QVBoxLayout(this);
        pLay->setContentsMargins(0, 0, 0, 6);
        pLay->addStretch(1);
        auto* pLblBrand = new QLabel(QStringLiteral("AutoCode"), this);
        QFont stF = pLblBrand->font();
        stF.setPointSize(28);
        stF.setBold(true);
        stF.setLetterSpacing(QFont::AbsoluteSpacing, 6.0);
        pLblBrand->setFont(stF);
        pLblBrand->setAlignment(Qt::AlignHCenter);
        pLblBrand->setStyleSheet(QStringLiteral("color:#173A5E;background:transparent;"));
        auto* pLblSub = new QLabel(QStringLiteral("AI CODING WORKBENCH"), this);
        QFont stFs = pLblSub->font();
        stFs.setPointSize(9);
        stFs.setLetterSpacing(QFont::AbsoluteSpacing, 4.0);
        pLblSub->setFont(stFs);
        pLblSub->setAlignment(Qt::AlignHCenter);
        pLblSub->setStyleSheet(QStringLiteral("color:#5B7EA6;background:transparent;"));
        pLay->addWidget(pLblBrand, 0, Qt::AlignHCenter);
        pLay->addSpacing(2);
        pLay->addWidget(pLblSub, 0, Qt::AlignHCenter);
        pLay->addStretch(1);
        // 2026.10.11 版本号：右下角小字（叠加层，resizeEvent 定位；与主窗口标题
        //   「工程名-AutoCode vX.Y.N」同版本源 AutoCodeVersionText）。
        m_pLblVer = new QLabel(AutoCodeVersionText(), this);
        QFont stFv = m_pLblVer->font();
        stFv.setPointSize(8);
        m_pLblVer->setFont(stFv);
        m_pLblVer->setStyleSheet(QStringLiteral("color:#6B8CB5;background:transparent;"));
        m_pLblVer->adjustSize();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!m_PmBg.isNull()) {
            // 等比放大铺满 + 居中裁剪（窗口拉宽时网格纹理不拉伸变形）。
            const QSize szSrc(m_PmBg.width(), m_PmBg.height());
            const qreal dScale = qMax(static_cast<qreal>(width()) / szSrc.width(),
                                      static_cast<qreal>(height()) / szSrc.height());
            const QSizeF szDst(szSrc.width() * dScale, szSrc.height() * dScale);
            const QPointF ptTop((width() - szDst.width()) / 2.0,
                                (height() - szDst.height()) / 2.0);
            p.drawPixmap(QRectF(ptTop, szDst).toRect(), m_PmBg,
                         QRect(QPoint(0, 0), szSrc));
        } else {
            // 图缺失兑底：纯浅色底（保证文字可读，不空白）。
            p.fillRect(rect(), QColor(0xEF, 0xF5, 0xFD));
        }
    }

    void resizeEvent(QResizeEvent*) override {
        // 版本号右下角定位（宽随窗口拉伸）。
        if (m_pLblVer != nullptr) {
            m_pLblVer->move(width() - m_pLblVer->width() - 10,
                            height() - m_pLblVer->height() - 5);
        }
    }

private:
    QPixmap m_PmBg;
    QLabel* m_pLblVer = nullptr;
};
} // namespace

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent)
    : CRecentProjectsDialog(a_aRecent, QList<int>(), QString(), a_pParent) {}

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent,
                                             const QList<int>& a_aOpens,
                                             QWidget* a_pParent)
    : CRecentProjectsDialog(a_aRecent, a_aOpens, QString(), a_pParent) {}

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent,
                                             const QList<int>& a_aOpens,
                                             const QString& a_sCfgPath, QWidget* a_pParent)
    : CRecentProjectsDialog(a_aRecent, a_aOpens, a_sCfgPath, EMode::kNormal, a_pParent) {}

// 2026.10.09 全量构造（EMode 版）：kNormal 行为与旧版逐行一致；kLauncher 为
//   启动页形态（Title 横幅 + CRecentChipsPanel 按钮面板主体 + 跳过/退出底钮，
//   无搜索框/列表/OK 按钮；几何见 Restore/SaveLauncherGeometry）。
CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent,
                                             const QList<int>& a_aOpens,
                                             const QString& a_sCfgPath, EMode a_eMode,
                                             QWidget* a_pParent)
    : QDialog(a_pParent), m_sCfgPath(a_sCfgPath), m_eMode(a_eMode) {
    setWindowTitle(QStringLiteral("最近打开的工程"));
    resize(620, 420);

    // 2026.08.20 整窗 11 号字体（标题栏随系统，客户区含按钮统一）。
    QFont stFont = font();
    stFont.setPointSize(11);
    setFont(stFont);

    // 全量快照入 m_aAll（两种模式共用：列表版重建 / 按钮版 Refresh 的数据源）。
    m_aAll = a_aRecent;
    // 2026.09.20 打开次数对齐（缺省/旧档 → 1；异常长表截齐）。
    m_aOpens = a_aOpens;
    while (m_aOpens.size() > m_aAll.size()) {
        m_aOpens.removeLast();
    }
    while (m_aOpens.size() < m_aAll.size()) {
        m_aOpens.append(1);
    }

    // 2026.10.11 kLauncher：主体是按钮面板，无搜索框/列表；底钮仅「跳过/退出」。
    const bool bLauncher = (m_eMode == EMode::kLauncher);
    if (!bLauncher) {
        // 搜索框（列表最上方）：内容变化实时过滤列表（名称或路径包含即命中，不区分大小写）。
        m_pSearch = new QLineEdit(this);
        m_pSearch->installEventFilter(this);   // 2026.10.12 搜索框 ↓ → 列表首行
        m_pSearch->setFont(stFont);
        m_pSearch->setPlaceholderText(QStringLiteral("搜索工程（名称 / 路径）…"));
        m_pSearch->setClearButtonEnabled(true);
        connect(m_pSearch, &QLineEdit::textChanged,
                this, &CRecentProjectsDialog::OnSearchChanged);

        m_pList = new QListWidget(this);
        m_pList->setFont(stFont);
        m_pList->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 多选
        m_pList->setContextMenuPolicy(Qt::CustomContextMenu);             // 右键菜单
        connect(m_pList, &QListWidget::customContextMenuRequested,
                this, &CRecentProjectsDialog::OnContextMenu);
        RebuildList(QString());
    }
    RestoreGeometryFromConfig();   // 2026.09.20 几何持久化恢复（无档=缺省 620×420）

    // 2026.10.11 按钮版启动页底钮为自建「退出/跳过」行（见 SetupLauncherUi），
    //   不再创建 QDialogButtonBox（旧版 OK/Cancel 仅列表版需要）。
    if (!bLauncher) {
        m_pBtns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(m_pBtns, &QDialogButtonBox::accepted, this, [this]() {
            // OK：优先当前项，其次第一个选中项；都无则保持空（由调用方判空）。
            ApplyPath(m_pList->currentItem());
            if (m_sSelected.isEmpty()) {
                ApplyPath(m_pList->selectedItems().isEmpty()
                              ? nullptr : m_pList->selectedItems().first());
            }
            accept();
        });
        connect(m_pBtns, &QDialogButtonBox::rejected, this, [this]() {
            m_sSelected.clear();
            reject();
        });

        // 双击 = 确认该项（仅列表版；launcher 点按钮即开，见 SetupLauncherUi）。
        connect(m_pList, &QListWidget::itemDoubleClicked,
                this, &CRecentProjectsDialog::OnItemDoubleClicked);
    }

    auto* pV = new QVBoxLayout(this);
    if (bLauncher) {
        pV->setContentsMargins(0, 0, 0, 0);
        pV->setSpacing(0);
        SetupLauncherUi(pV);   // Title 横幅 + 按钮面板 + 底钮行（见函数注释）
        return;
    }
    pV->addWidget(m_pSearch);
    pV->addWidget(m_pList);
    pV->addWidget(m_pBtns);
}

// 2026.10.11 kLauncher 专用搭建（按钮版）：顶部品牌横幅（CTitleBanner，高恒
//   100 DIP）+ 主体 CRecentChipsPanel（复用组件：点击即开、悬停状态栏显路径、
//   右键删除、拖右/下边改尺寸、尺寸档 [Layout] RecentChipsSize 与菜单内弹出
//   面板共用）+ 底钮行「退出 | 跳过」（退出直接结束进程；跳过进空主界面）。
void CRecentProjectsDialog::SetupLauncherUi(QVBoxLayout* a_pMainLay) {
    // 2026.10.11 标题栏带版本号（系统标题栏：任务管理器/Alt-Tab 可辨认，与主窗口
    //   「工程名-AutoCode vX.Y.N」同源；品牌横幅右下角另有小字版本）。
    setWindowTitle(QStringLiteral("AutoCode ") + AutoCodeVersionText());
    a_pMainLay->insertWidget(0, new CTitleBanner(this));

    // 按钮面板主体：回调注入——点工程=选中并 accept；更多...=弹完整列表；
    //   右键删除=从数据源移除并刷新（RemovedPaths 统一上报）。
    m_pChips = new CRecentChipsPanel(
        [this](const QString& a_sPath) {          // FOpenProject：点按钮即开
            m_sSelected = a_sPath;
            accept();
        },
        [this]() {                                  // FMore：完整列表（搜索/多选删除）
            CRecentProjectsDialog stMore(m_aAll, m_aOpens, m_sCfgPath, this);
            const int iRc = stMore.exec();
            for (const QString& sP : stMore.RemovedPaths()) {
                RemoveAndRefresh(sP);
            }
            if (iRc == QDialog::Accepted && !stMore.SelectedPath().isEmpty()) {
                m_sSelected = stMore.SelectedPath();
                accept();
            }
        },
        this,
        [this]() -> QString { return m_sCfgPath; }, // FCfgPath：面板自读/写 RecentChipsSize
        [this](const QString& a_sPath) {           // FRemoveProject：右键删除
            RemoveAndRefresh(a_sPath);
        });
    // 2026-10-11 启动页搜索行：面板顶部固定一行搜索框（替代「更多...」芯片），
    //   输入实时过滤工程按钮——启动页场景全量列表入口冗余，过滤即搜索。
    m_pChips->SetFilterBar(true);
    // 面板尺寸外部变化（拖边/初建 Refresh）→ 窗口包紧（重入护栏见 eventFilter）。
    m_pChips->installEventFilter(this);
    m_pChips->Refresh(m_aAll, m_aOpens);   // 首刷：载入 RecentChipsSize 用户尺寸/自动尺寸
    a_pMainLay->addWidget(m_pChips);

    // 底钮行：退出（左）| 跳过（右）；留白边距与面板视觉分隔。
    m_pBtnRow = new QWidget(this);
    auto* pH = new QHBoxLayout(m_pBtnRow);
    pH->setContentsMargins(8, 4, 8, 8);
    auto* pBtnExit = new QPushButton(QStringLiteral("退出"), m_pBtnRow);
    connect(pBtnExit, &QPushButton::clicked, this, [this]() {
        m_bExitRequested = true;
        m_sSelected.clear();
        reject();
    });
    auto* pBtnSkip = new QPushButton(QStringLiteral("跳过"), m_pBtnRow);
    pBtnSkip->setDefault(true);   // Enter = 跳过（无列表后 OK 语义的接棒者）
    connect(pBtnSkip, &QPushButton::clicked, this, [this]() {
        m_bSkipRequested = true;   // 2026.10.14 main 据此改开 exe 同级 AutoCode 工作目录
        m_sSelected.clear();
        reject();
    });
    // 2026.10.11 「打开项目...」（跳过左边）：浏览选择任意工程目录（不限于最近
    //   列表）→ 与点 chip 同语义选中并 accept（取消则留在启动页）。
    auto* pBtnOpen = new QPushButton(QStringLiteral("打开项目..."), m_pBtnRow);
    connect(pBtnOpen, &QPushButton::clicked, this, [this]() {
        const QString sDir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择工程目录"), QString(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!sDir.isEmpty()) {
            m_sSelected = sDir;
            accept();
        }
    });
    pH->addWidget(pBtnExit, 0, Qt::AlignLeft);
    pH->addStretch(1);
    pH->addWidget(pBtnOpen);
    pH->addWidget(pBtnSkip);
    // 2026.10.11 固定底钮行高：若可拉伸，布局会把富余空间全给本行（面板 fixed
    //   不伸缩）→ SyncPanelToWindow 的目标尺寸恒等于面板当前值（幂等死锁），
    //   窗口拖大后面板永不跟随。固定后富余只留给面板（sync 随后填满）。
    m_pBtnRow->setFixedHeight(m_pBtnRow->sizeHint().height());
    a_pMainLay->addWidget(m_pBtnRow);

    // 窗口初始尺寸 = 面板初始固定尺寸 + Title + 底钮（SyncWindowToPanel 复用）；
    // 位置由 RestoreLauncherGeometry 处理（面板尺寸档即 RecentChipsSize）。
    SyncWindowToPanel();
    RestoreLauncherGeometry();
}

QString CRecentProjectsDialog::SelectedPath() const {
    return m_sSelected;
}

QStringList CRecentProjectsDialog::RemovedPaths() const {
    return m_aRemoved;
}

void CRecentProjectsDialog::ApplyPath(QListWidgetItem* a_pItem) {
    m_sSelected = (a_pItem == nullptr)
                      ? QString()
                      : a_pItem->data(Qt::UserRole).toString();
}

void CRecentProjectsDialog::RebuildList(const QString& a_sFilter) {
    // 依据过滤词重建列表：每项文本「目录名（完整路径）— N 次」（N=打开次数，
    //   2026.09.20 工程使用/修改频率统计），UserRole 存绝对路径。
    // 已被右键删除的路径不再出现（防止过滤重建时复活已删条目）。
    m_pList->clear();
    for (int i = 0; i < m_aAll.size(); ++i) {
        const QString& sPath = m_aAll.at(i);
        if (m_aRemoved.contains(sPath)) {
            continue;
        }
        const QString sName = QFileInfo(sPath).fileName();
        if (!a_sFilter.isEmpty()) {
            // 大小写不敏感：名称或完整路径包含关键词即命中。
            if (!sName.contains(a_sFilter, Qt::CaseInsensitive) &&
                !sPath.contains(a_sFilter, Qt::CaseInsensitive)) {
                continue;
            }
        }
        const int iOpens = (i < m_aOpens.size() && m_aOpens.at(i) > 0)
                               ? m_aOpens.at(i) : 1;
        const QString sText = QStringLiteral("%1（%2）— %3 次")
                                  .arg(sName, sPath)
                                  .arg(iOpens);
        auto* pItem = new QListWidgetItem(sText, m_pList);
        pItem->setData(Qt::UserRole, sPath);
        pItem->setToolTip(QStringLiteral("已打开 %1 次（每次成功打开工程时累加 1）")
                              .arg(iOpens));
    }
    // 重建后确认路径可能已不在可视列表 → 取第一条兜底为当前项（不改 m_sSelected）。
    if (m_pList->count() > 0) {
        m_pList->setCurrentRow(0);
    }
}

void CRecentProjectsDialog::OnSearchChanged(const QString& a_sText) {
    RebuildList(a_sText.trimmed());
}

void CRecentProjectsDialog::OnItemDoubleClicked() {
    // 双击当前项 → 记录路径并 accept。
    ApplyPath(m_pList->currentItem());
    accept();
}

void CRecentProjectsDialog::OnContextMenu(const QPoint& a_pos) {
    // 右键：光标处若有项则并入选中集，再弹「删除选中」菜单。
    if (auto* pAt = m_pList->itemAt(a_pos)) {
        if (!pAt->isSelected()) {
            m_pList->setCurrentItem(pAt);   // 未选中的右键项 → 置为当前（单选语义）
        }
    }
    if (m_pList->selectedItems().isEmpty()) {
        return;   // 空白处右键且无选中 → 不弹菜单
    }
    QMenu stMenu(this);
    stMenu.setFont(font());
    QAction* pActDel = stMenu.addAction(
        MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除选中（%1）").arg(m_pList->selectedItems().size()));
    connect(pActDel, &QAction::triggered, this, &CRecentProjectsDialog::OnDeleteSelected);
    stMenu.exec(m_pList->mapToGlobal(a_pos));
}

void CRecentProjectsDialog::OnDeleteSelected() {
    // 从列表移除并记录路径；只动列表副本，MRU 持久化由调用方经 RemovedPaths() 处理。
    const QList<QListWidgetItem*> aSel = m_pList->selectedItems();
    for (QListWidgetItem* pItem : aSel) {
        const QString sPath = pItem->data(Qt::UserRole).toString();
        if (!sPath.isEmpty() && !m_aRemoved.contains(sPath)) {
            m_aRemoved.append(sPath);
        }
        delete pItem;   // QListWidgetItem 析构自动从列表摘除
    }
    // 确认路径若刚被删除 → 清空，防止 OK 取到已删条目。
    if (!m_sSelected.isEmpty() && m_aRemoved.contains(m_sSelected)) {
        m_sSelected.clear();
    }
}

// —— 2026.09.20 几何持久化（[Layout] RecentWin=x,y,w,h；口径同 CRollbackDialog：显式
//    整型串 + 屏幕可用区限幅 + profile 写缓存冲刷；空 cfg 路径=禁用，测试不写盘）——

namespace {
// 恰好 4 个整数（允许负数——多屏左侧坐标）才通过；空/个数不符/非数字返回空表。
QList<int> SplitFourInts(const QString& a_s) {
    if (a_s.isEmpty()) {
        return {};
    }
    const QStringList aParts = a_s.split(QLatin1Char(','));
    if (aParts.size() != 4) {
        return {};
    }
    QList<int> v;
    for (const QString& sP : aParts) {
        bool bOk = false;
        const int i = sP.toInt(&bOk);
        if (!bOk) {
            return {};
        }
        v.append(i);
    }
    return v;
}
} // namespace

void CRecentProjectsDialog::RestoreGeometryFromConfig() {
    // 2026.10.11 kLauncher：几何统一在 SetupLauncherUi 尾部处理（窗口尺寸需先由
    //   面板初始尺寸推导——构造中段面板尚未创建），此处直接让位。
    if (m_eMode == EMode::kLauncher) {
        return;
    }
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    wchar_t szBuf[64] = {0};
    GetPrivateProfileStringW(L"Layout", L"RecentWin", L"", szBuf, 64,
                             (LPCWSTR)m_sCfgPath.utf16());
    const QList<int> aWin = SplitFourInts(QString::fromWCharArray(szBuf));
    if (aWin.size() != 4) {
        return;   // 无档/坏档：保持构造缺省 620x420
    }
    // 限幅到虚拟桌面可用区内（宽 ≥420、高 ≥300、不超屏；坐标移入可见区——
    //   多屏/分辨率变化后不出屏外窗口；负坐标在屏内合法放行）。
    const QRect rcDesk = QGuiApplication::primaryScreen()->availableVirtualGeometry();
    const int iW = qBound(420, aWin[2], rcDesk.width());
    const int iH = qBound(300, aWin[3], rcDesk.height());
    // 2026.09.25 修「每开一次上漂一个标题栏高度」：setGeometry 改 move()+resize()。
    //   本机实测（build/probe_recentdrift4 四变体对照）：Qt6.8.3 顶层窗口 setGeometry
    //   的客户区落点恒比请求 y 高 31px（构造期/showEvent/显示后调用均同），
    //   hideEvent 把偏移值存回 → 下次继续偏、逐次上漂；move()+resize() 走精确
    //   客户区路径不漂（回归：test_execpanel recentDialogGeometryNoDrift）。
    move(QPoint(qBound(rcDesk.left(), aWin[0], rcDesk.right() - iW + 1),
                qBound(rcDesk.top(), aWin[1], rcDesk.bottom() - iH + 1)));
    resize(iW, iH);
}

void CRecentProjectsDialog::SaveGeometryToConfig() {
    // 2026.10.09 kLauncher：位置落 RecentLauncherPos，尺寸回写 RecentWin
    //   （保留其原 x,y）——见 SaveLauncherGeometry。
    if (m_eMode == EMode::kLauncher) {
        SaveLauncherGeometry();
        return;
    }
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    const QString sVal = QStringLiteral("%1,%2,%3,%4")
                             .arg(x()).arg(y()).arg(width()).arg(height());
    WritePrivateProfileStringW(L"Layout", L"RecentWin",
                               (LPCWSTR)sVal.utf16(), (LPCWSTR)m_sCfgPath.utf16());
    // 冲刷 profile 写缓存（v1.3.78 教训：延迟落盘会让下一次打开读到空 → 「没持久化」）。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)m_sCfgPath.utf16());
}

void CRecentProjectsDialog::hideEvent(QHideEvent* a_pEv) {
    QDialog::hideEvent(a_pEv);
    // 隐藏（含 accept/reject/done 关闭路径——QDialog::done 内部走 hide）时落盘。
    SaveGeometryToConfig();
}

// 2026.10.11 kLauncher：点右上角关闭（X）= 直接退出程序（同「退出」按钮）：置
//   ExitRequested 后走 QDialog 默认 reject（hideEvent 几何落盘照常）。Esc/
//   「跳过」不经 closeEvent，仍进主窗口——两种关闭语义区分开。
void CRecentProjectsDialog::closeEvent(QCloseEvent* a_pEv) {
    if (m_eMode == EMode::kLauncher) {
        m_bExitRequested = true;
        m_sSelected.clear();
    }
    QDialog::closeEvent(a_pEv);
}

// —— 2026.10.09 kLauncher 启动页几何（与普通模式尺寸双向同步、位置独立）——

void CRecentProjectsDialog::RestoreLauncherGeometry() {
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    // 2026.10.11 按钮版：窗口尺寸已由面板初始尺寸推导（SetupLauncherUi 尾
    //   SyncWindowToPanel，面板尺寸档即 [Layout] RecentChipsSize），此处只管
    //   位置：独立键 RecentLauncherPos（无档/坏档 = 主屏居中——首次出场姿态）。
    const QRect rcDesk = QGuiApplication::primaryScreen()->availableVirtualGeometry();
    wchar_t szPos[32] = {0};
    GetPrivateProfileStringW(L"Layout", L"RecentLauncherPos", L"", szPos, 32,
                             (LPCWSTR)m_sCfgPath.utf16());
    const QStringList aPos = QString::fromWCharArray(szPos).split(QLatin1Char(','));
    if (aPos.size() == 2) {
        bool bOkX = false;
        bool bOkY = false;
        const int iX = aPos.at(0).toInt(&bOkX);
        const int iY = aPos.at(1).toInt(&bOkY);
        if (bOkX && bOkY) {
            move(QPoint(qBound(rcDesk.left(), iX, rcDesk.right() - width() + 1),
                        qBound(rcDesk.top(), iY, rcDesk.bottom() - height() + 1)));
            return;
        }
    }
    const QRect rcScreen = QGuiApplication::primaryScreen()->availableGeometry();
    move(rcScreen.center() - QPoint(width() / 2, height() / 2));
}

void CRecentProjectsDialog::SaveLauncherGeometry() {
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    // 位置独立落盘（与列表版「最近打开」窗口互不干扰）。
    const QString sPos = QStringLiteral("%1,%2").arg(x()).arg(y());
    WritePrivateProfileStringW(L"Layout", L"RecentLauncherPos",
                               (LPCWSTR)sPos.utf16(), (LPCWSTR)m_sCfgPath.utf16());
    // 2026.10.11 按钮区域尺寸回写 [Layout] RecentChipsSize（与菜单内弹出面板
    //   同步：面板自身拖边路径由面板 SaveUserSize 落盘，此处兑底窗口拖边路径）。
    if (m_pChips != nullptr) {
        const QString sSz = QStringLiteral("%1,%2")
                                .arg(m_pChips->width()).arg(m_pChips->height());
        WritePrivateProfileStringW(L"Layout", L"RecentChipsSize",
                                   (LPCWSTR)sSz.utf16(), (LPCWSTR)m_sCfgPath.utf16());
    }
    // 冲刷 profile 写缓存（同 SaveGeometryToConfig 口径）。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)m_sCfgPath.utf16());
}

// —— 2026.10.11 kLauncher 窗口↔面板尺寸双向同步 ——

void CRecentProjectsDialog::SyncPanelToWindow() {
    if (m_pChips == nullptr || m_pBtnRow == nullptr) {
        return;
    }
    // 面板目标尺寸 = 客户区减 Title（100）与底钮行实际高（未布局时用 sizeHint）。
    const int iBtnH = qMax(m_pBtnRow->height(), m_pBtnRow->sizeHint().height());
    const QSize szT(width(), height() - kTitleBannerH - iBtnH);
    if (szT != m_pChips->size() && szT.width() > 0 && szT.height() > 0) {
        m_bSyncingGeom = true;   // 面板 Resize → eventFilter 跳过（防乒乓）
        // 2026.10.11 走 ApplyWindowSize：同步面板内部用户尺寸 + 立即落盘——
        //   直接 setFixedSize 会与 m_iUserW/H 脱节，下次 Refresh 回退旧尺寸、
        //   hide 落盘又覆盖新尺寸（实测串档）。
        m_pChips->ApplyWindowSize(szT.width(), szT.height());
        m_bSyncingGeom = false;
    }
}

void CRecentProjectsDialog::SyncWindowToPanel() {
    if (m_pChips == nullptr || m_pBtnRow == nullptr) {
        return;
    }
    const int iBtnH = qMax(m_pBtnRow->height(), m_pBtnRow->sizeHint().height());
    const QSize szT(m_pChips->width(), m_pChips->height() + kTitleBannerH + iBtnH);
    if (szT != size()) {
        m_bSyncingGeom = true;   // 窗口 resize → resizeEvent 跳过（防乒乓）
        resize(szT);
        m_bSyncingGeom = false;
    }
}

void CRecentProjectsDialog::RemoveAndRefresh(const QString& a_sPath) {
    if (a_sPath.isEmpty() || m_aAll.contains(a_sPath) == false) {
        return;
    }
    // 从数据源移除（计数同步删）并记录上报集合；列表版（更多...）与面板右键共用。
    const int iIdx = m_aAll.indexOf(a_sPath);
    m_aAll.removeAt(iIdx);
    if (iIdx < m_aOpens.size()) {
        m_aOpens.removeAt(iIdx);
    }
    if (!m_aRemoved.contains(a_sPath)) {
        m_aRemoved.append(a_sPath);
    }
    if (m_sSelected == a_sPath) {
        m_sSelected.clear();   // 已删工程不再可被选中
    }
    if (m_pChips != nullptr) {
        m_pChips->Refresh(m_aAll, m_aOpens);
        // Refresh 重设面板固定尺寸 → 窗口包紧（Refresh 读档尺寸应与当前一致；
        //   数据变少自动高度变化时窗口跟随）。
        SyncWindowToPanel();
    }
}

void CRecentProjectsDialog::resizeEvent(QResizeEvent* a_pEv) {
    QDialog::resizeEvent(a_pEv);
    // kLauncher：窗口拖边/缩放 → 面板跟随（重入护栏内跳过——SyncWindowToPanel
    //   的 resize 会再进这里，值已收敛不循环）。
    if (m_eMode == EMode::kLauncher && !m_bSyncingGeom) {
        SyncPanelToWindow();
    }
}

bool CRecentProjectsDialog::eventFilter(QObject* a_pObj, QEvent* a_pEv) {
    // kLauncher：面板尺寸被外部改变（拖面板右/下边、Refresh 重设固定尺寸）
    //   → 窗口包紧面板（护栏内跳过：SyncPanelToWindow 的 setFixedSize 触发本过滤器）。
    if (a_pObj == m_pChips && a_pEv->type() == QEvent::Resize && !m_bSyncingGeom) {
        SyncWindowToPanel();
    }
    // 2026.10.12 完整列表搜索框内按 ↓：焦点跳到列表首行（键盘可达性——搜索后
    //   直接回车打开，不必摸鼠标；↑ 不拦保留光标编辑语义）。
    if (a_pObj == m_pSearch && a_pEv->type() == QEvent::KeyPress
            && m_pList != nullptr && m_pList->count() > 0) {
        auto* pKEv = static_cast<QKeyEvent*>(a_pEv);
        if (pKEv->key() == Qt::Key_Down) {
            m_pList->setFocus(Qt::OtherFocusReason);
            m_pList->setCurrentRow(0);
            a_pEv->accept();
            return true;
        }
    }
    return QDialog::eventFilter(a_pObj, a_pEv);
}

// 2026.10.12 启动页：焦点不在芯片区（底钮/无焦点）时按方向键 → 焦点跳入工程
//   按钮区（搜索框内 ↓ 已被面板内部过滤器消费，不会到这）；其余键交 QDialog
//   （Enter=缺省钮、Esc=关闭）。
void CRecentProjectsDialog::keyPressEvent(QKeyEvent* a_pEv) {
    const int iKey = a_pEv->key();
    if (m_eMode == EMode::kLauncher && m_pChips != nullptr
            && (iKey == Qt::Key_Down || iKey == Qt::Key_Up
                || iKey == Qt::Key_Left || iKey == Qt::Key_Right)) {
        if (m_pChips->NavigateChips(iKey)) {
            a_pEv->accept();
            return;
        }
    }
    QDialog::keyPressEvent(a_pEv);
}
