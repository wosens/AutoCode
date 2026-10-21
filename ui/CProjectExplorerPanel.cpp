#include "CProjectExplorerPanel.h"
#include "ui/CMenuIcon.h"

#include <QTreeView>
#include <QFileSystemModel>
#include <QAbstractProxyModel>
#include <QMenu>
#include <QAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include "core/CFrwDevice.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QThread>   // 2026.09.19 剪贴板占用冲突重试间隔
#include <QUrl>
#include <QModelIndex>
#include <QFont>
#include <QVariant>
#include <QProcess>
#include <QSettings>
#include <QHash>
#include <QFileIconProvider>
#include <QSet>
#include <QRegularExpression>
#include <functional>   // 2026.09.23 ReapplyHiddenDirs 递归遍历
#include <QProcessEnvironment>
#include <QApplication>
#include <QMouseEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QList>
#include <QUrl>
#include <QToolButton>
#include <QFileDialog>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>          // 2026.10.02 属性兑底对话框
#include <QFormLayout>      // 同上：属性字段表
#include <QLabel>           // 同上：字段值（路径可选中复制）
#include <QCheckBox>        // 同上：只读/隐藏写回
#include <QPushButton>      // 同上：确定按钮
#include <QDirIterator>      // 同上：目录递归统计

// 2026.08.28 非文本文件的系统打开改走 ShellExecuteW（显式传工作目录，对齐
//   Windows 资源管理器双击口径）；同项目其他文件，NOMINMAX 防 min/max 宏污染。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteExW（本翻译单元 windows.h 未带入，显式包含）
#include "sandbox/CSandboxManager.h"   // 2026.09.26 用户启动豁免登记（点开的程序不进沙箱不录像）
// 2026.09.17 打开方式菜单 shell 同源枚举：SHAssocEnumHandlers/IEnumAssocHandlers/
//   IAssocHandler（GetUIName 本地化名 + Invoke 交 shell 处理 %1 展开/DDE 动词如
//   devenv /dde）；ILCreateFromPath/SHCreateDataObject 用于构造 Invoke 入参。
#include <shobjidl_core.h>
#include <shlobj_core.h>

// 2026.09.17 打开方式启动链前向声明（定义见文件后段；InvokeAssocHandler 在前段先用到）。
static QString ExpandEnvVars(const QString& a_sCmd);
static QString MenuAssocName(IAssocHandler* a_pHandler);

const QStringList& CProjectExplorerPanel::HiddenDirs() {
    // Q1 默认：隐藏常见生成目录（后续可由 FR-8 设置调）。
    // 生效方式：rowsInserted 钩子 → HideGeneratedDirs 逐行隐藏（2026-08-17 接通，此前未挂接）。
    // 2026.08.26：.autodev 移出隐藏清单（用户要求可见：任务目录/record 等需要浏览）。
    // 2026.08.27：Execute 移出隐藏清单（用户要求可见：部署产物/config.ini/prompts/
    //   help 等常需浏览拷贝；它与备份排除 [Backup] ExcludeDirs 是两回事，互不影响）。
    static const QStringList s_aHidden {
        QStringLiteral("build"),
        QStringLiteral(".git"),
        QStringLiteral(".vs"),
        QStringLiteral("node_modules")
    };
    return s_aHidden;
}

// 本地递归目录拷贝（QDir::copyRecursively 在部分 Qt6 构建中可见性不稳，自实现以保稳）。
// 2026.10.16 a_bOverwrite=true（粘贴「覆盖」档）：目标同名文件先删再拷——目录合并语义
//   （目标独有文件保留、同名文件被源替换），不整树推倒重来。
static bool CopyDirRecursively(const QString& a_sSrc, const QString& a_sDst,
                               bool a_bOverwrite = false) {
    const QDir stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return false;
    }
    if (!QDir().mkpath(a_sDst)) {
        return false;
    }
    // 子目录递归
    const QStringList aDirs = stSrc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sSub : aDirs) {
        if (!CopyDirRecursively(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub),
                                a_bOverwrite)) {
            return false;
        }
    }
    // 文件拷贝
    const QStringList aFiles = stSrc.entryList(QDir::Files);
    for (const QString& sFile : aFiles) {
        const QString sOne = QDir(a_sDst).filePath(sFile);
        if (a_bOverwrite && QFile::exists(sOne) && !QFile::remove(sOne)) {
            return false;
        }
        if (!QFile::copy(stSrc.filePath(sFile), sOne)) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// 2026.09.21 多根代理模型 CExplorerProxyModel：树顶层 = 根目录集合（工程根 +
//   关联项目根），每个根的子树 1:1 映射到底层 QFileSystemModel 的对应子树。
//   · mapToSource(顶层行) = 该根在源模型中的索引；子级行号/列号与源一致——源
//     内部排序（QFileSystemModel::sort 天然目录优先）直接反映到各根子树；顶层
//     根顺序恒为面板给的列表序（view 排序只调 model->sort，我们转发源后随
//     layoutChanged 整体 reset，不会重排顶层）。
//   · 源信号（rowsInserted/Removed/dataChanged/layout*/reset）映射后转发；根
//     子树之外（驱动器层/根的兄弟目录）的信号全部吞掉——旧版 HideSiblingRows
//     「隐藏兄弟行」机制由映射天然实现，且不再受根目录必须同父的限制（跨盘/
//     任意深度的关联项目均可作根）。layout* 为规范转发（持久索引重映射，保住
//     展开/选中；2026.09.22——旧“翻译成整体 reset”会在源每次延迟重排时折叠全树）。
//   · 根目录被外部删除/改名：源索引失效/路径漂移 → PruneDeadRoots/ResyncRootPaths
//     自动剪除/同步并 emit sigRootsChanged（面板收到后持久化）。
// ===========================================================================
class CExplorerProxyModel : public QAbstractProxyModel {
    Q_OBJECT
public:
    explicit CExplorerProxyModel(QFileSystemModel* a_pFs, QObject* a_pParent)
        : QAbstractProxyModel(a_pParent), m_pFs(a_pFs) {
        setSourceModel(a_pFs);
        // —— 源信号映射转发（见类注释）——
        connect(a_pFs, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& stA, const QModelIndex& stB,
                       const QVector<int>& aRoles) {
            const QModelIndex stMA = mapFromSource(stA);
            const QModelIndex stMB = mapFromSource(stB);
            if (stMA.isValid() && stMB.isValid()) {
                emit dataChanged(stMA, stMB, aRoles);
            }
        });
        connect(a_pFs, &QAbstractItemModel::rowsAboutToBeInserted, this,
                [this](const QModelIndex& stParent, int iFirst, int iLast) {
            const QModelIndex stMapped = mapFromSource(stParent);
            m_bInsArmed = stMapped.isValid();
            if (m_bInsArmed) {
                beginInsertRows(stMapped, iFirst, iLast);
            }
        });
        connect(a_pFs, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex&, int, int) {
            if (m_bInsArmed) {
                m_bInsArmed = false;
                endInsertRows();
            }
            PruneDeadRoots();   // 根目录被删：源根索引随之失效 → 剪除该顶层行
        });
        connect(a_pFs, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& stParent, int iFirst, int iLast) {
            const QModelIndex stMapped = mapFromSource(stParent);
            m_bRmArmed = stMapped.isValid();
            if (m_bRmArmed) {
                beginRemoveRows(stMapped, iFirst, iLast);
            }
        });
        connect(a_pFs, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex&, int, int) {
            if (m_bRmArmed) {
                m_bRmArmed = false;
                endRemoveRows();
            }
            PruneDeadRoots();
        });
        // 2026.09.22 v3.0.209 修复「点击子文件夹→全树折叠」：源 layoutChanged 改为
        //   规范转发，不再翻译成整体 reset！根因（Qt 6.8 qfilesystemmodel.cpp 实证）：
        //   目录首次列出/监视目录新增文件时 directoryChanged 全部子节点计入 newFiles
        //   → forceSort=true+delayedSort()→0ms 后 sort()（forceSort 绕过同序早退）→
        //   layoutAboutToBeChanged/layoutChanged——旧代码 reset 转发会让 QTreeView 清空
        //   expandedIndexes（展开态全丢）：点击展开未列出过的子目录，列表毫秒级返回即
        //   触发重排 → 被点目录弹回收起 + 已展开层级全部折叠（用户实害）。
        //   规范转发：about 时捕获代理持久索引并映射到源（QPersistentModelIndex 随源
        //   changePersistentIndexList 自动更新到新行）；changed 时回映射并把代理持久
        //   索引（视图展开集/选中/当前项都是持久索引）改指到新行，再 emit
        //   layoutChanged——视图仅重排不丢展开。顶层根行不参与源排序（顺序由本类
        //   列表序决定），映射恒为自身。回归 test_tasklistpanel
        //   explorerExpandSurvivesFirstListing（旧代码必 FAIL）。
        connect(a_pFs, &QAbstractItemModel::layoutAboutToBeChanged, this, [this]() {
            m_aLayoutProxy = persistentIndexList();
            m_aLayoutSrc.clear();
            for (int i = 0; i < m_aLayoutProxy.size(); ++i) {
                m_aLayoutSrc.append(
                    QPersistentModelIndex(mapToSource(m_aLayoutProxy.at(i))));
            }
            emit layoutAboutToBeChanged();
        });
        connect(a_pFs, &QAbstractItemModel::layoutChanged, this, [this]() {
            const int n = qMin(m_aLayoutProxy.size(), m_aLayoutSrc.size());
            for (int i = 0; i < n; ++i) {
                const QModelIndex stNew = mapFromSource(m_aLayoutSrc.at(i));
                if (stNew.isValid() && stNew != m_aLayoutProxy.at(i)) {
                    changePersistentIndex(m_aLayoutProxy.at(i), stNew);
                }
            }
            m_aLayoutProxy.clear();
            m_aLayoutSrc.clear();
            ResyncRootPaths();
            PruneDeadRoots();
            emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        });
        connect(a_pFs, &QAbstractItemModel::modelAboutToBeReset,
                this, [this]() { beginResetModel(); });
        connect(a_pFs, &QAbstractItemModel::modelReset, this, [this]() {
            ReresolveRoots();   // 源 reset 后持久索引全部失效：按存储路径重解析
            endResetModel();
        });
        // 根目录被改名：源索引跟随但存储路径串漂移 → 从源索引回读同步。
        connect(a_pFs, &QFileSystemModel::fileRenamed, this, [this]() {
            ResyncRootPaths();
        });
    }

    // —— 根列表驱动（面板调用；顺序即顶层显示顺序） ——
    void ResetRoots(const QStringList& a_aRoots, const QString& a_sPrimary) {
        beginResetModel();
        m_aRoots.clear();
        m_aRootSrc.clear();
        m_aSlots.clear();
        m_aSlotOfNode.clear();
        if (m_pFs != nullptr) {
            if (!a_sPrimary.isEmpty()) {
                m_pFs->setRootPath(a_sPrimary);   // 工程根：目录监视+预取（保持旧行为）
            }
            for (const QString& s : a_aRoots) {
                const QModelIndex stIdx = m_pFs->index(s);
                if (!stIdx.isValid()) {
                    continue;   // 目录不存在/解析失败：跳过（面板收敛后回写持久化）
                }
                m_aRoots << s;
                m_aRootSrc << QPersistentModelIndex(stIdx);
            }
        }
        endResetModel();
    }
    bool AppendRoot(const QString& a_sPath) {
        if (m_pFs == nullptr || a_sPath.isEmpty()) {
            return false;
        }
        const QModelIndex stIdx = m_pFs->index(a_sPath);
        if (!stIdx.isValid()) {
            return false;
        }
        const int n = m_aRoots.size();
        beginInsertRows(QModelIndex(), n, n);
        m_aRoots << a_sPath;
        m_aRootSrc << QPersistentModelIndex(stIdx);
        endInsertRows();
        return true;
    }
    void RemoveRootAt(int a_iRow) {
        if (a_iRow < 0 || a_iRow >= m_aRoots.size()) {
            return;
        }
        beginRemoveRows(QModelIndex(), a_iRow, a_iRow);
        m_aRoots.removeAt(a_iRow);
        m_aRootSrc.remove(a_iRow);
        endRemoveRows();
    }
    bool MoveRoot(int a_iFrom, int a_iTo) {
        if (a_iFrom < 0 || a_iFrom >= m_aRoots.size()
            || a_iTo < 0 || a_iTo >= m_aRoots.size() || a_iFrom == a_iTo) {
            return false;
        }
        // Qt 目的行语义：移到 a_iTo 之后传 a_iTo+1（下行方向）。
        beginMoveRows(QModelIndex(), a_iFrom, a_iFrom, QModelIndex(),
                      a_iTo > a_iFrom ? a_iTo + 1 : a_iTo);
        m_aRoots.move(a_iFrom, a_iTo);
        m_aRootSrc.move(a_iFrom, a_iTo);
        endMoveRows();
        return true;
    }
    int RootCount() const { return m_aRoots.size(); }
    QString RootPathAt(int a_iRow) const {
        return (a_iRow >= 0 && a_iRow < m_aRoots.size()) ? m_aRoots.at(a_iRow)
                                                         : QString();
    }
    QStringList RootPaths() const { return m_aRoots; }

    // —— 面板用的文件语义辅助（转发源模型；顶层根行同样成立） ——
    QString FilePath(const QModelIndex& a_stIdx) const {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return (m_pFs != nullptr && stSrc.isValid()) ? m_pFs->filePath(stSrc)
                                                     : QString();
    }
    bool IsDir(const QModelIndex& a_stIdx) const {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return (m_pFs != nullptr && stSrc.isValid()) ? m_pFs->isDir(stSrc) : false;
    }
    QString FileName(const QModelIndex& a_stIdx) const {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return (m_pFs != nullptr && stSrc.isValid()) ? m_pFs->fileName(stSrc)
                                                     : QString();
    }
    QIcon FileIcon(const QModelIndex& a_stIdx) const {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return (m_pFs != nullptr && stSrc.isValid()) ? m_pFs->fileIcon(stSrc) : QIcon();
    }

signals:
    // 根集合/顺序外部变化（磁盘删根剪除/改名同步）——面板收到后持久化+刷新按钮；
    //   面板自身驱动的增删/移动不 emit（由调用点显式持久化）。
    void sigRootsChanged();

public:
    // —— QAbstractProxyModel 纯虚（public：面板/视图直接调用） ——
    QModelIndex mapFromSource(const QModelIndex& a_stSrc) const override {
        if (!a_stSrc.isValid() || m_aRootSrc.isEmpty()) {
            return QModelIndex();
        }
        for (int i = 0; i < m_aRootSrc.size(); ++i) {
            if (a_stSrc == m_aRootSrc.at(i)) {
                return index(i, a_stSrc.column(), QModelIndex());
            }
        }
        const QModelIndex stSrcParent = a_stSrc.parent();
        if (!stSrcParent.isValid()) {
            return QModelIndex();   // 驱动器层/根的祖先：不映射（顶层只有根行）
        }
        const QModelIndex stOurParent = mapFromSource(stSrcParent);
        if (!stOurParent.isValid()) {
            return QModelIndex();
        }
        return index(a_stSrc.row(), a_stSrc.column(), stOurParent);
    }
    QModelIndex mapToSource(const QModelIndex& a_stProxy) const override {
        if (!a_stProxy.isValid() || m_pFs == nullptr) {
            return QModelIndex();
        }
        const quintptr id = a_stProxy.internalId();
        if (id & 1) {
            // 顶层根行（奇 internalId）。绝不能在这里调 a_stProxy.parent()——
            //   会回调本类 parent()→mapToSource() 互相死循环（栈溢出
            //   0xC00000FD，2026.09.21 实测；槽位制子级索引就是为了绕开它）。
            if (a_stProxy.row() < 0 || a_stProxy.row() >= m_aRootSrc.size()) {
                return QModelIndex();
            }
            const QModelIndex stRoot = m_aRootSrc.at(a_stProxy.row());
            if (!stRoot.isValid()) {
                return QModelIndex();
            }
            return a_stProxy.column() == 0
                ? stRoot
                : stRoot.sibling(stRoot.row(), a_stProxy.column());
        }
        // 子级：槽号解码 → 源父持久索引 → 源子索引（全程不碰自身 parent()）。
        const int iSlot = int(id >> 1) - 1;
        if (iSlot < 0 || iSlot >= m_aSlots.size()) {
            return QModelIndex();
        }
        const QModelIndex stSrcParent = m_aSlots.at(iSlot);
        if (!stSrcParent.isValid()) {
            return QModelIndex();
        }
        return m_pFs->index(a_stProxy.row(), a_stProxy.column(), stSrcParent);
    }

    // —— QAbstractItemModel 必需实现（public：面板/视图直接调用） ——
    QModelIndex index(int a_iRow, int a_iCol,
                      const QModelIndex& a_stParent = QModelIndex()) const override {
        if (a_iRow < 0 || a_iCol < 0
            || a_iCol >= columnCount(a_stParent)
            || a_iRow >= rowCount(a_stParent)) {
            return QModelIndex();
        }
        if (!a_stParent.isValid()) {
            // 顶层根行：internalId=行号*2+1（奇数；子级=(槽号+1)*2 必偶——奇偶
            //   分流即顶层/子级判定；QModelIndex 等值=row/col/internal/model 四元
            //   组不含 parent，子级槽号（源父唯一）+ 行号保证唯一性；旧版 nullptr
            //   指针跨根同位置碰撞、mapToSource↔parent 死循环均已修复
            //   （2026.09.21；嵌套根（根包含根）同样会碰撞，面板侧已拒绝）。
            return createIndex(a_iRow, a_iCol,
                               reinterpret_cast<void*>(
                                   static_cast<quintptr>(a_iRow * 2 + 1)));
        }
        const QModelIndex stSrcParent = mapToSource(a_stParent);
        if (m_pFs == nullptr || !stSrcParent.isValid()) {
            return QModelIndex();
        }
        const int iSlot = SlotFor(stSrcParent);
        return createIndex(a_iRow, a_iCol,
                           reinterpret_cast<void*>(
                               static_cast<quintptr>((iSlot + 1) * 2)));
    }
    QModelIndex parent(const QModelIndex& a_stIdx) const override {
        if (!a_stIdx.isValid() || (a_stIdx.internalId() & 1)) {
            return QModelIndex();   // 顶层根行（奇 internalId）无父
        }
        const int iSlot = int(a_stIdx.internalId() >> 1) - 1;
        if (iSlot < 0 || iSlot >= m_aSlots.size()) {
            return QModelIndex();
        }
        const QModelIndex stSrcParent = m_aSlots.at(iSlot);
        if (!stSrcParent.isValid()) {
            return QModelIndex();
        }
        return mapFromSource(stSrcParent);   // 源链上溯（安全，非自身索引）
    }
    int rowCount(const QModelIndex& a_stParent = QModelIndex()) const override {
        if (m_pFs == nullptr) {
            return 0;
        }
        if (!a_stParent.isValid()) {
            return m_aRoots.size();
        }
        const QModelIndex stSrc = mapToSource(a_stParent);
        return stSrc.isValid() ? m_pFs->rowCount(stSrc) : 0;
    }
    int columnCount(const QModelIndex& a_stParent = QModelIndex()) const override {
        // QFileSystemModel 恒 4 列；顶层根行同样 4（尺寸/类型/日期列由视图隐藏）。
        return m_pFs != nullptr ? 4 : 0;
    }
    QVariant data(const QModelIndex& a_stIdx, int a_iRole) const override {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return stSrc.isValid() ? stSrc.data(a_iRole) : QVariant();
    }
    Qt::ItemFlags flags(const QModelIndex& a_stIdx) const override {
        const QModelIndex stSrc = mapToSource(a_stIdx);
        return (m_pFs != nullptr && stSrc.isValid()) ? m_pFs->flags(stSrc)
                                                     : Qt::NoItemFlags;
    }
    bool hasChildren(const QModelIndex& a_stParent = QModelIndex()) const override {
        if (!a_stParent.isValid()) {
            return !m_aRoots.isEmpty();
        }
        const QModelIndex stSrc = mapToSource(a_stParent);
        return m_pFs != nullptr && stSrc.isValid() ? m_pFs->hasChildren(stSrc)
                                                   : false;
    }
    bool canFetchMore(const QModelIndex& a_stParent) const override {
        if (!a_stParent.isValid()) {
            return false;   // 顶层根行常就绪（子级懒加载交给展开/fetchMore）
        }
        const QModelIndex stSrc = mapToSource(a_stParent);
        return m_pFs != nullptr && stSrc.isValid() ? m_pFs->canFetchMore(stSrc)
                                                   : false;
    }
    void fetchMore(const QModelIndex& a_stParent) override {
        if (!a_stParent.isValid()) {
            return;
        }
        const QModelIndex stSrc = mapToSource(a_stParent);
        if (m_pFs != nullptr && stSrc.isValid()) {
            m_pFs->fetchMore(stSrc);
        }
    }
    void sort(int a_iColumn, Qt::SortOrder a_eOrder) override {
        if (m_pFs == nullptr || a_iColumn < 0) {
            return;
        }
        m_pFs->sort(a_iColumn, a_eOrder);   // 源内部重排；layoutChanged→整体 reset
    }
    QVariant headerData(int a_iSection, Qt::Orientation a_eOrient,
                        int a_iRole) const override {
        return m_pFs != nullptr
            ? m_pFs->headerData(a_iSection, a_eOrient, a_iRole)
            : QVariant();
    }

private:
    // 失效根（源索引死亡=目录被外部删除）剪除；变化后通知面板。
    void PruneDeadRoots() {
        bool bAny = false;
        for (int i = m_aRootSrc.size() - 1; i >= 0; --i) {
            if (!m_aRootSrc.at(i).isValid()) {
                beginRemoveRows(QModelIndex(), i, i);
                m_aRoots.removeAt(i);
                m_aRootSrc.remove(i);
                endRemoveRows();
                bAny = true;
            }
        }
        if (bAny) {
            emit sigRootsChanged();
        }
    }
    // 根目录改名（源索引跟随、存储路径串漂移）→ 回读源索引实际路径同步。
    void ResyncRootPaths() {
        if (m_pFs == nullptr) {
            return;
        }
        bool bAny = false;
        for (int i = 0; i < m_aRoots.size(); ++i) {
            const QModelIndex stRoot = m_aRootSrc.at(i);
            if (!stRoot.isValid()) {
                continue;
            }
            const QString sReal = m_pFs->filePath(stRoot);
            if (!sReal.isEmpty() && sReal != m_aRoots.at(i)) {
                m_aRoots[i] = sReal;
                bAny = true;
            }
        }
        if (bAny) {
            emit sigRootsChanged();
        }
    }
    // 源整体 reset 后所有持久索引失效：按存储路径重解析（目录仍在则根保留）。
    void ReresolveRoots() {
        if (m_pFs == nullptr) {
            return;
        }
        for (int i = 0; i < m_aRoots.size(); ++i) {
            m_aRootSrc[i] = QPersistentModelIndex(m_pFs->index(m_aRoots.at(i)));
        }
        PruneDeadRoots();
    }

    // 源父索引槽（子级索引携槽号；同一源父只占一槽，Node* 作缓存键）。
    int SlotFor(const QModelIndex& a_stSrcParent) const {
        void* k = a_stSrcParent.internalPointer();
        const auto it = m_aSlotOfNode.constFind(k);
        if (it != m_aSlotOfNode.constEnd() && it.value() < m_aSlots.size()
            && m_aSlots.at(it.value()) == a_stSrcParent) {
            return it.value();
        }
        const int iSlot = m_aSlots.size();
        m_aSlots << QPersistentModelIndex(a_stSrcParent);
        m_aSlotOfNode.insert(k, iSlot);
        return iSlot;
    }

    QStringList m_aRoots;                    // 有序根路径（顶层行序）
    QVector<QPersistentModelIndex> m_aRootSrc;   // 对应源索引（监视/映射锚点）
    mutable QVector<QPersistentModelIndex> m_aSlots;      // 源父索引槽
    mutable QHash<void*, int> m_aSlotOfNode;              // 源父 Node* → 槽号
    QFileSystemModel* m_pFs = nullptr;
    bool m_bInsArmed = false;   // beginInsertRows 已发（源 about/inserted 成对消费）
    bool m_bRmArmed = false;    // beginRemoveRows 同上
    // 2026.09.22 源 layoutChanged 规范转发（不再整体 reset）：变更窗口内捕获的
    //   代理持久索引与其源映射（持久索引随源重排自动更新，changed 时回映射）。
    QModelIndexList m_aLayoutProxy;
    QVector<QPersistentModelIndex> m_aLayoutSrc;
};

CProjectExplorerPanel::CProjectExplorerPanel(QWidget* a_pParent)
    : QWidget(a_pParent) {
    auto* pV = new QVBoxLayout(this);
    pV->setContentsMargins(0, 0, 0, 0);
    pV->setSpacing(0);

    m_pModel = new QFileSystemModel(this);
    m_pModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot |
                        QDir::NoSymLinks | QDir::Hidden);
    // 文件不做类型过滤、全部显示（2026-08-17 修正：旧 setNameFilters(*.o 等)+
    // setNameFilterDisables(false) 的真实语义是「只显示匹配项」，曾导致源码/文档
    // 全部被隐藏、树里只剩构建产物，与意图相反）。生成目录（build/.git 等）
    // 仍由 HiddenDirs 按目录整体隐藏。
    // 2026.08.26 补 QDir::Hidden：否则点开头的目录/文件（.autodev 等）在模型层就不出现，
    // HiddenDirs 找不到行可隐藏/显示；.git/.vs 等仍由 HiddenDirs 名单按目录隐藏。
    // 2026.09.21 树改挂多根代理（顶层=根集合，子树映射共享同一源模型）。
    m_pProxy = new CExplorerProxyModel(m_pModel, this);

    m_pTree = new QTreeView(this);
    m_pTree->setModel(m_pProxy);
    // FR-8：树控件字号恒为 11pt，独立于 main.cpp 全局字体（即使被单独设了其它字号也呈现 11pt）。
    QFont fTree = m_pTree->font();
    fTree.setPointSize(11);
    m_pTree->setFont(fTree);
    // header 虽隐藏（setHeaderHidden），一并设字号兜底，防止后续开启 header 时字号不一致。
    m_pTree->header()->setFont(fTree);
    m_pTree->setHeaderHidden(true);
    m_pTree->setUniformRowHeights(true);
    m_pTree->setSortingEnabled(true);
    m_pTree->sortByColumn(0, Qt::AscendingOrder);
    // 隐藏除名称列外的尺寸/类型/日期列。
    for (int iCol = 1; iCol < m_pProxy->columnCount(); ++iCol) {
        m_pTree->setColumnHidden(iCol, true);
    }
    m_pTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // 2026.08.27 拖拽（Windows 资源管理器式）：多选 + 拖出（urls→CF_HDROP，可拖到
    // 系统资源管理器/聊天软件发送框）+ 拖入/树内拖动（移动/复制）。拖拽事件
    // 目标是 viewport，统一在 eventFilter 处理并消耗，绕开 QAbstractItemView
    // 内建 DnD（那套走 model 插行，QFileSystemModel 不适用）。
    m_pTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTree->setAcceptDrops(true);   // QAbstractScrollArea 自动转发到 viewport
    m_pTree->viewport()->installEventFilter(this);
    connect(m_pTree, &QTreeView::customContextMenuRequested,
            this, &CProjectExplorerPanel::OnCustomContextMenu);
    connect(m_pTree, &QTreeView::doubleClicked,
            this, &CProjectExplorerPanel::OnDoubleClicked);
    // 2026.08.21 单击文件：与双击一致打开（文本→内嵌预览、其他→系统默认程序），
    //   但文本文件不切换 Tab（只后台加载内容），用户停留当前页。
    connect(m_pTree, &QTreeView::clicked,
            this, &CProjectExplorerPanel::OnSingleClicked);
    // 父目录行随模型异步陆续插入：新行若命中 HiddenDirs 清单（build/.git 等）则整棵
    // 隐藏（2026.09.21 起挂多根代理——根子树之外的事件被代理吞掉，旧版隐藏兄弟行
    // 机制随多根重构移除）。
    connect(m_pProxy, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex& a_stParent, int a_iFirst, int a_iLast) {
        HideGeneratedDirs(a_stParent, a_iFirst, a_iLast);
    });
    // 2026.09.21 选中变化 → 多根三按钮可用性刷新。
    connect(m_pTree->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex&, const QModelIndex&) {
        UpdateRootButtons();
    });
    connect(m_pProxy, &CExplorerProxyModel::sigRootsChanged,
            this, &CProjectExplorerPanel::OnProxyRootsChanged);
    // —— 2026.09.22 展开态自愈：记住展开路径 + 结构事件后重放 ——
    connect(m_pTree, &QTreeView::expanded, this, [this](const QModelIndex& stIdx) {
        RememberExpand(stIdx, true);
    });
    connect(m_pTree, &QTreeView::collapsed, this, [this](const QModelIndex& stIdx) {
        RememberExpand(stIdx, false);
    });
    // 结构事件源（源模型增量插入/删除、延迟重排、目录列出完成）→ 去抖后重放展开态：
    //   目录被删建/重命名后新节点行异步回来，行一到就按记住的路径重新展开。
    connect(m_pProxy, &QAbstractItemModel::rowsInserted, this, [this]() {
        m_tExpandSync.start();
    });
    connect(m_pProxy, &QAbstractItemModel::layoutChanged,
            &m_tExpandSync, qOverload<>(&QTimer::start));
    connect(m_pModel, &QFileSystemModel::directoryLoaded,
            &m_tExpandSync, qOverload<>(&QTimer::start));
    m_tExpandSync.setSingleShot(true);
    m_tExpandSync.setInterval(200);
    connect(&m_tExpandSync, &QTimer::timeout, this,
            &CProjectExplorerPanel::ReapplyExpandedPaths);
    pV->addWidget(m_pTree);

    // —— 2026.09.21 多根节点三按钮（树下方，仅图标 + tooltip；图标
    //    ow-rootadd/ow-rootup/ow-rootdown，icon_claude 技能绘制，16/32 双尺寸）。
    //    2026.09.21 放大：SVG 重绘（+/↑/↓ 语义符号从角落小角标改为约 2 倍大
    //    主视觉）+ 按钮图标 16→22（QIcon 自动取 32 档缩放，高 DPI 仍清晰）、
    //    按钮尺寸 26x24→34x28——用户反馈旧角标在 16px 显示下仅约 3px 看不清。 ——
    auto* pBtnBar = new QWidget(this);
    auto* pH = new QHBoxLayout(pBtnBar);
    pH->setContentsMargins(4, 2, 4, 2);
    pH->setSpacing(2);
    QFont fBtn = pBtnBar->font();
    fBtn.setPointSize(11);
    pBtnBar->setFont(fBtn);
    auto fnMakeBtn = [this](QToolButton** a_ppBtn, const QString& a_sIcon,
                            const QString& a_sTip) {
        QToolButton* pBtn = new QToolButton(this);
        pBtn->setIcon(MenuIcon(a_sIcon));
        pBtn->setIconSize(QSize(22, 22));
        pBtn->setAutoRaise(true);
        pBtn->setFixedSize(34, 28);
        pBtn->setToolTip(a_sTip);
        *a_ppBtn = pBtn;
    };
    fnMakeBtn(&m_pBtnAddRoot, QStringLiteral("ow-rootadd"),
              QStringLiteral("添加根节点：选择关联项目目录加入本树并排浏览\n"
                             "（选择对话框缺省打开当前工程目录；树顶层右键可移除）"));
    fnMakeBtn(&m_pBtnRootUp, QStringLiteral("ow-rootup"),
              QStringLiteral("根节点上移：把选中的根节点在树顶层向上移动一位"));
    fnMakeBtn(&m_pBtnRootDown, QStringLiteral("ow-rootdown"),
              QStringLiteral("根节点下移：把选中的根节点在树顶层向下移动一位"));
    connect(m_pBtnAddRoot, &QToolButton::clicked,
            this, &CProjectExplorerPanel::OnAddRootClicked);
    connect(m_pBtnRootUp, &QToolButton::clicked,
            this, &CProjectExplorerPanel::OnMoveRootUp);
    connect(m_pBtnRootDown, &QToolButton::clicked,
            this, &CProjectExplorerPanel::OnMoveRootDown);
    pH->addWidget(m_pBtnAddRoot);
    pH->addWidget(m_pBtnRootUp);
    pH->addWidget(m_pBtnRootDown);
    pH->addStretch(1);
    pV->addWidget(pBtnBar);
    UpdateRootButtons();
}

CProjectExplorerPanel::~CProjectExplorerPanel() {
    ClearOpenWithHandlers();
}

// 2026.09.17 打开方式 COM 处理器条目（SHAssocEnumHandlers 枚举产物；菜单项 data=索引）。
struct CProjectExplorerPanel::SOpenWithHandler {
    IAssocHandler* m_pHandler = nullptr;   // 引用计数持有（Next 已 AddRef，Release 于 Clear）
};

void CProjectExplorerPanel::ClearOpenWithHandlers() {
    for (SOpenWithHandler* pEntry : m_aOpenWithHandlers) {
        if (pEntry->m_pHandler != nullptr) {
            pEntry->m_pHandler->Release();
        }
        delete pEntry;
    }
    m_aOpenWithHandlers.clear();
}

// 2026.09.17 处理器显示名（兑底启动失败时的友好提示用；取不到回退「未知程序」）。
static QString MenuAssocName(IAssocHandler* a_pHandler) {
    if (a_pHandler == nullptr) {
        return QStringLiteral("未知程序");
    }
    PWSTR pwsz = nullptr;
    QString sName;
    if (SUCCEEDED(a_pHandler->GetUIName(&pwsz)) && pwsz != nullptr) {
        sName = QString::fromWCharArray(pwsz);
        CoTaskMemFree(pwsz);
    }
    return sName.trimmed().isEmpty() ? QStringLiteral("未知程序") : sName.trimmed();
}

void CProjectExplorerPanel::RunBatViaCmd(const QString& a_sAbsPath) {
    // 2026.09.26 v3.1.244 「用 CMD 运行」：cmd /k 执行批处理，完成后保留窗口
    //   （显示输出，工程师场景要看得见执行结果；/c 会闪退丢输出）。
    //   /k ""路径"" 双引号形态：cmd 对 /k 后串剥首末引号，内层引号保护含空格路径
    //   （同 v2.1.121 cmd /k 引号规则先例）。
    //   cwd=文件所在目录（批处理常用相对路径）；ShellExecuteExW 拿 pid 登记
    //   沙箱豁免（同 OpenWithShell——否则 cmd 窗口作为 AutoCode 后代被自动收纳）。
    const QString sNative = QDir::toNativeSeparators(a_sAbsPath);
    const QString sDir = QDir::toNativeSeparators(QFileInfo(a_sAbsPath).absolutePath());
    const QString sParams = QStringLiteral("/k \"\"%1\"\"").arg(sNative);
    SHELLEXECUTEINFOW stSei = {};
    stSei.cbSize = sizeof(stSei);
    stSei.fMask = SEE_MASK_NOCLOSEPROCESS;
    stSei.lpFile = L"cmd.exe";
    stSei.lpParameters = reinterpret_cast<LPCWSTR>(sParams.utf16());
    stSei.lpDirectory = reinterpret_cast<LPCWSTR>(sDir.utf16());
    stSei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&stSei)) {
        if (stSei.hProcess != nullptr) {
            const DWORD dwPid = GetProcessId(stSei.hProcess);
            if (dwPid != 0) {
                CSandboxManager::RegisterUserRoot(dwPid, stSei.hProcess);   // 句柄移交
                return;
            }
            CloseHandle(stSei.hProcess);
        }
        return;
    }
    QMessageBox::warning(this, QStringLiteral("运行失败"),
        QStringLiteral("无法用 CMD 运行批处理（错误码 %1）：\n%2")
            .arg(QString::number(static_cast<qulonglong>(GetLastError())))
            .arg(sNative));
}

void CProjectExplorerPanel::InvokeAssocHandler(int a_iIndex, const QString& a_sFilePath) {
    if (a_iIndex < 0 || a_iIndex >= m_aOpenWithHandlers.size()) {
        return;
    }
    IAssocHandler* pH = m_aOpenWithHandlers.at(a_iIndex)->m_pHandler;
    if (pH == nullptr) {
        return;
    }
    const QString sNative = QDir::toNativeSeparators(a_sFilePath);
    const QString sParam = QStringLiteral("\"%1\"").arg(sNative);   // 引号防路径空格

    // 2026.09.18 v2.3.174 启动顺序重构（用户实测：shell Invoke 在本机会弹系统
    //   「选择打开文件」选择器/E_FAIL，点记事本都直达不了）——**直启为主、Invoke 兜底**：
    //   ① GetName = 处理器注册的程序路径（权威 exe；cleanPath 折叠注册值双反斜杠、
    //      展开环境变量）→ ShellExecuteW 直启并传文件（引号包裹参数，cwd=exe 目录）。
    //   ② GetIconLocation 图标路径同法（兼容图标指向与主程序不同盘的处理器）。
    //   ③ 仅当①②都拿不到可用 exe（极罕见）才走 shell Invoke（BHID 规范数据对象）。
    //   全部失败：友好提示含程序名与文件。
    QStringList aExeCands;
    PWSTR pwsz = nullptr;
    if (SUCCEEDED(pH->GetName(&pwsz)) && pwsz != nullptr) {
        aExeCands.append(QString::fromWCharArray(pwsz));
        CoTaskMemFree(pwsz);
    }
    int iIconIdx = 0;
    if (SUCCEEDED(pH->GetIconLocation(&pwsz, &iIconIdx)) && pwsz != nullptr) {
        aExeCands.append(QString::fromWCharArray(pwsz));
        CoTaskMemFree(pwsz);
    }
    for (const QString& sRaw : aExeCands) {
        const QString sExe = QDir::cleanPath(ExpandEnvVars(sRaw).trimmed());
        if (sExe.isEmpty() || !QFileInfo::exists(sExe)) {
            continue;
        }
        const QString sWorkDir = QFileInfo(sExe).absolutePath();
        // 2026.09.26 同 OpenWithShell：ShellExecuteExW 拿 pid 登记沙箱豁免
        //   （用户选的程序打开文件，不是 Agent 被测对象，不收纳不录像）。
        SHELLEXECUTEINFOW stSei = {};
        stSei.cbSize = sizeof(stSei);
        stSei.fMask = SEE_MASK_NOCLOSEPROCESS;
        stSei.lpFile = reinterpret_cast<LPCWSTR>(
            QDir::toNativeSeparators(sExe).utf16());
        stSei.lpParameters = reinterpret_cast<LPCWSTR>(sParam.utf16());   // 引号包裹的目标文件
        stSei.lpDirectory = reinterpret_cast<LPCWSTR>(
            QDir::toNativeSeparators(sWorkDir).utf16());
        stSei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&stSei)) {
            if (stSei.hProcess != nullptr) {
                const DWORD dwPid = GetProcessId(stSei.hProcess);
                if (dwPid != 0) {
                    CSandboxManager::RegisterUserRoot(dwPid, stSei.hProcess);
                } else {
                    CloseHandle(stSei.hProcess);
                }
            }
            return;
        }
        qWarning() << "[OpenWith] 直启失败 err=" << GetLastError() << "exe=" << sExe;
    }

    // ③ 兜底：shell Invoke（BHID 规范数据对象）。注意：本机实测 shell 内部
    //    ShellExecuteEx 可能 E_FAIL/弹 openas 选择器，故仅在前两路拿不到 exe 时才走。
    IShellItem* psi = nullptr;
    IDataObject* pdo = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(
            reinterpret_cast<const wchar_t*>(sNative.utf16()),
            nullptr, IID_PPV_ARGS(&psi))) && psi != nullptr) {
        psi->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&pdo));
        psi->Release();
    }
    if (pdo != nullptr) {
        const HRESULT hrInvoke = pH->Invoke(pdo);
        pdo->Release();
        if (SUCCEEDED(hrInvoke)) {
            return;
        }
        qWarning() << "[OpenWith] shell Invoke 失败 hr=0x" << Qt::hex << (quint32)hrInvoke;
    }

    // ④ 全部失败：友好提示（含程序名/文件/rc，不再让 shell 弹裸报错）。
    QMessageBox::warning(nullptr, QStringLiteral("打开方式"),
        QStringLiteral("未能启动该程序打开文件。\n\n"
                       "程序：%1\n文件：%2\n\n"
                       "该程序的文件关联可能已失效（残留注册项），可在 Windows 资源"
                       "管理器中对同类文件右键「打开方式→选择其他应用」重新关联。")
            .arg(MenuAssocName(pH), a_sFilePath));
}

void CProjectExplorerPanel::SetRoot(const QString& a_sDir) {
    // 2026.09.21 多根：工程根入根列表首项，同时载入 <工程>/.autodev/explorer.json
    //   持久化的有序根（过滤不存在目录/去重；工程根缺位时置首）。树顶层 = 根集合，
    //   旧版「视图根=父目录+隐藏兄弟行」机制随多根重构移除（映射天然隔离）。
    const QString sNew = a_sDir.isEmpty()
        ? QString()
        : QDir::cleanPath(QDir::fromNativeSeparators(a_sDir));
    // 2026.09.22 换工程即作废展开记忆（旧工程路径不应重放到新工程）。
    if (QString::compare(sNew, m_sRootDir, Qt::CaseInsensitive) != 0) {
        m_hExpandPaths.clear();
    }
    m_sRootDir = sNew;
    if (m_pProxy == nullptr) {
        return;
    }
    if (m_sRootDir.isEmpty()) {
        m_pProxy->ResetRoots({}, {});
        UpdateRootButtons();
        return;
    }
    QStringList aWanted = LoadExplorerState();
    bool bHasPrimary = false;
    for (const QString& s : aWanted) {
        if (QString::compare(QDir::cleanPath(s), m_sRootDir,
                             Qt::CaseInsensitive) == 0) {
            bHasPrimary = true;
            break;
        }
    }
    if (!bHasPrimary) {
        aWanted.prepend(m_sRootDir);
    }
    QStringList aFinal;
    QSet<QString> aSeen;
    for (const QString& sRaw : aWanted) {
        const QString s = QDir::cleanPath(sRaw);
        const QString sKey = s.toLower();
        if (sKey.isEmpty() || aSeen.contains(sKey) || !QFileInfo(s).isDir()) {
            continue;
        }
        aSeen.insert(sKey);
        aFinal << s;
    }
    m_pProxy->ResetRoots(aFinal, m_sRootDir);
    if (m_pProxy->RootPaths() != aFinal) {
        SaveExplorerState();   // 坏根被剪（索引解析失败）：回写收敛
    }
    // 顶层根各展开一级（懒加载；常态只有工程根一个，多根时关联项目也直接可见）。
    for (int i = 0; i < m_pProxy->rowCount(); ++i) {
        m_pTree->expand(m_pProxy->index(i, 0));
    }
    // 2026.09.22 重放已记住的子目录展开态（ResetRoots 全量 reset 会清展开态；
    //   已列出的目录立即恢复，未列出的由 directoryLoaded 驱动后续重放）。
    m_tExpandSync.start();
    UpdateRootButtons();
}

void CProjectExplorerPanel::HideGeneratedDirs(const QModelIndex& a_stParent,
                                              int a_iFirst, int a_iLast) {
    if (!a_stParent.isValid() || m_pProxy == nullptr) {
        return;   // 顶层根行不走目录隐藏（根由用户/工程显式指定，名字撞清单也显示）
    }
    const int iFirst = a_iFirst < 0 ? 0 : a_iFirst;
    const int iCount = m_pProxy->rowCount(a_stParent);
    for (int iRow = iFirst; iRow <= a_iLast && iRow < iCount; ++iRow) {
        const QModelIndex stRow = m_pProxy->index(iRow, 0, a_stParent);
        if (!stRow.isValid() || !m_pProxy->IsDir(stRow)) {
            continue;   // 文件不走目录隐藏
        }
        // 2026.09.23 用实例清单（SetHiddenDirs 可配置；缺省=内置 HiddenDirs()）。
        if (m_aHiddenDirs.contains(m_pProxy->FileName(stRow), Qt::CaseInsensitive)) {
            m_pTree->setRowHidden(iRow, a_stParent, true);
        }
    }
}

// 2026.09.23 显示隐藏清单变化 → 已加载全树重隐/重显（仅遍历模型已取节点；
//   未取节点由后续 rowsInserted 钩子按新清单处理）。
void CProjectExplorerPanel::ReapplyHiddenDirs() {
    if (m_pTree == nullptr || m_pProxy == nullptr) {
        return;
    }
    std::function<void(const QModelIndex&)> fnWalk =
        [&](const QModelIndex& a_stParent) {
        const int iCount = m_pProxy->rowCount(a_stParent);
        for (int iRow = 0; iRow < iCount; ++iRow) {
            const QModelIndex stRow = m_pProxy->index(iRow, 0, a_stParent);
            if (!stRow.isValid()) {
                continue;
            }
            const bool bDir = m_pProxy->IsDir(stRow);
            const bool bHide = bDir
                && m_aHiddenDirs.contains(m_pProxy->FileName(stRow), Qt::CaseInsensitive);
            m_pTree->setRowHidden(iRow, a_stParent, bHide);
            if (bDir) {
                fnWalk(stRow);   // 目录递归（隐藏行的子树同样校正，展开后状态即正确）
            }
        }
    };
    for (int i = 0; i < m_pProxy->rowCount(); ++i) {
        fnWalk(m_pProxy->index(i, 0));   // 顶层根行不隐藏，子树递归
    }
}

void CProjectExplorerPanel::SetHiddenDirs(const QStringList& a_aNames) {
    QStringList aNew;
    for (const QString& s : a_aNames) {
        const QString sName = s.trimmed();
        if (!sName.isEmpty()) {
            aNew << sName;
        }
    }
    if (aNew == m_aHiddenDirs) {
        return;   // 未变不折腾（热重载/重复应用路径）
    }
    m_aHiddenDirs = aNew;
    ReapplyHiddenDirs();
}

QString CProjectExplorerPanel::SelectedAbsPath() const {
    const QModelIndex stIdx = m_pTree->currentIndex();
    if (!stIdx.isValid()) {
        return QString();
    }
    const QString sPath = m_pProxy->FilePath(stIdx);
    if (sPath.isEmpty()) {
        return QString();
    }
    return sPath;
}

QStringList CProjectExplorerPanel::SelectedAbsPaths() const {
    // 2026.09.19 多选取（剪切/复制用，与拖拽同口径）：树内全部选中行，限工程根内；
    //   无多选时回退当前项。空=无可用源。
    QModelIndexList aIdx = m_pTree->selectionModel()->selectedRows(0);
    if (aIdx.isEmpty()) {
        const QString sOne = SelectedAbsPath();
        return sOne.isEmpty() ? QStringList() : QStringList(sOne);
    }
    QStringList aPaths;
    for (const QModelIndex& stIdx : aIdx) {
        const QString sPath = m_pProxy->FilePath(stIdx);
        if (IsUnderRoot(sPath)) {
            aPaths << sPath;
        }
    }
    return aPaths;
}

bool CProjectExplorerPanel::IsUnderRoot(const QString& a_sAbsPath) const {
    // 2026.09.21 多根：任一根子树内（含根本身）即合法；工程根未设时全拒。
    if (m_sRootDir.isEmpty() || a_sAbsPath.isEmpty()) {
        return false;
    }
    for (const QString& sRoot : RootPaths()) {
        if (sRoot.isEmpty()) {
            continue;
        }
        const QString sRootC = QDir(sRoot).canonicalPath();
        const QString sAbsC  = QDir(a_sAbsPath).canonicalPath();
        if (!sRootC.isEmpty() && !sAbsC.isEmpty()) {
            if (sAbsC == sRootC || sAbsC.startsWith(sRootC + QLatin1Char('/'))) {
                return true;
            }
            continue;
        }
        // 路径尚不存在（如目标尚未创建）→ 用规范化路径做前缀比较兜底。
        const QString sRootN = QDir::cleanPath(sRoot);
        const QString sAbsN  = QDir::cleanPath(a_sAbsPath);
        if (sAbsN == sRootN || sAbsN.startsWith(sRootN + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

QString CProjectExplorerPanel::ContainingRoot(const QString& a_sAbsPath) const {
    // 2026.09.21 最深包含根（相对路径基准）：关联根嵌套工程根时同一文件落在多个
    //   根子树内，取路径最长的根——相对路径最有意义。匹配口径与 IsUnderRoot 相同
    //   （canonical 优先，退 clean 前缀）。
    QString sBest;
    const QString sAbsC = QDir(a_sAbsPath).canonicalPath();
    const QString sAbsN = QDir::cleanPath(a_sAbsPath);
    for (const QString& sRoot : RootPaths()) {
        if (sRoot.isEmpty()) {
            continue;
        }
        const QString sRootC = QDir(sRoot).canonicalPath();
        const bool bCanon = !sRootC.isEmpty() && !sAbsC.isEmpty();
        const QString sRootUse = bCanon ? sRootC : QDir::cleanPath(sRoot);
        const QString sAbsUse  = bCanon ? sAbsC : sAbsN;
        if (sAbsUse == sRootUse || sAbsUse.startsWith(sRootUse + QLatin1Char('/'))) {
            if (sRootUse.size() > sBest.size()) {
                sBest = sRootUse;
            }
        }
    }
    return sBest;
}

void CProjectExplorerPanel::OnCustomContextMenu(const QPoint& a_stPos) {
    const QModelIndex stIdx = m_pTree->indexAt(a_stPos);
    if (!stIdx.isValid()) {
        return;
    }
    m_pTree->setCurrentIndex(stIdx);
    const QString sSel = SelectedAbsPath();
    const bool bUnderRoot = IsUnderRoot(sSel);

    QMenu menu(this);
    // 2026.09.17 菜单项图标（icon_claude 技能生成：svg 源 resources/svg/ow-*.svg，
    //   16/32 双尺寸 png 内嵌 qrc，主色 #2563eb 同 user.svg，删除红/聊天加号绿点缀）。
    // 2026.09.21 关联根节点专属：「移除该根节点」置顶（仅额外根；工程根随工程存在
    //   不可移除；仅从树移除，不动磁盘目录）。
    const bool bRootRow = stIdx.isValid() && !stIdx.parent().isValid();
    const QString sRootAtRow = bRootRow ? RootPathAt(stIdx.row()) : QString();
    const bool bExtraRootRow = bRootRow && !sRootAtRow.isEmpty()
        && QString::compare(QDir::cleanPath(sRootAtRow), QDir::cleanPath(m_sRootDir),
                            Qt::CaseInsensitive) != 0;
    QAction* pActRemoveRoot = nullptr;
    if (bExtraRootRow) {
        pActRemoveRoot = menu.addAction(
            MenuIcon(QStringLiteral("ow-remove")),
            QStringLiteral("移除该根节点（仅移出树，保留磁盘目录）(&Y)"));
        menu.addSeparator();
    }
    // 2026-10-08 菜单项加快捷键（用户需求：菜单弹出后按键字母即触发，如 R=属性）：
    //   括号助记风格同 Windows 中文菜单；字母全菜单内唯一——标准操作沿用 Windows
    //   习惯（X剪切/C复制/V粘贴/D删除/M改名/A复制路径），其余按语义/拼音首字母
    //   （E资源管理器/W打开方式/N新建/L相对路径/H聊天/S审查/R属性/F刷新/Y移除根）。
    QAction* pActExplorer  = menu.addAction(
        MenuIcon(QStringLiteral("ow-explorer")), QStringLiteral("通过资源管理器打开(&E)"));
    // 2026.08.21 「打开方式」子菜单：列出该文件类型在注册表注册过的程序（仅文件）。
    QMenu* pMenuOpenWith = nullptr;
    if (QFileInfo(sSel).isFile()) {
        pMenuOpenWith = menu.addMenu(
            MenuIcon(QStringLiteral("ow-openwith")), QStringLiteral("打开方式(&W)"));
        BuildOpenWithMenu(pMenuOpenWith, sSel);
    }
    QAction* pActCut       = menu.addAction(
        MenuIcon(QStringLiteral("ow-cut")), QStringLiteral("剪切(&X)"));
    QAction* pActCopy      = menu.addAction(
        MenuIcon(QStringLiteral("ow-copy")), QStringLiteral("复制(&C)"));
    QAction* pActPaste     = menu.addAction(
        MenuIcon(QStringLiteral("ow-paste")), QStringLiteral("粘贴(&V)"));
    // 2026.09.26 v3.1.247 新建文件夹：目标=选中目录/选中文件父目录/根；建完即行内改名。
    QAction* pActNewFolder = menu.addAction(
        MenuIcon(QStringLiteral("ow-newfolder")), QStringLiteral("新建文件夹(&N)"));
    QAction* pActRename    = menu.addAction(
        MenuIcon(QStringLiteral("ow-rename")), QStringLiteral("改名(&M)"));
    menu.addSeparator();
    QAction* pActDelete    = menu.addAction(
        MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除(&D)"));
    menu.addSeparator();
    QAction* pActCopyPath  = menu.addAction(
        MenuIcon(QStringLiteral("ow-copypath")), QStringLiteral("复制路径(&A)"));
    QAction* pActCopyRel   = menu.addAction(
        MenuIcon(QStringLiteral("ow-copyrel")), QStringLiteral("复制相对路径(&L)"));
    QAction* pActAddChat   = menu.addAction(
        MenuIcon(QStringLiteral("ow-chat")), QStringLiteral("添加到聊天框(&H)"));
    // 2026-09-30 代码审查（ReviewSdk 静态审查，选中文件/文件夹均可）。
    QAction* pActCReview   = menu.addAction(
        MenuIcon(QStringLiteral("ow-review")), QStringLiteral("代码审查(&S)"));
    // 2026.09.23 刷新：树更新依赖 QFileSystemModel 内部 watcher 的文件系统事件，
    //   Windows 通知缓冲溢出/杀软延迟会丢事件致树停滞（如 Agent 批量落盘后新文件
    //   不出现）——手动触发一次全量重列（详见 NudgeDirWatch）。视图级操作恒可用。
    //   2026.10.20 与「属性」位置互换（刷新在上、属性在下），分隔线随组前移。
    menu.addSeparator();
    QAction* pActRefresh   = menu.addAction(
        MenuIcon(QStringLiteral("ow-refresh")), QStringLiteral("刷新(&F)"));
    // 2026.10.02 属性：SHObjectProperties 原生属性框（失败兑底自绘）；只读/
    //   隐藏勾选可写回，与 NFR-7 口径一致根外禁用。
    QAction* pActProps     = menu.addAction(
        MenuIcon(QStringLiteral("ow-properties")), QStringLiteral("属性(&R)"));

    // 粘贴依赖工程根已设 + 剪贴板可能有文件（系统 CF_HDROP——含资源管理器剪切/复制，
    //   或写板失败时的内部快照兑底）；其余依赖选中节点在根内。
    pActPaste->setEnabled(!m_sRootDir.isEmpty()
                          && (IsClipboardFormatAvailable(CF_HDROP)
                              || !m_aClipFiles.isEmpty()));
    if (!bUnderRoot) {
        // 选中根外节点：禁用所有写操作与路径操作（NFR-7）。
        for (QAction* pA : {pActCut, pActCopy, pActPaste, pActNewFolder, pActRename,
                            pActDelete, pActCopyPath, pActCopyRel, pActAddChat,
                            pActExplorer, pActCReview, pActProps}) {
            pA->setEnabled(false);
        }
        if (pMenuOpenWith != nullptr) {
            pMenuOpenWith->setEnabled(false);
        }
    }

    QAction* pSel = menu.exec(m_pTree->viewport()->mapToGlobal(a_stPos));
    if (pSel == nullptr) {
        return;
    }
    // 2026.08.21 「打开方式」子菜单项：data() 携带该程序的 command 模板，命中即
    //   以模板展开 %1 启动对应注册程序打开文件，不落入下方固定项分支。
    //   2026.09.17 主路径改为 SHAssocEnumHandlers 枚举：data=索引，交 IAssocHandler::
    //   Invoke 由 shell 启动（自动处理 %1 展开/DDE 动词如 devenv /dde/提权提示）；
    //   模板串仅为 COM 枚举整体失败时的注册表兑底路径。
    if (pSel->data().userType() == QMetaType::Int) {
        const int iIdx = pSel->data().toInt();
        // 2026.09.26 v3.1.244 「用 CMD 运行」哨兵（仅 .bat/.cmd 菜单首项）：
        //   cmd /k 执行批处理（见 RunBatViaCmd），不进 COM 处理器链。
        if (iIdx == s_iOpenWithCmdRun) {
            RunBatViaCmd(sSel);
            return;
        }
        InvokeAssocHandler(iIdx, sSel);
        return;
    }
    if (pSel->data().isValid() && pSel->data().canConvert<QString>()) {
        LaunchOpenWithCommand(pSel->data().toString(), sSel);
        return;
    }
    if (pSel == pActRemoveRoot) {
        // 2026-10-16 移除根节点二次确认（防误删；RemoveExtraRoot 保持纯逻辑供单测直调，
        //   确认弹窗只在 UI 入口）：默认 No，回车/Esc 不误触；文案重申仅移出树不动磁盘。
        //   2026-10-16 弹窗字体显式 11pt（正文+按钮，同 CMainWindow::ExecQuestion 口径）：
        //   不依赖继承（部分风格/路径下按钮不跟随 box 字体，工程内已有先例注释）。
        const QString sRootName = QFileInfo(sRootAtRow).fileName();
        QMessageBox stBox(QMessageBox::Question, QStringLiteral("移除根节点"),
            QStringLiteral("确认把根节点「%1」从资源管理器树中移除？\n"
                           "（仅移出树，磁盘目录与文件不受影响；可随时重新添加）")
                .arg(sRootName),
            QMessageBox::Yes | QMessageBox::No, this);
        stBox.setDefaultButton(QMessageBox::No);
        {
            QFont fBox = stBox.font();
            fBox.setPointSize(11);
            stBox.setFont(fBox);
            for (QAbstractButton* pB : stBox.buttons()) {
                QFont fB = pB->font();
                fB.setPointSize(11);
                pB->setFont(fB);
            }
        }
        if (stBox.exec() == QMessageBox::Yes) {
            RemoveExtraRoot(sRootAtRow);
        }
    }
    else if (pSel == pActExplorer) { OpenInExplorer(); }
    else if (pSel == pActCut)      { DoCut(); }
    else if (pSel == pActCopy)     { DoCopy(); }
    else if (pSel == pActPaste)    { DoPaste(); }
    else if (pSel == pActNewFolder) { StartNewFolder(); }
    else if (pSel == pActRename)   { DoRename(); }
    else if (pSel == pActDelete)   { DoDelete(); }
    else if (pSel == pActCopyPath) { CopyPath(); }
    else if (pSel == pActCopyRel)  { CopyRelativePath(); }
    else if (pSel == pActAddChat)  { AddToChat(); }
    else if (pSel == pActCReview)  { emit sigCodeReview(sSel); }
    else if (pSel == pActProps)    { StartProperties(sSel); }
    else if (pSel == pActRefresh)  { RefreshSelected(); }
}

void CProjectExplorerPanel::OnDoubleClicked(const QModelIndex& a_stIdx) {
    // 2026.08.18 双击文件：文本文件 → 内嵌 Tab 显示（sigFileOpen，经 CTaskListPanel
    //   转发到执行面板）；非文本 → 系统默认程序打开。
    //   2026.08.21 sigFileOpen 增加 a_bSwitchTab 参数，双击=true（切到预览页）。
    //   2026.08.21 目录：不弹资源管理器，直接 return 交给 QTreeView 原生
    //   expandsOnDoubleClick 展开/折叠本节点。
    if (!a_stIdx.isValid()) {
        return;
    }
    const QString sPath = m_pProxy->FilePath(a_stIdx);
    if (sPath.isEmpty() || !IsUnderRoot(sPath)) {
        return;
    }
    const QFileInfo stInfo(sPath);
    if (!stInfo.isFile()) {
        return;   // 目录：双击仅展开/折叠（树原生行为），不再外部打开
    }
    if (IsTextFile(sPath)) {
        emit sigFileOpen(sPath, /*a_bSwitchTab=*/true);
        return;
    }
    OpenWithShell(sPath);
}

void CProjectExplorerPanel::OnSingleClicked(const QModelIndex& a_stIdx) {
    // 2026.08.21 单击文件：与双击语义一致地「打开」，但内嵌预览不切换 Tab
    //   （sigFileOpen 第二参 false：只加载内容与更新页标题，用户停留当前 Tab）。
    //   仅文件响应（目录单击保持原生选中/展开交互，不劫持）；单击先于双击触发，
    //   文本文件随后双击会再带 true 走一遍（幂等：换内容+切页）。
    if (!a_stIdx.isValid()) {
        return;
    }
    const QString sPath = m_pProxy->FilePath(a_stIdx);
    if (sPath.isEmpty() || !IsUnderRoot(sPath)) {
        return;
    }
    const QFileInfo stInfo(sPath);
    if (!stInfo.isFile()) {
        return;   // 目录：单击不动作（留给双击展开/原生交互）
    }
    if (IsTextFile(sPath)) {
        emit sigFileOpen(sPath, /*a_bSwitchTab=*/false);
        return;
    }
    OpenWithShell(sPath);
}

void CProjectExplorerPanel::OpenWithShell(const QString& a_sAbsPath) {
    // 2026.08.28 对齐 Windows 资源管理器双击口径：ShellExecuteW 显式传
    //   lpDirectory=文件所在目录。此前走 QDesktopServices::openUrl，其内部
    //   ShellExecute 的目录参数为 NULL → 子进程继承 AutoCode 的当前工作目录
    //   （Execute\ 等）而非 exe 自己的文件夹——用相对路径读资源/配置的程序
    //   （游戏/便携软件常见）启动即找不到文件退出，表现为「点了没反应」，
    //   而在系统资源管理器双击同一 exe 正常（实测两种口径子进程 cwd 差异
    //   确认：NULL 目录=继承调用方 cwd；传目录=exe 所在目录）。
    //   附带收益：失败弹窗可见真因，旧 openUrl 只写调试输出。
    // 2026.09.26 改 ShellExecuteExW（SEE_MASK_NOCLOSEPROCESS 拿新进程句柄）：
    //   用户主动打开的程序登记为沙箱豁免根——否则点开的 exe 作为 AutoCode 后代
    //   会被自动收纳（窗口凭空消失）并触发录像（本机 Intel 硬编缺陷可崩进程，
    //   实测 4 dump 同 RIP mfx_mft_h264ve_64.dll+0xB575E）。
    const QString sNative = QDir::toNativeSeparators(a_sAbsPath);
    const QString sDir = QDir::toNativeSeparators(
        QFileInfo(a_sAbsPath).absolutePath());
    SHELLEXECUTEINFOW stSei = {};
    stSei.cbSize = sizeof(stSei);
    stSei.fMask = SEE_MASK_NOCLOSEPROCESS;
    stSei.lpFile = reinterpret_cast<LPCWSTR>(sNative.utf16());
    stSei.lpDirectory = reinterpret_cast<LPCWSTR>(sDir.utf16());
    stSei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&stSei)) {
        if (stSei.hProcess != nullptr) {
            const DWORD dwPid = GetProcessId(stSei.hProcess);
            if (dwPid != 0) {
                CSandboxManager::RegisterUserRoot(dwPid, stSei.hProcess);   // 句柄移交
                return;
            }
            CloseHandle(stSei.hProcess);
        }
        return;   // 打开成功但拿不到句柄（DDE 复用既有进程等）：无可豁免，维持原行为
    }
    QMessageBox::warning(this, QStringLiteral("打开失败"),
        QStringLiteral("无法用系统默认程序打开（错误码 %1）：\n%2")
            .arg(QString::number(static_cast<qulonglong>(GetLastError())))
            .arg(sNative));
}

QString CProjectExplorerPanel::UniqueDstPath(const QString& a_sPath) {
    // 目标同名自动加 " (n)" 序号（n 从 2 递增），全部撞名返回空串（调用方按失败计）。
    const QFileInfo stFi(a_sPath);
    const QDir stDir(stFi.absolutePath());
    const QString sBase = stFi.completeBaseName();
    const QString sSuffix = stFi.suffix();
    for (int i = 2; i < 1000; ++i) {
        const QString sCand = sSuffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(sBase).arg(i)
            : QStringLiteral("%1 (%2).%3").arg(sBase).arg(i).arg(sSuffix);
        const QString sFull = stDir.filePath(sCand);
        if (!QFileInfo::exists(sFull)) {
            return sFull;
        }
    }
    return QString();
}

void CProjectExplorerPanel::StartTreeDrag() {
    // 拖出（源）：按下项 + 树内已多选项 → QMimeData::setUrls。Windows 平台 Qt 自动
    // 把 urls 转成 CF_HDROP OLE 数据源：拖到系统资源管理器=复制/移动文件，
    // 拖到聊天软件发送框=作为附件，与从 Windows 资源管理器拖出的行为一致。
    QModelIndexList aIdx = m_pTree->selectionModel()->selectedRows(0);
    if (!aIdx.contains(QModelIndex(m_stDragIdx))) {
        aIdx.clear();
        aIdx << QModelIndex(m_stDragIdx);
    }
    QList<QUrl> aUrls;
    for (const QModelIndex& stIdx : aIdx) {
        const QString sPath = m_pProxy->FilePath(stIdx);
        if (IsUnderRoot(sPath)) {
            aUrls << QUrl::fromLocalFile(sPath);
        }
    }
    if (aUrls.isEmpty()) {
        return;
    }
    auto* pMime = new QMimeData;
    pMime->setUrls(aUrls);
    QDrag stDrag(this);
    stDrag.setMimeData(pMime);
    const QIcon stIcon = m_pProxy->FileIcon(QModelIndex(m_stDragIdx));
    if (!stIcon.isNull()) {
        stDrag.setPixmap(stIcon.pixmap(16, 16));
    }
    // exec 是模态循环；目标在外部移走文件（同盘 Move）时 QFileSystemModel 的
    // 目录监视会自动刷新树，返回值无需再处理。
    stDrag.exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
}

void CProjectExplorerPanel::HandleTreeDrop(QDropEvent* a_pEv) {
    // 拖入（目标）：落点目录判定——命中目录=其内部；命中文件=所在目录；
    // 空白=工程根。内部拖动默认移动（资源管理器同盘语义，Ctrl=复制），
    // 外部（系统资源管理器等）拖入=复制。同名目标自动 " (n)" 编号。
    const QMimeData* pMime = a_pEv->mimeData();
    if (m_sRootDir.isEmpty() || pMime == nullptr || !pMime->hasUrls()) {
        a_pEv->ignore();
        return;
    }
    QString sDstDir;
    const QModelIndex stIdx = m_pTree->indexAt(a_pEv->position().toPoint());
    if (stIdx.isValid() && m_pProxy->IsDir(stIdx)) {
        sDstDir = m_pProxy->FilePath(stIdx);
    } else if (stIdx.isValid()) {
        sDstDir = QFileInfo(m_pProxy->FilePath(stIdx)).absolutePath();
    } else {
        sDstDir = m_sRootDir;
    }
    if (!IsUnderRoot(sDstDir)) {
        a_pEv->ignore();
        return;
    }
    const bool bInternal = (a_pEv->source() == this);
    const bool bMove = bInternal
        && !(a_pEv->modifiers() & Qt::ControlModifier);
    int iDone = 0, iFailed = 0;
    QStringList aFailed;
    for (const QUrl& stUrl : pMime->urls()) {
        const QString sSrc = stUrl.toLocalFile();
        if (sSrc.isEmpty()) {
            continue;
        }
        const QFileInfo stSrc(sSrc);
        if (!stSrc.exists()) {
            continue;
        }
        // 移动禁止落点=源自身/源子目录（目录拖入自己的子树）；原地复制忽略。
        const QString sSrcC = QDir::cleanPath(sSrc);
        const QString sDstC = QDir::cleanPath(sDstDir);
        if (bMove && (sDstC == sSrcC || sDstC.startsWith(sSrcC + QLatin1Char('/')))) {
            iFailed++;
            aFailed << stSrc.fileName();
            continue;
        }
        QString sDst = QDir(sDstDir).filePath(stSrc.fileName());
        if (QFileInfo::exists(sDst)) {
            if (QDir::cleanPath(sDst) == sSrcC) {
                continue;   // 复制到自身原位置：忽略
            }
            const QString sUnique = UniqueDstPath(sDst);
            if (sUnique.isEmpty()) {
                iFailed++;
                aFailed << stSrc.fileName();
                continue;
            }
            sDst = sUnique;
        }
        bool bOk = false;
        if (bMove) {
            bOk = QFile::rename(sSrc, sDst);
            if (!bOk) {
                // 跨盘 rename 失败 → 复制后删源（目录递归）。
                bOk = stSrc.isDir() ? CopyDirRecursively(sSrc, sDst)
                                    : QFile::copy(sSrc, sDst);
                if (bOk) {
                    if (stSrc.isDir()) {
                        QDir(sSrc).removeRecursively();
                    } else {
                        QFile::remove(sSrc);
                    }
                }
            }
        } else {
            bOk = stSrc.isDir() ? CopyDirRecursively(sSrc, sDst)
                                : QFile::copy(sSrc, sDst);
        }
        if (bOk) {
            ++iDone;
        } else {
            ++iFailed;
            aFailed << stSrc.fileName();
        }
    }
    if (iDone > 0) {
        a_pEv->acceptProposedAction();
    } else {
        a_pEv->ignore();
    }
    if (iFailed > 0) {
        QMessageBox::warning(this, QStringLiteral("拖放"),
            QStringLiteral("以下 %1 项未完成：%2\n（可能原因：目录移入自身子树、"
                            "同名冲突无法编号、文件被占用）")
                .arg(iFailed)
                .arg(aFailed.join(QStringLiteral("、"))));
    }
}

bool CProjectExplorerPanel::eventFilter(QObject* a_pObj, QEvent* a_pEv) {
    // 只接管树 viewport 的拖拽相关事件；其余默认处理。
    if (a_pObj != m_pTree->viewport()) {
        return QWidget::eventFilter(a_pObj, a_pEv);
    }
    switch (a_pEv->type()) {
    case QEvent::MouseButtonPress: {
        auto* pEv = static_cast<QMouseEvent*>(a_pEv);
        if (pEv->button() == Qt::LeftButton) {
            m_ptDragStart = pEv->position().toPoint();
            m_stDragIdx = QPersistentModelIndex(
                m_pTree->indexAt(pEv->position().toPoint()));
            m_bMaybeDrag = true;
        }
        return false;   // 交还 QTreeView 正常处理（选中/展开箭头等）
    }
    case QEvent::MouseMove: {
        auto* pEv = static_cast<QMouseEvent*>(a_pEv);
        if (m_bMaybeDrag && (pEv->buttons() & Qt::LeftButton)
            && m_stDragIdx.isValid()) {
            const QPoint stDelta = pEv->position().toPoint() - m_ptDragStart;
            if (stDelta.manhattanLength()
                >= QApplication::startDragDistance()) {
                m_bMaybeDrag = false;
                StartTreeDrag();
                return true;   // 本帧消耗，拖动不触发点击/展开
            }
        }
        if (!(pEv->buttons() & Qt::LeftButton)) {
            m_bMaybeDrag = false;
        }
        return false;
    }
    case QEvent::MouseButtonRelease:
        m_bMaybeDrag = false;
        return false;
    case QEvent::DragEnter: {
        auto* pEv = static_cast<QDragEnterEvent*>(a_pEv);
        if (!m_sRootDir.isEmpty() && pEv->mimeData()->hasUrls()) {
            pEv->acceptProposedAction();
        } else {
            pEv->ignore();
        }
        return true;   // 消耗，阻止 QAbstractItemView 默认 model drop
    }
    case QEvent::DragMove: {
        auto* pEv = static_cast<QDragMoveEvent*>(a_pEv);
        if (!m_sRootDir.isEmpty() && pEv->mimeData()->hasUrls()) {
            pEv->acceptProposedAction();
        } else {
            pEv->ignore();
        }
        return true;
    }
    case QEvent::DragLeave:
        return true;
    case QEvent::Drop: {
        auto* pEv = static_cast<QDropEvent*>(a_pEv);
        HandleTreeDrop(pEv);
        return true;
    }
    default:
        return false;
    }
}

bool CProjectExplorerPanel::IsTextFile(const QString& a_sAbsPath) {
    // 两级判定：扩展名白名单命中即文本（覆盖源码/文档/配置常见格式）；
    //   白名单外的扩展名嗅探文件头 4KB，含 \0 视为二进制，否则也按文本放行
    //   （无扩展名/新后缀的纯文本文件也能进内嵌 Tab）。
    static const QStringList aTextExts = {
        // 源码
        "cpp", "cc", "cxx", "c", "h", "hh", "hpp", "hxx", "inl", "cs", "java",
        "py", "pyw", "rb", "go", "rs", "js", "ts", "jsx", "tsx", "m", "mm",
        "sql", "lua", "pl", "php", "vue",
        // 标记/文档
        "md", "markdown", "txt", "log", "rtf", "csv", "tsv",
        // Web/样式
        "html", "htm", "xml", "css", "scss", "less", "svg", "json",
        // 配置/脚本/工程
        "ini", "cfg", "conf", "properties", "yml", "yaml", "toml",
        "bat", "cmd", "ps1", "sh", "cmake", "mak", "mk", "pro", "pri", "prf",
        "sln", "vcxproj", "filters", "props", "targets", "rc", "rc2", "def",
        "gitignore", "gitattributes", "editorconfig", "dockerfile", "npmrc"
    };
    const QString sSuffix = QFileInfo(a_sAbsPath).suffix().toLower();
    if (aTextExts.contains(sSuffix)) {
        return true;
    }
    // 白名单外：嗅探前 4KB（含 \0 → 二进制；读不到也按二进制处理，稳妥）。
    CFrwDevice stFile(a_sAbsPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray aHead = stFile.read(4096);
    return !aHead.isEmpty() && !aHead.contains('\0');
}

bool CProjectExplorerPanel::WriteFileClipboard(const QStringList& a_aPaths, bool a_bCut) {
    // 2026.09.19 写系统剪贴板（与资源管理器联动）：CF_HDROP（多文件宽字符双 NUL 结尾）
    //   + 「Preferred DropEffect」=DROPEFFECT_MOVE(2)/COPY(1)——资源管理器凭该标记决定
    //   粘贴时移动还是复制（无标记时默认复制）。每轮新建 HGLOBAL，被系统接管的不再
    //   释放；剪贴板被其他进程短暂占用时重试（Explorer 同款做法）。
    if (a_aPaths.isEmpty()) {
        return false;
    }
    // 拼接宽字符路径区：每项 NUL 结尾 + 末尾额外 NUL（CF_HDROP 双 NUL 终止约定）。
    QString sBuf;
    for (const QString& s : a_aPaths) {
        sBuf += QDir::toNativeSeparators(s) + QChar(u'\0');
    }
    sBuf += QChar(u'\0');
    const int iChars = int(sBuf.size());
    auto fnOnce = [&]() -> bool {
        HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE,
                                    sizeof(DROPFILES) + SIZE_T(iChars) * sizeof(wchar_t));
        HGLOBAL hEff  = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hDrop == nullptr || hEff == nullptr) {
            if (hDrop != nullptr) { GlobalFree(hDrop); }
            if (hEff  != nullptr) { GlobalFree(hEff); }
            return false;
        }
        bool bDropTaken = false, bEffTaken = false;
        auto fnCleanup = [&]() {
            if (!bDropTaken) { GlobalFree(hDrop); }
            if (!bEffTaken)  { GlobalFree(hEff); }
        };
        if (!EmptyClipboard()) {
            fnCleanup();
            return false;
        }
        // 先写效果标记，再写 CF_HDROP（主数据成功即整体成功；效果标记若先被接管
        //   而主数据失败，EmptyClipboard 会在下一轮清掉孤儿标记，无悬挂）。
        const UINT uEffFmt = RegisterClipboardFormatW(L"Preferred DropEffect");
        if (uEffFmt != 0) {
            void* pEff = GlobalLock(hEff);
            if (pEff == nullptr) {
                fnCleanup();
                return false;
            }
            *static_cast<DWORD*>(pEff) = a_bCut ? 2u /*DROPEFFECT_MOVE*/ : 1u /*COPY*/;
            GlobalUnlock(hEff);
            bEffTaken = SetClipboardData(uEffFmt, hEff) != nullptr;
            if (!bEffTaken) {
                fnCleanup();
                return false;
            }
        }
        void* pRaw = GlobalLock(hDrop);
        if (pRaw == nullptr) {
            fnCleanup();
            return false;
        }
        auto* pDF = static_cast<DROPFILES*>(pRaw);
        ZeroMemory(pDF, sizeof(DROPFILES));
        pDF->pFiles = sizeof(DROPFILES);
        pDF->fWide  = TRUE;
        memcpy(reinterpret_cast<BYTE*>(pDF) + sizeof(DROPFILES),
               sBuf.utf16(), SIZE_T(iChars) * sizeof(wchar_t));
        GlobalUnlock(hDrop);
        bDropTaken = SetClipboardData(CF_HDROP, hDrop) != nullptr;
        if (!bDropTaken) {
            fnCleanup();
            return false;
        }
        return true;   // 两个 HGLOBAL 均已被系统接管
    };
    // 剪贴板被其他进程短暂占用时重试（Explorer 同款做法；50ms 不够——多 Qt 应用
    //   共存/剪贴板管理器下实测需数百毫秒，20×25ms=500ms）。
    for (int iTry = 0; iTry < 20; ++iTry) {
        if (!OpenClipboard(nullptr)) {
            QThread::msleep(25);
            continue;
        }
        const bool bOk = fnOnce();
        CloseClipboard();
        if (bOk) {
            return true;
        }
        QThread::msleep(25);
    }
    return false;
}

QStringList CProjectExplorerPanel::ReadFileClipboard(bool* a_pbCut) {
    // 2026.09.19 读系统剪贴板（CF_HDROP：本程序与资源管理器剪切/复制均写入）：
    //   「Preferred DropEffect」=2 → 剪切（粘贴后移动并清板，Explorer 同款一次性语义）；
    //   其他/无标记 → 复制。读不到 CF_HDROP 返回空。
    QStringList aFiles;
    if (a_pbCut != nullptr) { *a_pbCut = false; }
    for (int iTry = 0; iTry < 10 && aFiles.isEmpty(); ++iTry) {
        if (!OpenClipboard(nullptr)) {
            QThread::msleep(5);
            continue;
        }
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            HANDLE h = GetClipboardData(CF_HDROP);
            if (h != nullptr) {
                auto* pDF = static_cast<const DROPFILES*>(GlobalLock(h));
                if (pDF != nullptr && pDF->fWide) {
                    const wchar_t* pwsz = reinterpret_cast<const wchar_t*>(
                        reinterpret_cast<const BYTE*>(pDF) + pDF->pFiles);
                    const SIZE_T cchMax = (GlobalSize(h) - pDF->pFiles) / sizeof(wchar_t);
                    QStringList aCollected;
                    QString sOne;
                    for (SIZE_T i = 0; i < cchMax; ++i) {
                        if (pwsz[i] == L'\0') {
                            if (sOne.isEmpty()) { break; }   // 连续 NUL=列表结束
                            // 资源管理器写入的是原生反斜杠路径——归一为 Qt 斜杠，
                            //   与树内路径/IsUnderRoot 前缀比较同口径。
                            aCollected << QDir::fromNativeSeparators(sOne);
                            sOne.clear();
                        } else {
                            sOne += QChar(pwsz[i]);
                        }
                    }
                    aFiles = aCollected;
                    GlobalUnlock(h);
                } else if (pDF != nullptr) {
                    GlobalUnlock(h);
                }
            }
            if (a_pbCut != nullptr && !aFiles.isEmpty()) {
                const UINT uEffFmt = RegisterClipboardFormatW(L"Preferred DropEffect");
                if (uEffFmt != 0 && IsClipboardFormatAvailable(uEffFmt)) {
                    HANDLE hEff = GetClipboardData(uEffFmt);
                    if (hEff != nullptr) {
                        auto* pEff = static_cast<const DWORD*>(GlobalLock(hEff));
                        if (pEff != nullptr) {
                            *a_pbCut = (*pEff == 2u);   // DROPEFFECT_MOVE
                            GlobalUnlock(hEff);
                        }
                    }
                }
            }
        }
        CloseClipboard();
        if (!aFiles.isEmpty()) { break; }
        // 空 CF_HDROP 或无格式：不再重试（仅 OpenClipboard 冲突重试）。
        if (IsClipboardFormatAvailable(CF_HDROP)) { break; }
    }
    return aFiles;
}

CProjectExplorerPanel::ETransfer CProjectExplorerPanel::TransferItem(
        const QString& a_sSrc, const QString& a_sDstDir, bool a_bMove,
        bool a_bOverwrite) {
    // 2026.09.19 剪切/粘贴单项落盘（与树内拖拽同语义，拖拽路径收敛于此）：
    //   同名自动 " (n)"；移动禁落源自身/子树；跨盘 rename 失败退复制+删源。
    // 2026.10.16 a_bOverwrite（粘贴同名弹窗「覆盖」档）：文件先删目标再落；
    //   目录合并——目标独有文件保留、同名文件被源替换（移动撞已存在目录 rename
    //   必败 → 自然退到 CopyDirRecursively(覆盖)+删源分支）。
    const QFileInfo stSrc(a_sSrc);
    if (!stSrc.exists()) {
        return XferSkip;
    }
    const QString sSrcC = QDir::cleanPath(a_sSrc);
    const QString sDstC = QDir::cleanPath(a_sDstDir);
    if (a_bMove && (sDstC == sSrcC || sDstC.startsWith(sSrcC + QLatin1Char('/')))) {
        return XferFail;   // 目录移入自身子树
    }
    QString sDst = QDir(a_sDstDir).filePath(stSrc.fileName());
    if (QFileInfo::exists(sDst)) {
        if (QDir::cleanPath(sDst) == sSrcC) {
            return XferSkip;   // 复制到自身原位置：忽略
        }
        if (a_bOverwrite) {
            // 覆盖档：文件删目标后按常规路径落盘；目录不删整树，留待下方合并覆盖。
            if (!stSrc.isDir() && !QFile::remove(sDst)) {
                return XferFail;
            }
        } else {
            const QString sUnique = UniqueDstPath(sDst);
            if (sUnique.isEmpty()) {
                return XferFail;
            }
            sDst = sUnique;
        }
    }
    bool bOk = false;
    if (a_bMove) {
        bOk = QFile::rename(a_sSrc, sDst);
        if (!bOk) {
            bOk = stSrc.isDir() ? CopyDirRecursively(a_sSrc, sDst, a_bOverwrite)
                                : QFile::copy(a_sSrc, sDst);
            if (bOk) {
                if (stSrc.isDir()) {
                    QDir(a_sSrc).removeRecursively();
                } else {
                    QFile::remove(a_sSrc);
                }
            }
        }
    } else {
        bOk = stSrc.isDir() ? CopyDirRecursively(a_sSrc, sDst, a_bOverwrite)
                            : QFile::copy(a_sSrc, sDst);
    }
    return bOk ? XferOk : XferFail;
}

void CProjectExplorerPanel::DoCut() {
    // 2026.09.19 剪切→系统剪贴板（CF_HDROP+Move 效果标记）：多选支持（与拖拽同选取），
    //   资源管理器/本树均可粘贴；写板失败时内部快照兑底（仅本树可粘）。
    const QStringList aPaths = SelectedAbsPaths();
    if (aPaths.isEmpty()) {
        return;
    }
    WriteFileClipboard(aPaths, /*a_bCut=*/true);
    m_aClipFiles = aPaths;
    m_bClipCut   = true;
}

void CProjectExplorerPanel::DoCopy() {
    // 2026.09.19 复制→系统剪贴板（CF_HDROP+Copy 效果标记）：多选支持，
    //   资源管理器/本树均可粘贴；写板失败时内部快照兑底。
    const QStringList aPaths = SelectedAbsPaths();
    if (aPaths.isEmpty()) {
        return;
    }
    WriteFileClipboard(aPaths, /*a_bCut=*/false);
    m_aClipFiles = aPaths;
    m_bClipCut   = false;
    
}

void CProjectExplorerPanel::DoPaste() {
    // 2026.09.19 粘贴读系统剪贴板（CF_HDROP）：本程序与 Windows 资源管理器剪切/复制
    //   的文件均可粘入工程（源可在工程外）；「Preferred DropEffect」=2 → 移动（剪切）
    //   否则复制。系统板无文件时退内部快照（写板失败时的兑底，仅本程序内有效）。
    //   逐项语义与拖拽一致：同名 " (n)" 编号、目录禁入自身子树、跨盘移动退复制+删源；
    //   剪切粘贴完成后清板（一次性，Explorer 同款）。
    // 2026.10.16 同名冲突先弹窗（覆盖/自动重命名/取消，见下方预扫段），静默编号
    //   不再是无提示的缺省结果。
    if (m_sRootDir.isEmpty()) {
        return;
    }
    bool bCut = false;
    QStringList aSrcs = ReadFileClipboard(&bCut);
    bool bFromSystem = !aSrcs.isEmpty();
    if (!bFromSystem) {
        if (m_aClipFiles.isEmpty()) {
            return;
        }
        aSrcs = m_aClipFiles;
        bCut  = m_bClipCut;
    }
    // 目标目录：选中节点为目录则粘贴其内，否则粘贴到选中节点所在目录（或工程根）。
    QString sDstDir;
    const QString sSel = SelectedAbsPath();
    if (!sSel.isEmpty() && QFileInfo(sSel).isDir()) {
        sDstDir = sSel;
    } else if (!sSel.isEmpty()) {
        sDstDir = QFileInfo(sSel).absolutePath();
    } else {
        sDstDir = m_sRootDir;
    }
    if (!IsUnderRoot(sDstDir)) {
        return;
    }
    // 2026.10.16 同名冲突预扫（落盘前整体过一遍）：命中才弹窗让用户定夺，选择对整批
    //   生效——「覆盖」替换目标同名文件（目录合并）、「自动重命名」保留两者（"名称
    //   (n)"，即原静默行为）、「取消」整批不动。无冲突保持零弹窗原体验。
    QStringList aConflicts;
    for (const QString& sSrc : aSrcs) {
        const QFileInfo stSrc(sSrc);
        if (!stSrc.exists()) {
            continue;
        }
        const QString sDst = QDir(sDstDir).filePath(stSrc.fileName());
        if (QFileInfo::exists(sDst)
            && QDir::cleanPath(sDst) != QDir::cleanPath(sSrc)) {
            aConflicts << stSrc.fileName();
        }
    }
    bool bOverwrite = false;
    if (!aConflicts.isEmpty()) {
        QString sList = aConflicts.join(QStringLiteral("、"));
        if (aConflicts.size() > 10) {
            sList = aConflicts.first(10).join(QStringLiteral("、"))
                  + QStringLiteral(" …等 %1 项").arg(aConflicts.size());
        }
        QMessageBox stBox(QMessageBox::Warning, QStringLiteral("粘贴确认"),
            QStringLiteral("目标位置已存在同名项目：\n%1\n\n"
                            "「覆盖」将替换已有文件（目录为合并覆盖）；\n"
                            "「自动重命名」保留两者（名称 (n)）。")
                .arg(sList),
            QMessageBox::NoButton, this);
        QPushButton* pBtnOver = stBox.addButton(QStringLiteral("覆盖(&O)"),
                                                QMessageBox::DestructiveRole);
        QPushButton* pBtnKeep = stBox.addButton(QStringLiteral("自动重命名(&R)"),
                                                QMessageBox::YesRole);
        stBox.addButton(QStringLiteral("取消(&C)"), QMessageBox::RejectRole);
        stBox.exec();
        if (stBox.clickedButton() == pBtnOver) {
            bOverwrite = true;
        } else if (stBox.clickedButton() != pBtnKeep) {
            return;   // 取消/关闭：整批不落盘（预扫先行，此时尚未动任何文件）
        }
    }
    int iDone = 0, iFailed = 0;
    QStringList aFailed;
    for (const QString& sSrc : aSrcs) {
        const ETransfer eRet = TransferItem(sSrc, sDstDir, bCut, bOverwrite);
        if (eRet == XferOk) {
            ++iDone;
        } else if (eRet == XferFail) {
            ++iFailed;
            aFailed << QFileInfo(sSrc).fileName();
        }
    }
    if (bCut && iDone > 0 && iFailed == 0) {
        // 剪切整体落地：清系统板与内部快照（一次性语义；部分失败保留供重试）。
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        }
        m_aClipFiles.clear();
        m_bClipCut = false;
    } else if (!bFromSystem && !bCut) {
        m_aClipFiles.clear();   // 内部复制快照用后即清（系统板才是权威）
    }
    if (iDone == 0 && iFailed == 0) {
        QMessageBox::information(this, QStringLiteral("粘贴"),
            QStringLiteral("剪贴板中没有可粘贴的文件（源不存在或均为原位置）。"));
    } else if (iFailed > 0) {
        QMessageBox::warning(this, QStringLiteral("粘贴"),
            QStringLiteral("已粘贴 %1 项；以下 %2 项未完成：%3\n（可能原因：目录移入自身子树、"
                            "同名冲突无法编号、文件被占用）")
                .arg(iDone)
                .arg(iFailed)
                .arg(aFailed.join(QStringLiteral("、"))));
    }
}

void CProjectExplorerPanel::DoDelete() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    const QString sName = QFileInfo(sPath).fileName();
    // 二次确认（NFR-7）。
    const auto eRet = QMessageBox::question(
        this, QStringLiteral("删除"),
        QStringLiteral("确认删除「%1」？该操作不可撤销。").arg(sName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (eRet != QMessageBox::Yes) {
        return;
    }
    if (QFileInfo(sPath).isDir()) {
        QDir(sPath).removeRecursively();
    } else {
        QFile::remove(sPath);
    }
}

QString CProjectExplorerPanel::CreateNewFolderAt(const QString& a_sTargetDir) {
    // 2026.09.26 v3.1.247 新建文件夹：目标目录显式传入（空=按选中推导：选中目录自身/
    //   选中文件父目录/无选中=工程根，多根回退首个根）；默认名「新建文件夹」，重名自动
    //   " (n)"（Windows 口径，用户改不成名也不冲突）。公开供回归直调。
    QString sDir = a_sTargetDir;
    if (sDir.isEmpty()) {
        const QString sSel = SelectedAbsPath();
        if (!sSel.isEmpty() && IsUnderRoot(sSel)) {
            sDir = QFileInfo(sSel).isDir() ? sSel : QFileInfo(sSel).absolutePath();
        } else if (!m_sRootDir.isEmpty()) {
            sDir = m_sRootDir;
        } else {
            const QStringList aRoots = RootPaths();
            sDir = aRoots.isEmpty() ? QString() : aRoots.first();
        }
    }
    if (sDir.isEmpty() || !QDir(sDir).exists()) {
        return QString();
    }
    QString sName = QStringLiteral("新建文件夹");
    int iSeq = 2;
    while (QDir(sDir).exists(sName)) {
        sName = QStringLiteral("%1 (%2)").arg(QStringLiteral("新建文件夹"))
                                            .arg(iSeq++);
    }
    if (!QDir(sDir).mkdir(sName)) {
        return QString();
    }
    return QDir(sDir).filePath(sName);
}

void CProjectExplorerPanel::StartNewFolder() {
    // 右键「新建文件夹」入口：创建后选中并进入行内改名（Windows 同款建完即命名；
    //   Esc/空名提交 → QFileSystemModel 对空名不动作，保留默认名，不残留空态）。
    const QString sNew = CreateNewFolderAt(QString());
    if (sNew.isEmpty()) {
        return;
    }
    const QModelIndex stSrc = m_pModel->index(sNew);
    if (!stSrc.isValid()) {
        return;
    }
    const QModelIndex stIdx = m_pProxy->mapFromSource(stSrc);
    if (!stIdx.isValid()) {
        return;
    }
    m_pTree->scrollTo(stIdx);
    m_pTree->setCurrentIndex(stIdx);
    m_pTree->edit(stIdx);
}

void CProjectExplorerPanel::StartProperties(const QString& a_sPath)
{
    // 2026.10.02 属性（用户需求：优先 Windows 资源管理器原生属性框）：
    //   SHObjectProperties(SHOP_FILEPATH) —— 资源管理器同源多页签属性框（常规/
    //   安全/详细信息等，文件/目录同接口），shell 负责页签与内容；返回 FALSE 时
    //   兑底自绘属性对话框（BuildPropertiesDialog：名称/类型/位置/大小/三个时间/
    //   只读隐藏写回）。
    if (a_sPath.trimmed().isEmpty()) {
        return;
    }
    const QFileInfo stFi(a_sPath);
    if (!stFi.exists()) {
        QMessageBox::warning(this, QStringLiteral("属性"),
            QStringLiteral("路径不存在：%1").arg(a_sPath));
        return;
    }
    const QString sNative = QDir::toNativeSeparators(
        QDir::cleanPath(stFi.absoluteFilePath()));
    HWND hOwner = (window() != nullptr && window()->winId() != 0)
        ? reinterpret_cast<HWND>(window()->winId()) : nullptr;
    if (SHObjectProperties(hOwner, SHOP_FILEPATH,
                           (LPCWSTR)sNative.utf16(), nullptr) != FALSE) {
        return;   // 原生属性框已弹（shell 自管生命周期）
    }
    // 兑底：自绘属性框（11pt；目录递归统计文件数/大小，大目录同步耗时可接受——
    //   Windows 资源管理器对目录同样要现场统计）。
    QDialog* pDlg = BuildPropertiesDialog(stFi.absoluteFilePath(), this);
    pDlg->exec();
    delete pDlg;
}

QDialog* CProjectExplorerPanel::BuildPropertiesDialog(const QString& a_sPath,
                                                       QWidget* a_pParent)
{
    // 兑底属性框（2026.10.02）：QFormLayout 字段表 + 只读/隐藏勾选写回
    //   （GetFileAttributesW/SetFileAttributesW）。公开静态供测试：返回未显示的
    //   对话框实例，测试按 objectName 断言字段填充（propLblName/propLblSize/
    //   propLblType/propLblLocation/propLblModified/propChkReadOnly/propChkHidden）。
    const QFileInfo stFi(a_sPath);
    QDialog* pDlg = new QDialog(a_pParent);
    pDlg->setObjectName(QStringLiteral("propDialog"));
    pDlg->setWindowTitle(QStringLiteral("%1 属性").arg(stFi.fileName()));
    QFont fnt11 = pDlg->font();
    fnt11.setPointSize(11);   // 新 UI 11pt 缺省
    pDlg->setFont(fnt11);
    auto* pForm = new QFormLayout(pDlg);
    pForm->setHorizontalSpacing(16);
    pForm->setVerticalSpacing(8);

    // 名称行（图标 + 文件名）
    QFileIconProvider stProv;
    QPixmap pmIcon = stProv.icon(stFi).pixmap(32, 32);
    auto* pLblName = new QLabel(
        stFi.fileName().isEmpty() ? stFi.absoluteFilePath() : stFi.fileName(),
        pDlg);
    pLblName->setObjectName(QStringLiteral("propLblName"));
    if (!pmIcon.isNull()) {
        pLblName->setPixmap(pmIcon);
        pLblName->setToolTip(stFi.fileName());
    }
    pForm->addRow(QStringLiteral("名称："), pLblName);

    // 类型
    QString sType;
    if (stFi.isDir()) {
        sType = QStringLiteral("文件夹");
    } else if (stFi.suffix().isEmpty()) {
        sType = QStringLiteral("文件");
    } else {
        sType = QStringLiteral(".%1 文件").arg(stFi.suffix().toLower());
    }
    auto* pLblType = new QLabel(sType, pDlg);
    pLblType->setObjectName(QStringLiteral("propLblType"));
    pForm->addRow(QStringLiteral("类型："), pLblType);

    // 位置（可选中复制）
    auto* pLblLoc = new QLabel(stFi.absolutePath(), pDlg);
    pLblLoc->setObjectName(QStringLiteral("propLblLocation"));
    pLblLoc->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pForm->addRow(QStringLiteral("位置："), pLblLoc);

    // 大小（目录递归统计：文件数/文件夹数/总字节，千分位）
    QString sSize;
    if (stFi.isDir()) {
        qint64 iFiles = 0, iDirs = 0, iBytes = 0;
        QDirIterator it(stFi.absoluteFilePath(), QDir::Files | QDir::Dirs
                         | QDir::NoSymLinks | QDir::Hidden | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo e(it.next());
            if (e.isDir()) {
                ++iDirs;
            } else {
                ++iFiles;
                iBytes += e.size();
            }
        }
        sSize = QStringLiteral("%1 个文件、%2 个文件夹，共 %3 字节")
                    .arg(QLocale().toString(iFiles))
                    .arg(QLocale().toString(iDirs))
                    .arg(QLocale().toString(iBytes));
    } else {
        sSize = QStringLiteral("%1 字节")
                    .arg(QLocale().toString(stFi.size()));
    }
    auto* pLblSize = new QLabel(sSize, pDlg);
    pLblSize->setObjectName(QStringLiteral("propLblSize"));
    pForm->addRow(QStringLiteral("大小："), pLblSize);

    // 时间（修改/创建/访问）
    const QString sFmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    auto* pLblMod = new QLabel(stFi.lastModified().toString(sFmt), pDlg);
    pLblMod->setObjectName(QStringLiteral("propLblModified"));
    pForm->addRow(QStringLiteral("修改时间："), pLblMod);
    auto* pLblCrt = new QLabel(stFi.birthTime().toString(sFmt), pDlg);
    pLblCrt->setObjectName(QStringLiteral("propLblCreated"));
    pForm->addRow(QStringLiteral("创建时间："), pLblCrt);

    // 只读/隐藏（勾选写回 SetFileAttributesW）
    const DWORD dwAttr = GetFileAttributesW(
        (LPCWSTR)QDir::toNativeSeparators(
            stFi.absoluteFilePath()).utf16());
    const bool bBad = (dwAttr == INVALID_FILE_ATTRIBUTES);
    auto* pChkRO = new QCheckBox(QStringLiteral("只读"), pDlg);
    pChkRO->setObjectName(QStringLiteral("propChkReadOnly"));
    pChkRO->setChecked(!bBad && (dwAttr & FILE_ATTRIBUTE_READONLY) != 0);
    auto* pChkHid = new QCheckBox(QStringLiteral("隐藏"), pDlg);
    pChkHid->setObjectName(QStringLiteral("propChkHidden"));
    pChkHid->setChecked(!bBad && (dwAttr & FILE_ATTRIBUTE_HIDDEN) != 0);
    auto* pRowAttr = new QWidget(pDlg);
    auto* pLayAttr = new QHBoxLayout(pRowAttr);
    pLayAttr->setContentsMargins(0, 0, 0, 0);
    pLayAttr->addWidget(pChkRO);
    pLayAttr->addWidget(pChkHid);
    pLayAttr->addStretch(1);
    pForm->addRow(QStringLiteral("属性："), pRowAttr);

    // 确定按钮（accepted 时写回勾选变化）
    auto* pBtnOk = new QPushButton(QStringLiteral("确定"), pDlg);
    auto* pLayBtn = new QHBoxLayout();
    pLayBtn->addStretch(1);
    pLayBtn->addWidget(pBtnOk);
    pForm->addRow(QString(), pLayBtn);
    QObject::connect(pBtnOk, &QPushButton::clicked, pDlg, [pDlg, stFi, pChkRO, pChkHid, bBad, dwAttr]() {
        // 勾选有变化才写回；保持其余属性位不变（按原值增删两位）。
        if (!bBad) {
            const bool bWantRO = pChkRO->isChecked();
            const bool bWantHid = pChkHid->isChecked();
            const bool bHadRO = (dwAttr & FILE_ATTRIBUTE_READONLY) != 0;
            const bool bHadHid = (dwAttr & FILE_ATTRIBUTE_HIDDEN) != 0;
            if (bWantRO != bHadRO || bWantHid != bHadHid) {
                DWORD dwNew = dwAttr;
                if (bWantRO) dwNew |= FILE_ATTRIBUTE_READONLY;
                else         dwNew &= ~DWORD(FILE_ATTRIBUTE_READONLY);
                if (bWantHid) dwNew |= FILE_ATTRIBUTE_HIDDEN;
                else          dwNew &= ~DWORD(FILE_ATTRIBUTE_HIDDEN);
                const QString sNative = QDir::toNativeSeparators(
                    stFi.absoluteFilePath());
                if (!SetFileAttributesW((LPCWSTR)sNative.utf16(), dwNew)) {
                    QMessageBox::warning(pDlg, QStringLiteral("属性"),
                        QStringLiteral("写回属性失败（错误码 %1）")
                            .arg(GetLastError()));
                    return;   // 不关窗：用户可重试或取消
                }
            }
        }
        pDlg->accept();
    });
    pDlg->setMinimumWidth(460);
    return pDlg;
}

void CProjectExplorerPanel::DoRename() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    const QFileInfo stFi(sPath);
    const QString sNew = QInputDialog::getText(
        this, QStringLiteral("改名"),
        QStringLiteral("新名称："), QLineEdit::Normal, stFi.fileName());
    if (sNew.trimmed().isEmpty() || sNew.trimmed() == stFi.fileName()) {
        return;
    }
    const QString sDst = QDir(stFi.absolutePath()).filePath(sNew.trimmed());
    if (!IsUnderRoot(sDst)) {
        return;
    }
    if (QFileInfo::exists(sDst)) {
        QMessageBox::warning(this, QStringLiteral("改名"),
            QStringLiteral("目标已存在：%1").arg(sNew.trimmed()));
        return;
    }
    QFile::rename(sPath, sDst);
}

void CProjectExplorerPanel::CopyPath() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(sPath));
}

void CProjectExplorerPanel::CopyRelativePath() {
    // 2026.09.21 多根口径：根节点→直接复制名称（旧版对根给出 "."，无意义）；
    //   普通节点→相对「所在根」的路径（多根嵌套时取最深包含根）。
    const QModelIndex stIdx = m_pTree->currentIndex();
    if (!stIdx.isValid() || m_sRootDir.isEmpty()) {
        return;
    }
    const QString sPath = m_pProxy->FilePath(stIdx);
    if (sPath.isEmpty() || !IsUnderRoot(sPath)) {
        return;
    }
    if (!stIdx.parent().isValid()) {
        QGuiApplication::clipboard()->setText(QFileInfo(sPath).fileName());
        return;
    }
    const QString sRoot = ContainingRoot(sPath);
    if (sRoot.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(
        QDir::toNativeSeparators(QDir(sRoot).relativeFilePath(sPath)));
}

void CProjectExplorerPanel::AddToChat() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    // Q1 默认：注入路径（kind="path"）到 adhoc 助手输入框。
    emit sigAddToChat(QDir::toNativeSeparators(sPath), QStringLiteral("path"));
}

void CProjectExplorerPanel::OpenInExplorer() {
    // 用 explorer.exe 打开：目录直接打开该目录；文件打开所在目录并选中该文件。
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    const QString sNative = QDir::toNativeSeparators(sPath);
    if (QFileInfo(sPath).isDir()) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                QStringList(sNative));
        return;
    }
    // 2026.09.25 修「打开的目录不对」：文件分支改 ShellExecuteW（原 QProcess 两参式
    //   对含空格路径整体加引号 → explorer.exe "/select,E:\a b\f.txt" —— /select, 被
    //   包进引号后 explorer 解析失败，退回打开默认目录（实测=我的文档）。改走
    //   ShellExecuteW 自控参数串 /select,\"path\"（引号只包路径，PowerShell 实证：
    //   正确打开所在目录且选中该文件）。
    const QString sParams = QStringLiteral("/select,\"%1\"").arg(sNative);
    const HINSTANCE hInst = ShellExecuteW(
        nullptr, L"open", L"explorer.exe",
        reinterpret_cast<const wchar_t*>(sParams.utf16()), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        qWarning() << "[Explorer] ShellExecute(/select) 失败 rc="
                   << reinterpret_cast<INT_PTR>(hInst) << "path=" << sNative;
    }
}

// ===========================================================================
// 2026.09.21 多根节点（关联项目）：三按钮 + 增删/移动 + 持久化。
// ===========================================================================
int CProjectExplorerPanel::RootCount() const {
    return m_pProxy != nullptr ? m_pProxy->RootCount() : 0;
}

QString CProjectExplorerPanel::RootPathAt(int a_iRow) const {
    return m_pProxy != nullptr ? m_pProxy->RootPathAt(a_iRow) : QString();
}

QStringList CProjectExplorerPanel::RootPaths() const {
    return m_pProxy != nullptr ? m_pProxy->RootPaths() : QStringList();
}

QAbstractItemModel* CProjectExplorerPanel::ExplorerModel() const {
    return m_pProxy;
}

int CProjectExplorerPanel::SelectedRootRow() const {
    if (m_pTree == nullptr || m_pProxy == nullptr) {
        return -1;
    }
    const QModelIndex stIdx = m_pTree->currentIndex();
    if (!stIdx.isValid() || stIdx.parent().isValid()
        || stIdx.row() >= m_pProxy->RootCount()) {
        return -1;
    }
    return stIdx.row();
}

bool CProjectExplorerPanel::SelectRoot(int a_iRow) {
    if (m_pProxy == nullptr || a_iRow < 0 || a_iRow >= m_pProxy->RootCount()) {
        return false;
    }
    const QModelIndex stIdx = m_pProxy->index(a_iRow, 0);
    m_pTree->setCurrentIndex(stIdx);
    m_pTree->scrollTo(stIdx, QAbstractItemView::PositionAtCenter);
    UpdateRootButtons();
    return true;
}

bool CProjectExplorerPanel::SelectPath(const QString& a_sAbsPath) {
    if (m_pProxy == nullptr) {
        return false;
    }
    const QString sClean = QDir::cleanPath(QDir::fromNativeSeparators(a_sAbsPath));
    for (int i = 0; i < m_pProxy->RootCount(); ++i) {
        if (QString::compare(QDir::cleanPath(m_pProxy->RootPathAt(i)), sClean,
                             Qt::CaseInsensitive) == 0) {
            return SelectRoot(i);
        }
    }
    // 递归搜已加载子树（目录先 fetchMore 再扫；未加载完由调用方重试）。
    QVector<QModelIndex> aStack;
    for (int i = 0; i < m_pProxy->rowCount(); ++i) {
        aStack << m_pProxy->index(i, 0);
    }
    int iGuard = 0;
    while (!aStack.isEmpty()) {
        const QModelIndex stCur = aStack.takeLast();
        if (m_pProxy->IsDir(stCur)) {
            m_pProxy->fetchMore(stCur);
        }
        const int iRows = m_pProxy->rowCount(stCur);
        for (int r = 0; r < iRows; ++r) {
            const QModelIndex stChild = m_pProxy->index(r, 0, stCur);
            const QString sP = QDir::cleanPath(m_pProxy->FilePath(stChild));
            if (QString::compare(sP, sClean, Qt::CaseInsensitive) == 0) {
                m_pTree->setCurrentIndex(stChild);
                return true;
            }
            if (m_pProxy->IsDir(stChild)) {
                aStack << stChild;
            }
        }
        if (++iGuard > 100000) {
            return false;   // 防坏档/符号链接环扫描失控
        }
    }
    return false;
}

void CProjectExplorerPanel::OnAddRootClicked() {
    if (m_sRootDir.isEmpty()) {
        return;
    }
    // 缺省路径=当前工程目录（用户需求：允许查看关联项目情况）；原生目录选择对话框。
    const QString sDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("添加根节点（选择关联项目目录）"), m_sRootDir);
    if (!sDir.isEmpty()) {
        AddExtraRoot(sDir);
    }
}

void CProjectExplorerPanel::OnMoveRootUp() {
    MoveSelectedRoot(-1);
}

void CProjectExplorerPanel::OnMoveRootDown() {
    MoveSelectedRoot(+1);
}

void CProjectExplorerPanel::OnProxyRootsChanged() {
    // 代理侧外部变化（磁盘删根剪除/改名同步）→ 持久化收敛 + 按钮态。
    SaveExplorerState();
    UpdateRootButtons();
}

// —— 展开态自愈（2026.09.22，勿回退）——
// 背景：多根代理层 v3.0.211 已把源延迟重排改为规范转发，但目录被外部删建/重命名后
//   新节点的展开态仍会丢（QPersistentModelIndex 不跨删建存活，Qt 语义如此）——Agent
//   任务频繁写盘/删建目录时表现为「文件夹莫名其妙的自动折叠」。会话内记住展开路径、
//   结构事件后重放，行一回来就重新展开，无论哪个源头杀掉展开态都自愈。
void CProjectExplorerPanel::RememberExpand(const QModelIndex& a_stIdx, bool a_bExpanded) {
    if (m_pProxy == nullptr || !a_stIdx.isValid()) {
        return;
    }
    const QString sPath = m_pProxy->FilePath(a_stIdx);
    if (sPath.isEmpty()) {
        return;
    }
    const QString sClean = QDir::cleanPath(sPath);
    if (a_bExpanded) {
        m_hExpandPaths.insert(sClean.toLower(), sClean);
    } else {
        m_hExpandPaths.remove(sClean.toLower());
    }
}

void CProjectExplorerPanel::ReapplyExpandedPaths() {
    if (m_pTree == nullptr || m_pProxy == nullptr || m_pModel == nullptr) {
        return;
    }
    for (auto it = m_hExpandPaths.constBegin(); it != m_hExpandPaths.constEnd(); ++it) {
        const QModelIndex stSrc = m_pModel->index(it.value());
        if (!stSrc.isValid()) {
            continue;   // 目录尚未重建/未列出：等下一轮 directoryLoaded 再试
        }
        const QModelIndex stP = m_pProxy->mapFromSource(stSrc);
        if (stP.isValid() && !m_pTree->isExpanded(stP)) {
            m_pTree->expand(stP);   // 触发 expanded 信号 → RememberExpand 幂等回写
        }
    }
    // 容量治理：极端工程无界增长兑底（600 目录足够，超出整体清零，仅损失记忆）。
    if (m_hExpandPaths.size() > 600) {
        m_hExpandPaths.clear();
    }
}

// —— 2026.09.23 手动刷新（右键「刷新」）——
// 背景（Qt 6.8 qfilesystemmodel.cpp / qfileinfogatherer.cpp 源码实证）：目录内容
//   同步完全依赖模型内部 QFileSystemWatcher 的真实事件——一次事件 →
//   QFileInfoGatherer::list() 全量重列 → updates 信号补齐新增/剔除已删。Windows
//   ReadDirectoryChangesW 通知缓冲固定，Agent 批量落盘/杀软实时扫描时会溢出丢事件，
//   树即停滞直到下一个事件；Qt6 也无公开的强制重列 API（fetchMore 仅对未列过的目录
//   生效、setRootPath 同路径早退）。手动刷新=对目标目录制造一次无害事件强制重列。
void CProjectExplorerPanel::NudgeDirWatch(const QString& a_sDir) {
    const QString sTmp = QDir(a_sDir).filePath(QStringLiteral(".autocode-refresh.tmp"));
    QFile::remove(sTmp);   // 上次异常中断残留兑底
    QFile stFile(sTmp);
    if (!stFile.open(QIODevice::WriteOnly)) {
        return;   // 目录只读/被锁：造不出事件，放弃（下一事件自然补齐）
    }
    stFile.close();
    // 隐藏属性：指纹扫描（排除隐藏文件/目录）不收（QFile 无 setHidden，用 Win32）。
    SetFileAttributesW(reinterpret_cast<const wchar_t*>(sTmp.utf16()),
                       FILE_ATTRIBUTE_HIDDEN);
    QFile::remove(sTmp);
    // 创建+删除背靠背，两次事件各触发一次全量重列；通常两次重列都在删除后执行，
    // 临时行不闪现（.tmp 亦不在代码备份白名单）。
}

QStringList CProjectExplorerPanel::RefreshTargets() const {
    QStringList aDirs;
    const QString sSel = SelectedAbsPath();
    if (!sSel.isEmpty()) {
        // 选中目录→自身；选中文件→父目录（文件变化由父目录重列带出）。
        const QFileInfo stInfo(sSel);
        aDirs << QDir::cleanPath(stInfo.isDir() ? sSel : stInfo.absolutePath());
    } else {
        aDirs = RootPaths();   // 无选中：全根
    }
    // 已展开目录=watcher 实际监视面，一并重列（子目录层级同样可能停滞——根目录的
    //   重列只同步顶层，不会深入已展开子目录）。
    QSet<QString> aSeen;
    for (const QString& s : aDirs) {
        aSeen.insert(QDir::fromNativeSeparators(s).toLower());
    }
    for (auto it = m_hExpandPaths.constBegin(); it != m_hExpandPaths.constEnd(); ++it) {
        const QString sKey = QDir::fromNativeSeparators(it.value()).toLower();
        if (aSeen.contains(sKey)) {
            continue;
        }
        for (const QString& sT : aDirs) {
            const QString sTl = QDir::fromNativeSeparators(sT).toLower();
            if (sKey.startsWith(sTl + QLatin1Char('/'))) {
                aSeen.insert(sKey);
                aDirs << it.value();
                break;
            }
        }
    }
    return aDirs;
}

void CProjectExplorerPanel::RefreshSelected() {
    if (m_pModel == nullptr) {
        return;
    }
    const QStringList aDirs = RefreshTargets();
    for (const QString& s : aDirs) {
        if (QFileInfo(s).isDir()) {
            NudgeDirWatch(s);
        }
    }
    // 兜底：重列新增行回来后按记住的路径重放展开
    //   （rowsInserted/directoryLoaded 链亦会触发，双保险）。
    m_tExpandSync.start();
}

bool CProjectExplorerPanel::AddExtraRoot(const QString& a_sDir) {
    if (m_sRootDir.isEmpty() || m_pProxy == nullptr) {
        return false;
    }
    const QString sClean = QDir::cleanPath(
        QDir::fromNativeSeparators(a_sDir.trimmed()));
    if (sClean.isEmpty() || !QFileInfo(sClean).isDir()) {
        return false;
    }
    const QStringList aNow = RootPaths();
    for (const QString& s : aNow) {
        if (QString::compare(s, sClean, Qt::CaseInsensitive) == 0) {
            return false;   // 已存在（含工程根）
        }
        // 嵌套根拒绝（任一方向包含）：同一源节点会同时出现在两个根子树，代理的
        //   QModelIndex 等值判定（不含 parent）会把两处索引视为同一节点 → 视图
        //   持久索引串位/堆损坏（2026.09.21 实测 fail-fast）。嵌套目录本就在
        //   父根树内可见，无重复添加的必要。
        if (s.startsWith(sClean + QLatin1Char('/'), Qt::CaseInsensitive)
            || sClean.startsWith(s + QLatin1Char('/'), Qt::CaseInsensitive)) {
            return false;
        }
    }
    if (!m_pProxy->AppendRoot(sClean)) {
        return false;
    }
    SaveExplorerState();
    const int iRow = m_pProxy->RootCount() - 1;
    SelectRoot(iRow);
    m_pTree->expand(m_pProxy->index(iRow, 0));
    UpdateRootButtons();
    emit sigExplorerRootsChanged();
    return true;
}

bool CProjectExplorerPanel::RemoveExtraRoot(const QString& a_sDir) {
    if (m_pProxy == nullptr || m_sRootDir.isEmpty()) {
        return false;
    }
    const QString sClean = QDir::cleanPath(
        QDir::fromNativeSeparators(a_sDir.trimmed()));
    if (QString::compare(sClean, QDir::cleanPath(m_sRootDir),
                         Qt::CaseInsensitive) == 0) {
        return false;   // 工程根不可移除（随工程开关存在）
    }
    const QStringList aNow = RootPaths();
    for (int i = 0; i < aNow.size(); ++i) {
        if (QString::compare(aNow.at(i), sClean, Qt::CaseInsensitive) == 0) {
            m_pProxy->RemoveRootAt(i);
            SaveExplorerState();
            UpdateRootButtons();
            emit sigExplorerRootsChanged();
            return true;
        }
    }
    return false;
}

bool CProjectExplorerPanel::MoveSelectedRoot(int a_iDelta) {
    if (m_pProxy == nullptr) {
        return false;
    }
    const int iRow = SelectedRootRow();
    if (iRow < 0) {
        return false;   // 未选根行（或选中普通节点）：仅对根节点有效
    }
    const int iTo = iRow + a_iDelta;
    if (!m_pProxy->MoveRoot(iRow, iTo)) {
        return false;
    }
    SaveExplorerState();
    SelectRoot(iTo);
    UpdateRootButtons();
    emit sigExplorerRootsChanged();
    return true;
}

void CProjectExplorerPanel::UpdateRootButtons() {
    const int iRow = SelectedRootRow();
    const int n = RootCount();
    if (m_pBtnAddRoot != nullptr) {
        m_pBtnAddRoot->setEnabled(!m_sRootDir.isEmpty());
    }
    if (m_pBtnRootUp != nullptr) {
        m_pBtnRootUp->setEnabled(iRow > 0);
    }
    if (m_pBtnRootDown != nullptr) {
        m_pBtnRootDown->setEnabled(iRow >= 0 && iRow < n - 1);
    }
}

QString CProjectExplorerPanel::ExplorerStatePath() const {
    if (m_sRootDir.isEmpty()) {
        return QString();
    }
    return QDir(m_sRootDir).filePath(QStringLiteral(".autodev/explorer.json"));
}

void CProjectExplorerPanel::SaveExplorerState() {
    // {version:1, roots:[有序全根]}（含工程根——顺序由用户排列）；UTF-8。
    //   工程级持久化：关联根随工程走（不同工程的关联项目集合互不干扰）；
    //   .autodev 由工程在席锁保护（同其它工程态文件）。
    if (m_sRootDir.isEmpty() || m_pProxy == nullptr) {
        return;
    }
    const QString sPath = ExplorerStatePath();
    if (sPath.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(sPath).absolutePath());
    QJsonObject oRoot;
    oRoot.insert(QStringLiteral("version"), 1);
    oRoot.insert(QStringLiteral("roots"), QJsonArray::fromStringList(RootPaths()));
    QFile fOut(sPath);
    if (!fOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[Explorer] explorer.json 写入失败:" << sPath;
        return;
    }
    fOut.write(QJsonDocument(oRoot).toJson(QJsonDocument::Indented));
}

QStringList CProjectExplorerPanel::LoadExplorerState() const {
    const QString sPath = ExplorerStatePath();
    if (sPath.isEmpty()) {
        return QStringList();
    }
    QFile fIn(sPath);
    if (!fIn.open(QIODevice::ReadOnly)) {
        return QStringList();
    }
    const QJsonDocument oDoc = QJsonDocument::fromJson(fIn.readAll());
    if (!oDoc.isObject()) {
        return QStringList();
    }
    QStringList aRoots;
    const QJsonArray aArr = oDoc.object().value(QStringLiteral("roots")).toArray();
    for (const QJsonValue& v : aArr) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) {
            aRoots << s;
        }
    }
    return aRoots;
}

// 展开命令模板里的 %VAR% 环境变量引用（定义见文末）。
static QString ExpandEnvVars(const QString& a_sCmd);

// 2026.09.21 多根代理模型（Q_OBJECT 内嵌 cpp 类）：AUTOMOC 需尾引。
#include "CProjectExplorerPanel.moc"

// 2026.09.17 程序图标：路径（含环境变量引用先展开）→ 系统 shell 图标链路
//（QFileIconProvider 天然多尺寸 16/32/48，exe/ico 直取）；静态缓存防菜单反复
// 打开时重复抽取（键=路径|索引；索引仅作缓存区分，Provider 取主图标——对
// exe 主图标场景等价，dll 资源位罕见）。
static QIcon ProgramIcon(const QString& a_sPath) {
    const QString sNorm = QDir::fromNativeSeparators(ExpandEnvVars(a_sPath).trimmed());
    if (sNorm.isEmpty() || !QFileInfo::exists(sNorm)) {
        return QIcon();
    }
    static QHash<QString, QIcon> s_aCache;
    auto it = s_aCache.constFind(sNorm);
    if (it != s_aCache.constEnd()) {
        return it.value();
    }
    static QFileIconProvider s_oProv;
    const QIcon oIcon = s_oProv.icon(QFileInfo(sNorm));
    s_aCache.insert(sNorm, oIcon);
    return oIcon;
}

// ---------------------------------------------------------------------------
// 2026.08.21 「打开方式」子菜单：HKCR 注册表枚举该类型注册过的程序。
// ---------------------------------------------------------------------------
// QSettings(NativeFormat) 读注册表键的默认值（Qt 以 "." 为默认值键名；部分键写
// 在显式 "Default" 名下，两处兜底）。
static QString RegDefaultValue(QSettings& a_stKey) {
    QString sVal = a_stKey.value(QStringLiteral(".")).toString();
    if (sVal.isEmpty()) {
        sVal = a_stKey.value(QStringLiteral("Default")).toString();
    }
    return sVal.trimmed();
}

// 从 verb/command 模板提取 exe 基名做展示名兜底（MuiVerb 是间接资源串时取不到
// 明文，用程序名比 verb 名（open/edit）更有辨识度）。
static QString ExeBaseFromCommand(const QString& a_sCmd) {
    const QStringList aParts = QProcess::splitCommand(a_sCmd);
    if (aParts.isEmpty()) {
        return QString();
    }
    return QFileInfo(aParts.first()).completeBaseName();
}

// 展开某 ProgID（HKCR\<ProgId>\shell\<verb>\command）下全部可用 verb 为菜单项。
static void AddProgIdVerbs(QMenu* a_pMenu, const QString& a_sProgId,
                           QSet<QString>* a_pSeenCmds) {
    const QString sShellKey = QStringLiteral("HKEY_CLASSES_ROOT\\%1\\shell")
                                  .arg(a_sProgId);
    QSettings stShell(sShellKey, QSettings::NativeFormat);
    const QStringList aVerbs = stShell.childGroups();
    if (aVerbs.isEmpty()) {
        return;
    }
    bool bAddedAny = false;
    for (const QString& sVerb : aVerbs) {
        QSettings stCmd(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\shell\\%2\\command")
                            .arg(a_sProgId, sVerb),
                        QSettings::NativeFormat);
        const QString sCmd = RegDefaultValue(stCmd);
        if (sCmd.isEmpty()) {
            continue;
        }
        // 去重：不同 ProgID 常指向同一 command（如多个 verb 都是同一 exe）。
        if (a_pSeenCmds->contains(sCmd.toLower())) {
            continue;
        }
        a_pSeenCmds->insert(sCmd.toLower());
        // 展示名优先级：verb 键 MuiVerb/默认值（明文时）→ 模板 exe 基名 → verb 名。
        QSettings stVerb(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\shell\\%2")
                             .arg(a_sProgId, sVerb),
                         QSettings::NativeFormat);
        QString sName = stVerb.value(QStringLiteral("MuiVerb")).toString().trimmed();
        if (sName.isEmpty() || sName.startsWith(QLatin1Char('@'))) {
            sName = RegDefaultValue(stVerb);
        }
        if (sName.isEmpty() || sName.startsWith(QLatin1Char('@'))) {
            sName = ExeBaseFromCommand(sCmd);
        }
        if (sName.isEmpty()) {
            sName = sVerb;
        }
        // 2026.09.17 兑底路径程序图标：从命令模板取 exe 路径交系统图标链路。
        const QStringList aCmdParts = QProcess::splitCommand(sCmd);
        QAction* pAct = a_pMenu->addAction(
            ProgramIcon(aCmdParts.isEmpty() ? QString() : aCmdParts.first()), sName);
        pAct->setData(sCmd);
        bAddedAny = true;
    }
    Q_UNUSED(bAddedAny);
}

// 2026.09.17 shell 同源枚举：SHAssocEnumHandlers（资源管理器「打开方式」同源，
//   含用户级 FileExts/OpenWithProgids、SystemFileAssociations（感知型 notepad/
//   wordpad）、VS Code 式注册、DDE 动词等全部来源）。返回收录数。
static int AddShellAssocHandlers(QMenu* a_pMenu,
                                 QVector<CProjectExplorerPanel::SOpenWithHandler*>& a_store,
                                 const QString& a_sDotExt,
                                 ASSOC_FILTER a_eFilter,
                                 int a_iMaxEntries = 0) {   // 2026.09.26 v3.1.244 上限（0=不限）
    IEnumAssocHandlers* pEnum = nullptr;
    const HRESULT hr = SHAssocEnumHandlers(
        reinterpret_cast<const wchar_t*>(a_sDotExt.utf16()), a_eFilter, &pEnum);
    if (FAILED(hr) || pEnum == nullptr) {
        return 0;
    }
    int nAdded = 0;
    IAssocHandler* aBatch[12] = {};
    ULONG ulGot = 0;
    while (SUCCEEDED(pEnum->Next(12, aBatch, &ulGot)) && ulGot > 0) {
        for (ULONG i = 0; i < ulGot; ++i) {
            IAssocHandler* pH = aBatch[i];
            // 2026.09.26 v3.1.244 上限截断：NONE 全量口径对无 OpenWithList 注册的
            //   扩展名（.bat 等）会把全机所有自荐处理器堆进来（用户实测「太多
            //   程序」）——封顶后续枚举全部释放丢弃。
            if (a_iMaxEntries > 0 && nAdded >= a_iMaxEntries) {
                if (pH != nullptr) { pH->Release(); }
                continue;
            }
            PWSTR pwszName = nullptr;
            if (pH != nullptr && SUCCEEDED(pH->GetUIName(&pwszName))
                    && pwszName != nullptr) {
                const QString sName = QString::fromWCharArray(pwszName).trimmed();
                CoTaskMemFree(pwszName);
                if (!sName.isEmpty()) {
                    // 2026.09.17 程序真实图标：GetIconLocation 出参=图标源文件路径
                    //（通常即程序 exe），交 QFileIconProvider 取系统图标。
                    PWSTR pwszIcon = nullptr;
                    int iIconIdx = 0;
                    QString sIconPath;
                    if (SUCCEEDED(pH->GetIconLocation(&pwszIcon, &iIconIdx))
                            && pwszIcon != nullptr) {
                        sIconPath = QString::fromWCharArray(pwszIcon);
                        CoTaskMemFree(pwszIcon);
                    }
                    QAction* pAct = a_pMenu->addAction(ProgramIcon(sIconPath), sName);
                    pAct->setData(static_cast<int>(a_store.size()));
                    auto* pEntry = new CProjectExplorerPanel::SOpenWithHandler();
                    pEntry->m_pHandler = pH;   // Next 已 AddRef，存入即转交所有权
                    a_store.append(pEntry);
                    ++nAdded;
                    continue;
                }
            }
            if (pH != nullptr) {
                pH->Release();   // 未收录（无名/空名）
            }
        }
        ulGot = 0;
    }
    pEnum->Release();
    return nAdded;
}

void CProjectExplorerPanel::BuildOpenWithMenu(QMenu* a_pParentMenu,
                                              const QString& a_sFilePath) {
    ClearOpenWithHandlers();
    const QString sSuffix = QFileInfo(a_sFilePath).suffix().toLower();
    if (sSuffix.isEmpty()) {
        a_pParentMenu->setEnabled(false);
        return;
    }
    // 主路径：SHAssocEnumHandlers（资源管理器同源，勿回退成纯注册表枚举——
    //   旧实现只扫 HKCR 机器级三口径，用户级注册（VS2015/VS2022/Qoder/Trae/
    //   记事本/写字板）全部遗失只出 QtCreator，2026.09.17 实害）。推荐口径=
    //   右键菜单语义；为空再退全量口径。
    // 2026.09.26 v3.1.244 两项收敛（用户报告 .bat「太多程序」+要能 CMD 执行）：
    //   ① .bat/.cmd 精简口径：首项固定「用 CMD 运行」（cmd /k 执行并保留窗口），
    //      其后只挂 RECOMMENDED（编辑器类），**不再退 NONE 全量**——.bat 无
    //      OpenWithList/OpenWithProgids 注册，RECOMMENDED 空、NONE 会把全机
    //      自荐处理器堆进来（几十项，用户实测）；RECOMMENDED 空则退注册表
    //      兑底（OpenWithList MRU 等，本就短）；
    //   ② 其余扩展名的 NONE 兑底封顶 12 项（同源问题预防性收敛）。
    const QString sExtKey = QStringLiteral(".") + sSuffix;
    const bool bBatch = (sSuffix == QStringLiteral("bat") || sSuffix == QStringLiteral("cmd"));
    if (bBatch) {
        QAction* pActCmd = a_pParentMenu->addAction(
            ProgramIcon(QDir::toNativeSeparators(
                QStringLiteral("C:/Windows/System32/cmd.exe"))),
            QStringLiteral("用 CMD 运行"));
        pActCmd->setData(static_cast<int>(CProjectExplorerPanel::s_iOpenWithCmdRun));
        pActCmd->setToolTip(QStringLiteral(
            "cmd /k 执行该批处理（完成后保留窗口显示输出；工作目录=文件所在目录）"));
        a_pParentMenu->addSeparator();
        AddShellAssocHandlers(a_pParentMenu, m_aOpenWithHandlers, sExtKey,
                              ASSOC_FILTER_RECOMMENDED);
        if (m_aOpenWithHandlers.isEmpty()) {
            BuildOpenWithRegistryFallback(a_pParentMenu, sExtKey);   // 下方既有兑底
        }
        return;
    }
    // 2026-10-09 可执行类（.exe/.msi/.com/.scr/.pif）同 .bat 收敛（用户实测截图：
    //   .exe 右键打开方式混入 KMPlayer/Foxmail/IE/WPS/智能播放器等全机自荐处理器
    //   ——RECOMMENDED 空时旧版兑 NONE 全量封顶 12，恰把 HKCR\Applications 无
    //   SupportedTypes 限制的自荐项全倒出来）。Explorer 对可执行文件本就不列
    //   打开方式程序：此处只走 RECOMMENDED、绝不退 NONE；无推荐 → 子菜单禁用
    //   （同资源管理器语义）。也不走注册表兑底——可执行类的 ProgID 动词是自身
    //   open（"%1" %*），列进「打开方式」无意义。
    if (sSuffix == QStringLiteral("exe") || sSuffix == QStringLiteral("msi")
            || sSuffix == QStringLiteral("com") || sSuffix == QStringLiteral("scr")
            || sSuffix == QStringLiteral("pif")) {
        AddShellAssocHandlers(a_pParentMenu, m_aOpenWithHandlers, sExtKey,
                              ASSOC_FILTER_RECOMMENDED);
        if (m_aOpenWithHandlers.isEmpty()) {
            a_pParentMenu->setEnabled(false);
        }
        return;
    }
    if (AddShellAssocHandlers(a_pParentMenu, m_aOpenWithHandlers, sExtKey,
                              ASSOC_FILTER_RECOMMENDED) > 0) {
        return;
    }
    if (AddShellAssocHandlers(a_pParentMenu, m_aOpenWithHandlers, sExtKey,
                              ASSOC_FILTER_NONE, 12) > 0) {   // 2026.09.26 封顶 12
        return;
    }
    BuildOpenWithRegistryFallback(a_pParentMenu, sExtKey);
}

// 2026.09.26 v3.1.244 自 BuildOpenWithMenu 抽出：注册表直读兑底（COM 枚举整体失败
//   或 .bat/.cmd 精简口径下 RECOMMENDED 为空时使用；含用户级补全与值名/子键修正）。
void CProjectExplorerPanel::BuildOpenWithRegistryFallback(QMenu* a_pParentMenu,
                                                          const QString& a_sExtKey) {
    // 枚举口径：
    //   0) HKCU FileExts\.<ext>\UserChoice → 用户默认（真实默认程序）；
    //   0.5) HKCR\.<ext> 默认值 → 主 ProgID；
    //   1) HKCU FileExts\.<ext>\OpenWithList（MRUList 顺序，值=exe 名）；
    //   2) HKCU\Software\Classes\.<ext>\OpenWithProgids（值名即 ProgID——
    //      HKCR 合并视图不并用户级同名值，须单独扫；VS2022 哈希 ProgID 在这）；
    //   2.5) HKCR\.<ext>\OpenWithProgids（值名即 ProgID，非子键！旧版 childGroups
    //      误枚举到 Shared 子键而漏全部条目——2026.09.17 实害根因）；
    //   3) HKCR\.<ext>\OpenWithList（XP 式子键）；
    //   4) PerceivedType → HKCR\SystemFileAssociations\<type>\OpenWithList
    //      （感知型：text 的 notepad/wordpad 在此注册）。
    QSet<QString> aSeenCmds;
    // UserChoice（Win10+ 有 Hash 值不校验，只取 ProgId 值）。
    QSettings stChoice(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                      "Windows\\CurrentVersion\\Explorer\\FileExts\\"
                                      "%1\\UserChoice").arg(a_sExtKey),
                       QSettings::NativeFormat);
    const QString sUserChoice = stChoice.value(QStringLiteral("ProgId")).toString();
    if (!sUserChoice.isEmpty()) {
        AddProgIdVerbs(a_pParentMenu, sUserChoice, &aSeenCmds);
    }
    QSettings stExt(QStringLiteral("HKEY_CLASSES_ROOT\\%1").arg(a_sExtKey),
                    QSettings::NativeFormat);
    const QString sMainProgId = RegDefaultValue(stExt);
    if (!sMainProgId.isEmpty()) {
        AddProgIdVerbs(a_pParentMenu, sMainProgId, &aSeenCmds);
    }
    // 用户级 FileExts OpenWithList：值名 a/b/c… = exe 名，MRUList 值给出顺序。
    {
        QSettings stFowl(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\"
                                        "Windows\\CurrentVersion\\Explorer\\"
                                        "FileExts\\%1\\OpenWithList").arg(a_sExtKey),
                         QSettings::NativeFormat);
        QStringList aExes;
        const QString sMru = stFowl.value(QStringLiteral("MRUList")).toString();
        if (!sMru.isEmpty()) {
            for (const QChar& ch : sMru) {
                const QString sExe = stFowl.value(QString(ch)).toString();
                if (!sExe.isEmpty()) {
                    aExes.append(sExe);
                }
            }
        } else {
            aExes = stFowl.childKeys();
            aExes.removeAll(QStringLiteral("MRUList"));
        }
        for (const QString& sExe : aExes) {
            AddProgIdVerbs(a_pParentMenu,
                           QStringLiteral("Applications\\%1").arg(sExe), &aSeenCmds);
        }
    }
    // OpenWithProgids：值名即 ProgID（HKCU Classes 与 HKCR 各扫一遍，合集去重）。
    const QStringList aOwpiRoots = {
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1\\OpenWithProgids"),
        QStringLiteral("HKEY_CLASSES_ROOT\\%1\\OpenWithProgids"),
    };
    for (const QString& sRoot : aOwpiRoots) {
        QSettings stOwpi(sRoot.arg(a_sExtKey), QSettings::NativeFormat);
        const QStringList aOwpiIds = stOwpi.childKeys();   // 值名（旧版误用 childGroups）
        for (const QString& sId : aOwpiIds) {
            AddProgIdVerbs(a_pParentMenu, sId, &aSeenCmds);
        }
    }
    // OpenWithList：<exe> 子键就地找 shell\open/edit\command，找不到再查
    // HKCR\Applications\<exe>。
    QSettings stOwl(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\OpenWithList")
                        .arg(a_sExtKey),
                    QSettings::NativeFormat);
    const QStringList aOwlExes = stOwl.childGroups();
    for (const QString& sExe : aOwlExes) {
        AddProgIdVerbs(a_pParentMenu,
                       QStringLiteral("%1\\OpenWithList\\%2").arg(a_sExtKey, sExe),
                       &aSeenCmds);
        AddProgIdVerbs(a_pParentMenu,
                       QStringLiteral("Applications\\%1").arg(sExe), &aSeenCmds);
    }
    // 感知型（PerceivedType=text/image/…）：SystemFileAssociations 下同样有
    // OpenWithList（子键=exe 名，notepad/wordpad 在 text 下注册）。
    const QString sPerceived = stExt.value(QStringLiteral("PerceivedType")).toString();
    if (!sPerceived.isEmpty()) {
        QSettings stSfa(QStringLiteral("HKEY_CLASSES_ROOT\\SystemFileAssociations\\"
                                       "%1\\OpenWithList").arg(sPerceived),
                        QSettings::NativeFormat);
        const QStringList aSfaExes = stSfa.childGroups();
        for (const QString& sExe : aSfaExes) {
            AddProgIdVerbs(a_pParentMenu,
                           QStringLiteral("Applications\\%1").arg(sExe), &aSeenCmds);
        }
    }
    // 无任何注册程序：子菜单置灰（Windows 语义下几乎不发生，扩展名总有默认项）。
    if (a_pParentMenu->isEmpty()) {
        a_pParentMenu->setEnabled(false);
    }
}

int CProjectExplorerPanel::ProbeAssocHandlers(const QString& a_sExtWithDot,
                                               int a_iFilter, QString* a_pFirstName,
                                               int* a_pIconifiedCount) {
    QMenu oMenu;
    QVector<SOpenWithHandler*> aStore;
    const int n = AddShellAssocHandlers(
        &oMenu, aStore, a_sExtWithDot,
        a_iFilter == 0 ? ASSOC_FILTER_RECOMMENDED : ASSOC_FILTER_NONE);
    if (a_pFirstName != nullptr && !oMenu.actions().isEmpty()) {
        *a_pFirstName = oMenu.actions().constFirst()->text();
    }
    if (a_pIconifiedCount != nullptr) {
        *a_pIconifiedCount = 0;
        for (const QAction* pAct : oMenu.actions()) {
            if (!pAct->icon().isNull()) {
                ++(*a_pIconifiedCount);
            }
        }
    }
    for (SOpenWithHandler* pEntry : aStore) {
        if (pEntry->m_pHandler != nullptr) {
            pEntry->m_pHandler->Release();
        }
        delete pEntry;
    }
    return n;
}

void CProjectExplorerPanel::LaunchOpenWithCommand(const QString& a_sCmdTemplate,
                                                  const QString& a_sFilePath) {
    // 模板形如 "C:\App\app.exe" "%1" / app.exe %1 %*：splitCommand 拆 token，
    //   占位符替换——%1/%l/%L/%v/%V → 文件原生路径；%2..%9/%* 丢弃。
    //   首 token 为程序，其余为参数，startDetached 启动（shell 已展开环境）。
    //   2026.08.21 环境变量展开：注册表 command 模板常含 %SystemRoot%/%windir% 等
    //   变量引用，startDetached 不做 shell 展开（如 notepad /pt 打印 verb），必须
    //   先用系统环境表把 %VAR% 就地替换成实值再启动。
    const QString sNative = QDir::toNativeSeparators(a_sFilePath);
    QStringList aParts = QProcess::splitCommand(ExpandEnvVars(a_sCmdTemplate));
    if (aParts.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("打开方式"),
            QStringLiteral("注册的打开命令无效：\n%1").arg(a_sCmdTemplate));
        return;
    }
    const QString sExe = aParts.takeFirst();
    for (QString& sTok : aParts) {
        if (sTok == QLatin1String("%*")) {
            sTok.clear();
            continue;
        }
        const QChar cPlace = sTok.startsWith(QLatin1Char('%')) && sTok.size() >= 2
                                 ? sTok.at(1).toLower() : QChar();
        if (cPlace == QLatin1Char('1') || cPlace == QLatin1Char('l')
            || cPlace == QLatin1Char('v')) {
            sTok = sNative;   // %1/%l/%v（含大写变体）→ 文件路径
        } else if (cPlace.isDigit() && cPlace != QLatin1Char('0')
                   && cPlace != QLatin1Char('1')) {
            sTok.clear();     // %2..%9 附加占位符：无对应实参，丢弃
        } else if (sTok.contains(QStringLiteral("%1"), Qt::CaseInsensitive)) {
            // token 内嵌占位（如 -file:%1）：原地替换。
            sTok.replace(QStringLiteral("%1"), sNative, Qt::CaseInsensitive);
        }
    }
    aParts.removeAll(QString());
    if (!QProcess::startDetached(sExe, aParts)) {
        QMessageBox::warning(nullptr, QStringLiteral("打开方式"),
            QStringLiteral("无法启动注册程序：\n%1\n\n文件：%2").arg(
                a_sCmdTemplate, sNative));
    }
}

// 2026.08.21 展开命令模板里的 %VAR% 环境变量引用（注册表 command 常见
// %SystemRoot%\system32\notepad.exe 这类写法）：逐个查系统环境表替换实值，
// 查不到的保留原样（不误伤）。%1/%l 等占位符不在 [A-Za-z_][A-Za-z0-9_]*%
// 文法内，不会被本函数碰到。
static QString ExpandEnvVars(const QString& a_sCmd) {
    static const QRegularExpression s_reVar(
        QStringLiteral("%([A-Za-z_][A-Za-z0-9_]*)%"),
        QRegularExpression::CaseInsensitiveOption);
    const QProcessEnvironment stEnv = QProcessEnvironment::systemEnvironment();
    QString sOut;
    qsizetype iPos = 0;
    QRegularExpressionMatchIterator it = s_reVar.globalMatch(a_sCmd);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        sOut += a_sCmd.mid(iPos, m.capturedStart() - iPos);
        const QString sVal = stEnv.value(m.captured(1));
        sOut += sVal.isEmpty() ? m.captured(0) : sVal;
        iPos = m.capturedEnd();
    }
    sOut += a_sCmd.mid(iPos);
    return sOut;
}
