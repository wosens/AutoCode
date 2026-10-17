#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QListWidgetItem;
class QDialogButtonBox;
class QLineEdit;
class QVBoxLayout;   // 2026.10.09 SetupLauncherUi 参数（启动页布局增补）

// 最近打开工程历史对话框：全量列出历史（MRU），双击 / 确认返回选中路径，
// 取消 / 关闭返回空。仅持有 QStringList 做展示，不直接访问 CProjectManager。
// 2026.08.20：列表 11 号字体；支持多选（ExtendedSelection）；右键菜单可删除选中
// 条目（仅从列表移除并经 RemovedPaths() 上报，由调用方落 MRU 持久化）。
// 2026.10.09 双模式：kNormal=原有「最近打开」弹窗（搜索+列表，菜栏入入口）；
//   kLauncher=启动界面（主窗口显示前）——顶部品牌 Title 横幅（浅色高科技背景 +
//   深色 AutoCode 字样）、主体 CRecentChipsPanel 按钮面板（点击即开、悬停路径、
//   右键删除、更多...弹完整列表）、「跳过/退出」底钮；窗口与面板尺寸双向同步、
//   位置独立持久化 [Layout] RecentLauncherPos，按钮区域尺寸与菜单内弹出面板
//   共用 [Layout] RecentChipsSize（拖边改尺寸互通）。
class CRecentProjectsDialog : public QDialog {
    Q_OBJECT

public:
    // 2026.10.09 窗口模式（见类注释）：kNormal 原有行为完全不变；kLauncher 启动页。
    enum class EMode { kNormal, kLauncher };

    // 2026.09.20 a_aOpens：与 a_aRecent 同序的打开次数（缺省空 = 旧调用兼容，显示「1 次」）。
    // 2026.09.20 a_sCfgPath：几何持久化（[Layout] RecentWin=x,y,w,h；空 = 不持久化，
    //   同 CRollbackDialog/a_sCfgPath 口径——测试传空不写盘）；hideEvent 保存。
    explicit CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent = nullptr);
    CRecentProjectsDialog(const QStringList& a_aRecent, const QList<int>& a_aOpens,
                          QWidget* a_pParent = nullptr);
    CRecentProjectsDialog(const QStringList& a_aRecent, const QList<int>& a_aOpens,
                          const QString& a_sCfgPath, QWidget* a_pParent = nullptr);
    // 2026.10.09 启动页构造：a_eMode=kLauncher 时启用启动界面形态（见类注释）。
    CRecentProjectsDialog(const QStringList& a_aRecent, const QList<int>& a_aOpens,
                          const QString& a_sCfgPath, EMode a_eMode, QWidget* a_pParent = nullptr);

    // exec() 返回 Accepted 时取选中路径；Rejected / 空选择返回空串。
    // 多选时返回当前项（current item）路径；无当前项取第一个选中项。
    // kLauncher：点按钮面板上的工程即选中并 accept；「更多...」弹完整列表选中
    //   亦同（弹窗内删除的条目同样经 RemovedPaths 上报）。
    QString SelectedPath() const;

    // 本次会话中被右键删除的工程路径（顺序无关）；调用方据此从 MRU 移除并落盘。
    QStringList RemovedPaths() const;

    // 2026.10.09 kLauncher 专用：用户点了「退出」按钮（区别于「跳过」——跳过仍进主
    //   窗口，退出直接结束进程；两者 exec 均为 Rejected，靠本标志区分）。
    bool ExitRequested() const { return m_bExitRequested; }

    // 2026.10.14 kLauncher 专用：用户点了「跳过」按钮——不选工程直接进主窗口，
    //   但调用方（main.cpp）会据此改开 exe 同级 AutoCode 工作目录（免选工程即可
    //   建任务/对话助手）；Esc/X 关闭不置位，维持原「进空主界面」语义。
    bool SkipRequested() const { return m_bSkipRequested; }

private slots:
    void OnItemDoubleClicked();              // 双击 = 确认
    void OnContextMenu(const QPoint& a_pos); // 右键菜单：删除选中
    void OnDeleteSelected();                 // 执行删除（列表内移除 + 记录）
    void OnSearchChanged(const QString& a_sText); // 搜索框内容变化 → 实时重筛列表

protected:
    // 2026.09.20 几何持久化：隐藏（含 accept/reject/done 关闭）时落 [Layout] RecentWin。
    void hideEvent(QHideEvent* a_pEv) override;
    // 2026.10.11 kLauncher：点右上角关闭（X）= 直接退出程序（同「退出」按钮，
    //   置 ExitRequested 后走 reject；Esc/「跳过」仍进主窗口，两者区分开）。
    void closeEvent(QCloseEvent* a_pEv) override;
    // 2026.10.11 kLauncher：窗口拖边 → 同步按钮面板固定尺寸（面板随窗口伸展）。
    void resizeEvent(QResizeEvent* a_pEv) override;
    // 2026.10.11 kLauncher：监控面板尺寸外部变化（拖面板边/Refresh）→ 窗口包紧。
    bool eventFilter(QObject* a_pObj, QEvent* a_pEv) override;
    // 2026.10.12 启动页：焦点不在工程按钮区（底钮/无焦点）时按方向键 → 焦点
    //   跳入芯片区（搜索框内 ↓ 由面板内部处理，不经此处）；其余键交 QDialog
    //   （Enter=跳过缺省钮、Esc=关闭）。
    void keyPressEvent(QKeyEvent* a_pEv) override;

private:
    void ApplyPath(QListWidgetItem* a_pItem); // 记录确认路径（空安全）
    void RebuildList(const QString& a_sFilter); // 依据过滤词重建列表（排除已删条目）
    void RestoreGeometryFromConfig();   // 2026.09.20 [Layout] RecentWin 恢复（限幅屏内）
    void SaveGeometryToConfig();        // 2026.09.20 落盘 + profile 写缓存冲刷
    // 2026.10.11 kLauncher：启动页专用搭建（Title 横幅 + CRecentChipsPanel 按钮
    //   面板主体 + 跳过/退出底钮；不再有搜索框/列表/OK 按钮）。
    void SetupLauncherUi(QVBoxLayout* a_pMainLay);
    // 2026.10.11 kLauncher：启动页几何——位置取 [Layout] RecentLauncherPos（无档
    //   居中主屏）；窗口尺寸由面板初始尺寸推导（面板尺寸档即 RecentChipsSize）。
    void RestoreLauncherGeometry();
    // 2026.10.11 kLauncher：位置落 RecentLauncherPos；面板当前尺寸回写
    //   [Layout] RecentChipsSize（与菜单内弹出面板同步；面板拖边路径由面板自身
    //   SaveUserSize 落盘，此处兑底窗口拖边路径）。
    void SaveLauncherGeometry();
    // 2026.10.11 kLauncher：窗口尺寸 → 面板固定尺寸（客户区减 Title/底钮固定高）。
    void SyncPanelToWindow();
    // 2026.10.11 kLauncher：面板尺寸 → 窗口包紧（面板拖边/初建时反向同步）。
    void SyncWindowToPanel();
    // 2026.10.11 kLauncher：从数据源移除已删工程并刷新面板（右键删除/更多...内
    //   删除共用）；m_aRemoved 同步记录。
    void RemoveAndRefresh(const QString& a_sPath);

    QListWidget*      m_pList = nullptr;
    QLineEdit*        m_pSearch = nullptr;   // 顶部搜索框（实时过滤；kLauncher 不创建）
    QStringList       m_aAll;                // 全量历史快照（构造入参，过滤/重建的数据源）
    QList<int>        m_aOpens;              // 2026.09.20 打开次数（与 m_aAll 同序；旧档=1）
    QString           m_sCfgPath;            // 2026.09.20 几何持久化路径（空=禁用）
    QDialogButtonBox* m_pBtns = nullptr;
    QString           m_sSelected;
    QStringList       m_aRemoved;             // 右键删除的路径集合
    EMode             m_eMode = EMode::kNormal; // 2026.10.09 窗口模式
    bool              m_bExitRequested = false; // 2026.10.09 「退出」按钮已点
    bool              m_bSkipRequested = false; // 2026.10.14 「跳过」按钮已点（main 据此开工作目录）
    class CRecentChipsPanel* m_pChips = nullptr; // 2026.10.11 启动页按钮面板主体
    QWidget*          m_pBtnRow = nullptr;   // 2026.10.11 启动页底钮行（高度基准）
    bool              m_bSyncingGeom = false; // 2026.10.11 窗口↔面板同步重入护栏
};
