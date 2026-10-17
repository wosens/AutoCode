#pragma once
// ============================================================================
// CRecentChipsPanel —— 「最近打开」按钮面板（2026.09.26 v3.1.239）
//
// 形态：工程菜单「最近打开」子菜单内的按钮面板（自绘按钮 + 流式行布局 +
//   底部状态栏），取代旧的 20 条 QAction 列表。宿主方式 = QWidgetAction
//   内嵌进 m_pMenuRecent（关键决策，勿改成独立弹出窗口）：QMenu 打开期间
//   独占鼠标事件路由，独立顶层窗口收不到点击；内嵌则子菜单全部语义白拿——
//   悬停「最近打开」自动弹出（位置与原级联菜单一致）、鼠标移到其他菜单项
//   自动隐藏、面板交互期间菜单保持显示、点击工程按钮才收起整条菜单链。
//
// 交互（用户 2026.09.26 设计定稿）：
//   ① 按钮宽度按面板宽自动计算：行内按内容最小宽比例分配富余空间（每行
//      占满整宽、每行个数不同、标题不截断），上限=面板客户区一半；
//   ② 首个按钮「更多...」→ 完整对话框（搜索/多选/右键批量删除/几何持久化）；
//   ③ 可视区约 40~50 个工程（9 行封顶），滚轮滚动查看其余；
//   ④ 底部状态栏：悬停按钮时显示该工程路径（中段省略），平时显示汇总；
//   ⑤ 悬停=蓝底(#2563eb)白字 + 假粗体（2x2 四次绘制——字号/字宽不变，
//      布局零抖动，用户方案）；
//   ⑥ 平时底色按打开次数渐变：1~5 基色不变 → 100 次=(255,0,0) →
//      255 次=(100,0,0)；打开次数在 tooltip 中显示。
// ============================================================================
#include <QWidget>
#include <QColor>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QScrollArea;
class CRecentChipButton;

class CRecentChipsPanel : public QWidget {
    Q_OBJECT
public:
    // 打开工程回调（路径）/「更多...」回调——均由 CMainWindow 注入；
    //   回调内负责先收起菜单链再执行动作。
    using FOpenProject = std::function<void(const QString&)>;
    using FMore = std::function<void()>;
    // 2026-10-09 持久化路径提供器（调用时取当前 config.ini 全路径；空返回 = 禁用读写）：
    //   面板构造时 PM 可能尚未就绪，传函数而非值；键 [Layout] RecentChipsSize="w,h"。
    using FCfgPath = std::function<QString()>;
    // 2026-10-09 右键删除：从最近打开列表移除指定工程（CMainWindow 注入：
    //   PM RemoveRecent + 面板 Refresh；空回调 = 右键菜单「删除」项不触发）。
    using FRemoveProject = std::function<void(const QString&)>;

    CRecentChipsPanel(FOpenProject a_fnOpen, FMore a_fnMore,
                      QWidget* a_pParent = nullptr,
                      const FCfgPath& a_fnCfgPath = FCfgPath(),
                      const FRemoveProject& a_fnRemove = FRemoveProject());

    // 刷新数据（菜单 aboutToShow 时调用，MRU 顺序；计数缺失按 1）。
    void Refresh(const QStringList& a_aPaths, const QList<int>& a_aOpens);

    // 2026.10.11 宿主（启动页）窗口拖边驱动的尺寸变更：钳位落尺寸 + 同步内部
    //   用户尺寸（下次 Refresh 不回退旧尺寸）+ 立即落盘（与面板自身拖边同语义）。
    void ApplyWindowSize(int a_iW, int a_iH) {
        ApplyUserSize(a_iW, a_iH);
        SaveUserSize();
    }

    // —— 纯函数（单测直调，勿依赖实例状态）——
    // 流式行布局：返回每行（首下标 + 行内各按钮宽度）。a_iAvailW=可用内容宽，
    //   a_iHPad=按钮左右内边距；行内按最小宽比例分配富余（保证标题完整显示、
    //   行满占满整宽），单按钮宽度上限=a_iAvailW/2（用户规则 3，触发时行尾留白）。
    struct SChipRow {
        int iFirst = 0;
        QList<int> aBtnW;
    };
    static QList<SChipRow> LayoutChips(const QStringList& a_aNames,
                                       const QFontMetrics& a_stFm,
                                       int a_iAvailW, int a_iSpacing,
                                       int a_iHPad);
    // 打开次数 → 底色（用户规则 7）：1~5 基色；(5,100] R 升 G/B 降至 (255,0,0)；
    //   (100,255] R 降至 (100,0,0)；>255 恒 (100,0,0)。
    static QColor ChipColorForCount(int a_iCount);

    // 供测试观察：当前按钮列表（首个恒为「更多...」）与状态栏文本。
    QList<CRecentChipButton*> Chips() const { return m_aChips; }
    QWidget* ChipAt(int a_iIdx) const;   // 按下标取按钮（QWidget* 观察口，测试/诊断用）
    QString StatusText() const;
    // 2026.10.12 键盘导航（启动页键盘选工程）：搜索框内按 ↓ 焦点跳入芯片区，
    //   方向键在芯片间移动（←→同行、↑↓跨行 x 最近）、回车/空格=打开（与点击同
    //   语义）。NavigateChips 焦点不在芯片时 Down/Right 落首个、Up/Left 落末个；
    //   搜索过滤隐藏的芯片不参与；返回 true=已处理（宿主据此吃键）。
    bool FocusFirstChip();
    bool NavigateChips(int a_iKey);   // a_iKey ∈ Qt::Key_Down/Up/Left/Right
    // —— 2026.10.12 键盘导航观察口（测试/诊断用；芯片类型为 cpp 内部类不导出）——
    int     FocusedChipIndex() const;              // 当前焦点芯片下标（-1=焦点不在芯片）
    QString ChipNameAt(int a_iIdx) const;
    QString ChipPathAt(int a_iIdx) const;
    int     ChipYAt(int a_iIdx) const;             // 芯片宿主内 y（跨行断言用，越界=-1）

    // 2026-10-09 用户拖拽尺寸观察口（0=未自定义，自动尺寸）；测试断言用。
    QSize UserSize() const { return QSize(m_iUserW, m_iUserH); }
    // 2026-10-09 右键删除执行链（菜单动作直调；测试直调验证回调链）。
    void DoRemoveChip(const QString& a_sPath);
    // 2026-10-09 右键上下文菜单观察口（exec 期间测试 singleShot 触发删除用）。
    QMenu* LastCtxMenu() const { return m_pCtxMenu.get(); }
    // 2026-10-11 启动页搜索行：启用后顶部固定一行搜索框（替代「更多...」芯片），
    //   输入实时过滤工程芯片（名称/路径不区分大小写包含，口径同完整列表搜索框）。
    //   菜单内嵌面板保持「更多...」不变（两者共享本组件，开关分离形态）。
    void SetFilterBar(bool a_bOn);
    QLineEdit* FilterEdit() const { return m_pEditFilter; }   // 测试/诊断观察口

protected:
    bool eventFilter(QObject* a_pObj, QEvent* a_pEv) override;

private:
    void Relayout();          // 按内容部件实宽重排（滚动条出现会变窄；只排未隐藏芯片）
    void OnFilterChanged();   // 搜索文本变化 → 芯片显隐 + 重排 + 状态栏（2026-10-11）
    // 2026.10.12 键盘回车/空格打开芯片（与鼠标点击同语义：更多...→完整列表，
    //   其余→打开工程；事件已在过滤器消费，不会落到 QDialog 缺省按钮）。
    void OpenChip(CRecentChipButton* a_pChip);
    void OnChipRightClick(const QString& a_sPath, const QPoint& a_ptGlobal);  // 右键上下文菜单
    // —— 2026-10-09 边缘拖拽改尺寸（qApp 级接管版）——
    //   初版子控件级过滤器实测（Windows 真实平台 probe）拖拽链路本身可通，但面板
    //   右/下缘外还有菜单 chrome（实测 4px/3px），贴视觉菜单边缘按下的事件 receiver
    //   是 QMenu 本身而非任何子控件——子控件级过滤器永远看不到 → 用户「拖不动」。
    //   改 qApp 事件过滤器统一接管：可见全部事件（含发往 QMenu 的）、不依赖子控件
    //   路由连续性、覆盖 chrome 带；判定用全局坐标（不依赖 receiver 坐标映射）。
    int  EdgeAtGlobal(const QPoint& a_ptGlobal) const; // 全局坐标→边掩码（含菜单 chrome 扩展）
    bool DragScopeContains(QObject* a_pObj) const;     // receiver 是否在拖拽监听范围
    void UpdateHoverCursor(int a_iEdge);               // 边缘带 hover 光标（0=恢复）
    void ApplyUserSize(int a_iW, int a_iH);           // 钳位后落尺寸（含同步所在 QMenu 重排）
    void LoadUserSize();                              // 首次 Refresh 时读档（一次）
    void SaveUserSize();                              // 拖拽释放时落盘

    FOpenProject      m_fnOpen;
    FMore             m_fnMore;
    FCfgPath          m_fnCfgPath;                    // 2026-10-09 持久化路径提供器
    FRemoveProject    m_fnRemove;                     // 2026-10-09 右键删除回调
    QScrollArea*      m_pScroll  = nullptr;
    QWidget*          m_pHost    = nullptr;   // 滚动内容（chips 宿主，手动几何）
    QLabel*           m_pLblStat = nullptr;
    QStringList       m_aNames;               // 含首个「更多...」占位名
    QList<CRecentChipButton*> m_aChips;
    QString           m_sStatDefault;
    // —— 2026-10-09 拖拽改尺寸状态（qApp 级接管）——
    int   m_iUserW = 0;         // 用户尺寸（0=自动：宽 kPanelW / 高按行数封顶）
    int   m_iUserH = 0;
    bool  m_bUserLoaded = false;   // 档只读一次（失败/无档=自动尺寸）
    bool  m_bResizing = false;     // 拖拽中（qApp 过滤器接管，吃掉范围内鼠标事件）
    QPoint m_ptRsStart;            // 拖拽起点（全局）
    int   m_iRsStartW = 0;         // 拖拽起始尺寸
    int   m_iRsStartH = 0;
    int   m_iRsEdge = 0;           // 起始边掩码
    bool  m_bCursorSet = false;    // hover 边缘带时光标已 override（配对恢复）
    // —— 2026-10-09 右键删除 ——
    std::unique_ptr<QMenu> m_pCtxMenu;   // 右键菜单（成员持有：exec 期间测试可触发）
    // —— 2026-10-11 启动页搜索行 ——
    bool           m_bFilterBar = false; // 搜索行启用（Refresh 时决定是否建「更多...」）
    QLineEdit*     m_pEditFilter = nullptr;   // 顶部搜索框（默认隐藏）
    QStringList    m_aPaths;              // 与 m_aChips 对齐的工程路径（过滤匹配用）
};
