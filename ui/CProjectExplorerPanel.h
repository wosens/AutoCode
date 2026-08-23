#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QPersistentModelIndex>

class QTreeView;
class QFileSystemModel;
class QMenu;
class QAction;

// 工程资源管理器面板（FR-3）：以 QTreeView + QFileSystemModel 展示当前工程根目录树
// （工程目录本身为顶层可见根节点），右键提供 9 项文件操作（通过资源管理器打开/剪切/复制/
// 粘贴/删除/改名/复制路径/复制相对路径/添加到聊天框）。
// 所有文件操作限定在工程根子树内（NFR-7 越界校验）；删除/改名二次确认。
class CProjectExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit CProjectExplorerPanel(QWidget* a_pParent = nullptr);

    // 切换工程根（OpenProjectDir 触发）；空串=清空树。
    // 工程目录本身作为树的可见根节点（顶层唯一一项）并自动展开。
    void SetRoot(const QString& a_sDir);

signals:
    // 「添加到聊天框」（Q1 默认）：把所选路径投到 adhoc 助手输入框。
    // a_sKind: "path" | "content"；本轮默认注入路径。
    void sigAddToChat(const QString& a_sText, const QString& a_sKind);
    // 2026.08.21 打开文件请求（a_sAbsPath 文本文件；a_bSwitchTab=true 切到预览 Tab，
    //   false=只加载内容/更新标题不切页——单击预览用，用户停留在当前 Tab）。
    void sigFileOpen(const QString& a_sAbsPath, bool a_bSwitchTab);

private slots:
    void OnCustomContextMenu(const QPoint& a_stPos);   // 右键菜单（含「打开方式」子菜单）
    void OnDoubleClicked(const QModelIndex& a_stIdx);  // 文本文件→sigFileOpen(切Tab)；其他→系统默认程序打开
    void OnSingleClicked(const QModelIndex& a_stIdx);  // 2026.08.21 单击文件：与双击一致打开，但不切 Tab

private:
    // 2026.08.18 文本文件判定：扩展名白名单（源码/文档/配置）命中即文本；
    //   未知扩展名再嗅探前 4KB 含 \0 判二进制。双击文本文件进内嵌 Tab 用。
    static bool IsTextFile(const QString& a_sAbsPath);

    // 8 项操作（均含工程根越界校验）
    void DoCut();
    void DoCopy();
    void DoPaste();
    void DoDelete();        // 二次确认
    void DoRename();        // 二次确认
    void CopyPath();        // 绝对路径 → 剪贴板
    void CopyRelativePath();// 相对工程根 → 剪贴板
    void AddToChat();       // emit sigAddToChat
    void OpenInExplorer();  // explorer.exe 打开：目录=打开该目录，文件=所在目录并选中
    // 2026.08.21 「打开方式」子菜单：枚举该文件类型在注册表（HKCR）注册的程序，
    //   子菜单逐项列出，点击按其 command 模板调用对应程序打开文件。
    void BuildOpenWithMenu(QMenu* a_pParentMenu, const QString& a_sFilePath);
    // 按 command 模板展开 %1 并 startDetached 启动注册程序打开文件。
    static void LaunchOpenWithCommand(const QString& a_sCmdTemplate,
                                      const QString& a_sFilePath);

    bool    IsUnderRoot(const QString& a_sAbsPath) const;   // NFR-7 越界判定
    QString SelectedAbsPath() const;                        // 当前选中节点绝对路径（空=未选/无效）
    // 把父目录下 [a_iFirst, a_iLast] 行内非工程根的行隐藏（工程根行恢复可见），
    // 使树顶层只剩工程目录一项（配合视图根=父目录的方案，见 SetRoot）。
    void HideSiblingRows(const QModelIndex& a_stParent, int a_iFirst, int a_iLast);
    // 目录级隐藏：[a_iFirst, a_iLast] 行内命中 HiddenDirs 清单的目录整棵隐藏
    // （隐藏行不可展开，子树随之不可见）。工程根本身永不隐藏（即使名字撞清单）。
    void HideGeneratedDirs(const QModelIndex& a_stParent, int a_iFirst, int a_iLast);

    QTreeView*           m_pTree   = nullptr;
    QFileSystemModel*    m_pModel  = nullptr;
    QString              m_sRootDir;          // 当前工程根（越界判定基准）；空=未设
    QPersistentModelIndex m_stRootIdx;        // 工程根节点（树顶层唯一可见项）
    // 进程内粘贴板（剪切/复制目标）：存绝对路径 + 是否剪切态
    QString           m_sClipboardPath;
    bool              m_bCutMode = false;

    // 生成目录过滤清单（Q1 默认，后续可由 FR-8 设置调）。
    static const QStringList& HiddenDirs();
};
