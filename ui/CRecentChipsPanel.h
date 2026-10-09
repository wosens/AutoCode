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

class QLabel;
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

    CRecentChipsPanel(FOpenProject a_fnOpen, FMore a_fnMore,
                      QWidget* a_pParent = nullptr,
                      const FCfgPath& a_fnCfgPath = FCfgPath());

    // 刷新数据（菜单 aboutToShow 时调用，MRU 顺序；计数缺失按 1）。
    void Refresh(const QStringList& a_aPaths, const QList<int>& a_aOpens);

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
    // 2026-10-09 用户拖拽尺寸观察口（0=未自定义，自动尺寸）；测试断言用。
    QSize UserSize() const { return QSize(m_iUserW, m_iUserH); }

protected:
    bool eventFilter(QObject* a_pObj, QEvent* a_pEv) override;

private:
    void Relayout();          // 按内容部件实宽重排（滚动条出现会变窄）
    // —— 2026-10-09 边缘拖拽改尺寸 ——
    int  EdgeAt(const QPoint& a_stPanelPos) const;   // 面板局部坐标→边掩码（1=右 2=下 4=右下角）
    void ApplyUserSize(int a_iW, int a_iH);           // 钳位后落尺寸（含同步所在 QMenu 重排）
    void LoadUserSize();                              // 首次 Refresh 时读档（一次）
    void SaveUserSize();                              // 拖拽释放时落盘

    FOpenProject      m_fnOpen;
    FMore             m_fnMore;
    FCfgPath          m_fnCfgPath;                    // 2026-10-09 持久化路径提供器
    QScrollArea*      m_pScroll  = nullptr;
    QWidget*          m_pHost    = nullptr;   // 滚动内容（chips 宿主，手动几何）
    QLabel*           m_pLblStat = nullptr;
    QStringList       m_aNames;               // 含首个「更多...」占位名
    QList<CRecentChipButton*> m_aChips;
    QString           m_sStatDefault;
    // —— 2026-10-09 拖拽改尺寸状态 ——
    int   m_iUserW = 0;         // 用户尺寸（0=自动：宽 kPanelW / 高按行数封顶）
    int   m_iUserH = 0;
    bool  m_bUserLoaded = false;   // 档只读一次（失败/无档=自动尺寸）
    bool  m_bResizing = false;     // 拖拽中（吃掉 Press/Move/Release，防菜单误触发）
    QPoint m_ptRsStart;            // 拖拽起点（全局）
    int   m_iRsStartW = 0;         // 拖拽起始尺寸
    int   m_iRsStartH = 0;
    int   m_iRsEdge = 0;           // 起始边掩码
    QPointer<QWidget> m_pRsGrabber;   // 拖拽中抓取鼠标的子控件（释放时归还）
};
