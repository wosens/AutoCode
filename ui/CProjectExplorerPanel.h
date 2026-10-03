#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QPersistentModelIndex>
#include <QVector>
#include <QTimer>
#include <QHash>

class QTreeView;
class QFileSystemModel;
class QMenu;
class QAction;
class QDropEvent;
class QToolButton;
class QAbstractItemModel;
class QDialog;   // 2026.10.02 属性兑底对话框（BuildPropertiesDialog 返回值）

// 工程资源管理器面板（FR-3）：以 QTreeView + 多根代理模型展示工程根目录树，
// 右键提供 9 项文件操作（通过资源管理器打开/剪切/复制/粘贴/删除/改名/复制路径/
// 复制相对路径/添加到聊天框）。
// 2026.09.21 多根节点（关联项目）：树顶层可为多个根目录（工程根 + 用户添加的关联
//   项目根），底部三按钮 [添加根节点][根节点上移][根节点下移]，根顺序持久化到
//   <工程>/.autodev/explorer.json；复制相对路径对根节点直接复制名称。
// 所有文件操作限定在根子树内（NFR-7 越界校验，多根时代入任一根）；删除/改名二次确认。
class CProjectExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit CProjectExplorerPanel(QWidget* a_pParent = nullptr);
    ~CProjectExplorerPanel() override;   // 释放打开方式 COM 处理器

    // 切换工程根（OpenProjectDir 触发）；空串=清空树。工程根入多根列表首项，
    // 同时载入 <工程>/.autodev/explorer.json 持久化的关联根（有序、过滤不存在项）。
    void SetRoot(const QString& a_sDir);

    // —— 2026.09.21 多根节点公开 API（按钮槽/右键菜单/测试共用） ——
    // 添加关联根目录（a_sDir 必须存在且不与现有根重复）；返回 false=拒绝（未开工程/
    //   路径无效/重复）。成功后新根行选中并展开，立即持久化。
    bool AddExtraRoot(const QString& a_sDir);
    // 移除关联根（仅允许移除非工程根的额外根）；同时持久化。
    bool RemoveExtraRoot(const QString& a_sDir);
    // 当前选中根节点上移（a_iDelta=-1）/下移（+1）；仅当选中树顶层根行且目标
    //   位置合法时生效，成功后保持该根选中并持久化。
    bool MoveSelectedRoot(int a_iDelta);
    int         RootCount() const;                 // 树顶层根数（含工程根）
    QString     RootPathAt(int a_iRow) const;      // 顶层第 a_iRow 个根的绝对路径
    QStringList RootPaths() const;                 // 有序全根（持久化内容同源）
    int         SelectedRootRow() const;           // 当前选中根行号（-1=未选根）
    bool        SelectRoot(int a_iRow);            // 选中某根行（滚动到可见；测试用）
    // 按绝对路径选中树内节点（顶层根或子节点；目录沿途 fetchMore，未加载完返回
    //   false——调用方可重试/事件循环后再试。测试/联动用）。
    bool        SelectPath(const QString& a_sAbsPath);
    QAbstractItemModel* ExplorerModel() const;     // 树模型（多根代理）观察口，测试用
    // 复制相对路径（公开供测试）：根节点→直接复制名称；普通节点→相对所在根的路径。
    void CopyRelativePath();

    // 2026.09.23 显示隐藏清单配置化：设置页/热重载下发（逗号分隔目录名；空=全可见；
    //   缺省用 HiddenDirs() 内置清单）。立即对已加载树重隐/重显（后续新行走钩子）。
    //   注意与备份排除/扫描排除无关（那两个管备份范围与指纹扫描，这个只管树显示）。
    void SetHiddenDirs(const QStringList& a_aNames);

    // 2026.09.23 手动刷新（右键「刷新」/测试用）：QFileSystemModel 的目录内容同步完全
    //   依赖其内部 watcher 的真实文件系统事件（事件→全量重列补齐/剔除），Windows 通知
    //   缓冲溢出/杀软延迟会丢事件致树停滞（Qt6 无公开强制重列 API）——对目标目录制造
    //   一次无害临时文件事件强制全量重列，并兜底重放展开态。选中目录→刷新该目录；
    //   选中文件→刷新其父目录；无选中→刷新全部根；已展开子目录一并重列（监视面）。
    void RefreshSelected();
    // 测试观察口：RefreshSelected 将刷新的目录清单（同口径推导，供单测锁定）。
    QStringList RefreshTargets() const;
    // 2026.09.26 v3.1.247 新建文件夹：在 a_sTargetDir 下创建（空=按选中推导：选中目录
    //   自身/选中文件父目录/无选中=工程根），默认名「新建文件夹」重名自动 " (n)"；
    //   返回新建绝对路径（失败/无根=空串）。公开供测试；菜单入口=StartNewFolder。
    QString CreateNewFolderAt(const QString& a_sTargetDir);
    // 2026.08.21 「打开方式」子菜单：枚举该文件类型在注册表（HKCR）注册的程序，
    //   子菜单逐项列出，点击按其 command 模板调用对应程序打开文件。
    //   2026.09.26 v3.1.244 改 public：回归测试直调检查菜单结构（.bat 精简口径）。
    void BuildOpenWithMenu(QMenu* a_pParentMenu, const QString& a_sFilePath);
    // 2026.10.02 属性：主路径 SHObjectProperties（Windows 资源管理器原生属性框，
    //   文件/目录同接口）；失败兑底自绘属性对话框（BuildPropertiesDialog，公开
    //   供测试断言字段填充；不 exec——调用方负责显示）。
    static QDialog* BuildPropertiesDialog(const QString& a_sPath, QWidget* a_pParent);

    // 2026.09.17 测试观察口：SHAssocEnumHandlers 枚举某扩展名的关联处理器数
    //   （iFilter=0 推荐 / 1 全部；出参返回首个 UIName）。返回 -1=COM 枚举失败。
    static int ProbeAssocHandlers(const QString& a_sExtWithDot, int a_iFilter,
                                  QString* a_pFirstName,
                                  int* a_pIconifiedCount = nullptr);

    // 2026.09.17 打开方式 COM 处理器前置声明（cpp 内定义，持 IAssocHandler；
    //   公开嵌套类型：文件级辅助函数需具名访问）。
    struct SOpenWithHandler;

signals:
    // 「添加到聊天框」（Q1 默认）：把所选路径投到 adhoc 助手输入框。
    // a_sKind: "path" | "content"；本轮默认注入路径。
    void sigAddToChat(const QString& a_sText, const QString& a_sKind);
    // 2026.08.21 打开文件请求（a_sAbsPath 文本文件；a_bSwitchTab=true 切到预览 Tab，
    //   false=只加载内容/更新标题不切页——单击预览用，用户停留在当前 Tab）。
    void sigFileOpen(const QString& a_sAbsPath, bool a_bSwitchTab);
    // 2026.09.21 根列表/顺序变化（增删/移动/磁盘上根失效自动剪除/改名同步）。
    //   外部变更（剪除）时面板内部完成持久化；测试可据此断言。
    void sigExplorerRootsChanged();
    // 2026-09-30 代码审查（ReviewSdk）：对选中的文件/文件夹发起静态代码审查，
    //   主窗口接 → 弹 CCodeReviewDialog（非模态，分钟级异步跑）。
    void sigCodeReview(const QString& a_sAbsPath);

private slots:
    void OnCustomContextMenu(const QPoint& a_stPos);   // 右键菜单（含「打开方式」子菜单）
    void OnDoubleClicked(const QModelIndex& a_stIdx);  // 文本文件→sigFileOpen(切Tab)；其他→系统默认程序打开
    void OnSingleClicked(const QModelIndex& a_stIdx);  // 2026.08.21 单击文件：与双击一致打开，但不切 Tab
    // 2026.09.21 多根节点三按钮。
    void OnAddRootClicked();     // 添加根节点：选目录对话框（缺省起点=当前工程根）
    void OnMoveRootUp();         // 根节点上移（仅顶层根行）
    void OnMoveRootDown();       // 根节点下移（仅顶层根行）
    void OnProxyRootsChanged();  // 代理侧根变化（磁盘剪除/改名同步）→持久化+按钮态

private:
    // 2026.08.18 文本文件判定：扩展名白名单（源码/文档/配置）命中即文本；
    //   未知扩展名再嗅探前 4KB 含 \0 判二进制。双击文本文件进内嵌 Tab 用。
    static bool IsTextFile(const QString& a_sAbsPath);

    // 8 项操作（均含根越界校验）
    void DoCut();
    void DoCopy();
    void DoPaste();
    void DoDelete();        // 二次确认
    void DoRename();        // 二次确认
    void StartNewFolder();  // 2026.09.26 新建文件夹：创建 + 选中进入行内改名
    void CopyPath();        // 绝对路径 → 剪贴板
    void AddToChat();       // emit sigAddToChat
    void OpenInExplorer();  // explorer.exe 打开：目录=打开该目录，文件=所在目录并选中
    void StartProperties(const QString& a_sPath);  // 2026.10.02 属性：shell 原生框，失败兑底自绘
    // 2026.08.28 非文本文件（exe 等）的系统打开：ShellExecuteW 显式传
    //   lpDirectory=文件所在目录（对齐资源管理器双击；QDesktopServices::openUrl
    //   目录参数 NULL 会继承 AutoCode cwd，相对路径读资源的 exe 启动即闪退）。
    void OpenWithShell(const QString& a_sAbsPath);
    // 2026.08.27 拖拽（Windows 资源管理器式）：拖出（CF_HDROP→系统资源管理器/
    //   聊天发送框）+ 拖入/树内拖动（移动/复制文件）。事件统一走 viewport 的
    //   eventFilter（拖拽事件目标是 viewport），不依赖 QAbstractItemView 内建 DnD。
    void StartTreeDrag();                 // 按下项+多选 → QDrag(urls=CF_HDROP)
    void HandleTreeDrop(QDropEvent* a_pEv); // 落点目录判定 + 移动/复制/同名编号
    // 目标同名时生成 "base (n).ext" 唯一名（n≥2 递增），全部撞名返回空串。
    static QString UniqueDstPath(const QString& a_sPath);
    // 2026.09.23 刷新的内功：在目录内创建+删除一个点前缀隐藏临时文件，各产生一次
    //   文件系统事件 → 模型内部 gatherer 全量重列该目录（补齐新增/剔除已删）。
    static void NudgeDirWatch(const QString& a_sDir);

protected:
    bool eventFilter(QObject* a_pObj, QEvent* a_pEv) override;
    // 2026.09.26 v3.1.244 .bat/.cmd 精简口径：用 CMD 运行（cmd /k 执行并保留窗口）。
    void RunBatViaCmd(const QString& a_sAbsPath);
    // 2026.09.26 v3.1.244 注册表直读兑底（自 BuildOpenWithMenu 抽出，.bat 精简口径共用）。
    void BuildOpenWithRegistryFallback(QMenu* a_pParentMenu, const QString& a_sExtKey);
    // 按 command 模板展开 %1 并 startDetached 启动注册程序打开文件。
    static void LaunchOpenWithCommand(const QString& a_sCmdTemplate,
                                      const QString& a_sFilePath);
    // 2026.09.17 打开方式 COM 处理器生命周期（SHAssocEnumHandlers 与资源管理器同源）。
    void ClearOpenWithHandlers();
    void InvokeAssocHandler(int a_iIndex, const QString& a_sFilePath);
    // 2026.09.26 「用 CMD 运行」菜单项哨兵（data()；正常 COM 条目 data=非负索引）。
    static constexpr int s_iOpenWithCmdRun = -1000;

    bool    IsUnderRoot(const QString& a_sAbsPath) const;   // NFR-7 越界判定（任一根子树内，含根本身）
    QString ContainingRoot(const QString& a_sAbsPath) const; // 2026.09.21 最深包含根（相对路径基准）
    QString SelectedAbsPath() const;                        // 当前选中节点绝对路径（空=未选/无效）
    QStringList SelectedAbsPaths() const;                   // 2026.09.19 多选取（剪切/复制/拖拽同口径，限根内）
    // 2026.09.17 打开方式菜单的 COM 处理器持有（菜单项 data=索引；重建/析构时释放）。
    QVector<SOpenWithHandler*> m_aOpenWithHandlers;
    // 目录级隐藏：[a_iFirst, a_iLast] 行内命中 HiddenDirs 清单的目录整棵隐藏
    // （隐藏行不可展开，子树随之不可见）。顶层根行不走此过滤（父无效早退）。
    void HideGeneratedDirs(const QModelIndex& a_stParent, int a_iFirst, int a_iLast);
    // 2026.09.23 对已加载全树重隐/重显（SetHiddenDirs 立即生效用；仅遍历已取节点）。
    void ReapplyHiddenDirs();

    QTreeView*           m_pTree   = nullptr;
    QFileSystemModel*    m_pModel  = nullptr;   // 底层文件系统模型（多根共享一个）
    class CExplorerProxyModel* m_pProxy = nullptr;  // 2026.09.21 多根代理（顶层=根节点集合）
    QString              m_sRootDir;          // 当前工程根（越界判定基准/添加根对话框缺省路径）；空=未设
    // 2026.09.23 显示隐藏清单（实例可配置；缺省=HiddenDirs() 内置；空=全可见）。
    QStringList          m_aHiddenDirs = HiddenDirs();
    // 粘贴板（剪切/复制目标）：与 Windows 剪贴板联动（CF_HDROP+Preferred DropEffect，
    //   资源管理器可互粘）；内部仅存快照供菜单态/系统剪贴板不可用时的兑底。
    QStringList       m_aClipFiles;
    bool              m_bClipCut = false;
    // 2026.09.19 系统剪贴板文件读写（CF_HDROP 双向 + 剪切/复制效果标记）。
    //   public static：纯 Win32 封装不触成员，供测试做跨进程（PowerShell/资源管理器
    //   同一套剪贴板）互操作性验证。
public:
    static bool WriteFileClipboard(const QStringList& a_aPaths, bool a_bCut);
    static QStringList ReadFileClipboard(bool* a_pbCut = nullptr);
    // 单项转入目标目录（剪切/粘贴与拖拽共用语义：同名 " (n)" 编号、目录禁入自身子树、
    //   跨盘移动退复制+删源）。返回 0=成功 1=忽略（原地/缺失） 2=失败。
    enum ETransfer { XferOk = 0, XferSkip = 1, XferFail = 2 };
    static ETransfer TransferItem(const QString& a_sSrc, const QString& a_sDstDir, bool a_bMove);

private:
    // 拖出候选态：左键按下未超阈值时记录起点与按下项（超距才升级为 QDrag）。
    QPoint                 m_ptDragStart;
    bool                   m_bMaybeDrag = false;
    QPersistentModelIndex  m_stDragIdx;

    // 2026.09.21 多根节点：底部三按钮（添加/上移/下移，仅图标 + tooltip）。
    QToolButton* m_pBtnAddRoot  = nullptr;
    QToolButton* m_pBtnRootUp   = nullptr;
    QToolButton* m_pBtnRootDown = nullptr;
    void UpdateRootButtons();                     // 按选中态刷新三按钮可用性
    // 持久化：<工程>/.autodev/explorer.json {version:1, roots:[有序全根]}。
    //   工程级语义（关联根随工程走）；仅存存在的目录，加载时过滤。
    QString ExplorerStatePath() const;
    void    SaveExplorerState();
    QStringList LoadExplorerState() const;

    // —— 2026.09.22 展开态自愈（用户反馈：文件夹总会莫名其妙的自动全部折叠）——
    //   会话内记住已展开目录的绝对路径；任何结构事件（源延迟重排/目录删建/重命名）
    //   后去抖重放，行一回来就重新展开。防哪些源头杀掉展开态都自愈。
    void RememberExpand(const QModelIndex& a_stIdx, bool a_bExpanded);
    void ReapplyExpandedPaths();
    QTimer  m_tExpandSync;               // 结构事件去抖重放（singleShot 200ms）
    QHash<QString, QString> m_hExpandPaths;   // 小写路径键 → 原路径（上限见 Reapply）

    // 生成目录过滤清单（Q1 默认，后续可由 FR-8 设置调）。
    static const QStringList& HiddenDirs();
};
