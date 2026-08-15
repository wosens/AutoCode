#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>

class QTreeView;
class QFileSystemModel;
class QMenu;
class QAction;

// 工程资源管理器面板（FR-3）：以 QTreeView + QFileSystemModel 展示当前工程根目录树，
// 提供 8 项右键文件操作（剪切/复制/粘贴/删除/改名/复制路径/复制相对路径/添加到聊天框）。
// 所有文件操作限定在工程根子树内（NFR-7 越界校验）；删除/改名二次确认。
class CProjectExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit CProjectExplorerPanel(QWidget* a_pParent = nullptr);

    // 切换工程根（OpenProjectDir 触发）；空串=清空树。
    void SetRoot(const QString& a_sDir);

signals:
    // 「添加到聊天框」（Q1 默认）：把所选路径投到 adhoc 助手输入框。
    // a_sKind: "path" | "content"；本轮默认注入路径。
    void sigAddToChat(const QString& a_sText, const QString& a_sKind);

private slots:
    void OnCustomContextMenu(const QPoint& a_stPos);   // 8 项菜单
    void OnDoubleClicked(const QModelIndex& a_stIdx);  // Q1 默认：调系统默认程序打开

private:
    // 8 项操作（均含工程根越界校验）
    void DoCut();
    void DoCopy();
    void DoPaste();
    void DoDelete();        // 二次确认
    void DoRename();        // 二次确认
    void CopyPath();        // 绝对路径 → 剪贴板
    void CopyRelativePath();// 相对工程根 → 剪贴板
    void AddToChat();       // emit sigAddToChat

    bool    IsUnderRoot(const QString& a_sAbsPath) const;   // NFR-7 越界判定
    QString SelectedAbsPath() const;                        // 当前选中节点绝对路径（空=未选/无效）

    QTreeView*        m_pTree   = nullptr;
    QFileSystemModel* m_pModel  = nullptr;
    QString           m_sRootDir;          // 当前工程根（越界判定基准）；空=未设
    // 进程内粘贴板（剪切/复制目标）：存绝对路径 + 是否剪切态
    QString           m_sClipboardPath;
    bool              m_bCutMode = false;

    // 生成目录过滤清单（Q1 默认，后续可由 FR-8 设置调）。
    static const QStringList& HiddenDirs();
};
