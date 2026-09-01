#include "CProjectExplorerPanel.h"

#include <QTreeView>
#include <QFileSystemModel>
#include <QMenu>
#include <QAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QUrl>
#include <QModelIndex>
#include <QFont>
#include <QVariant>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QRegularExpression>
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

// 2026.08.28 非文本文件的系统打开改走 ShellExecuteW（显式传工作目录，对齐
//   Windows 资源管理器双击口径）；同项目其他文件，NOMINMAX 防 min/max 宏污染。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW（本翻译单元 windows.h 未带入，显式包含）

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
static bool CopyDirRecursively(const QString& a_sSrc, const QString& a_sDst) {
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
        if (!CopyDirRecursively(stSrc.filePath(sSub), QDir(a_sDst).filePath(sSub))) {
            return false;
        }
    }
    // 文件拷贝
    const QStringList aFiles = stSrc.entryList(QDir::Files);
    for (const QString& sFile : aFiles) {
        if (!QFile::copy(stSrc.filePath(sFile), QDir(a_sDst).filePath(sFile))) {
            return false;
        }
    }
    return true;
}

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
    // HiddenDirs 丬不到行可隐藏/显示；.git/.vs 等仍由 HiddenDirs 名单按目录隐藏。

    m_pTree = new QTreeView(this);
    m_pTree->setModel(m_pModel);
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
    for (int iCol = 1; iCol < m_pModel->columnCount(); ++iCol) {
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
    // 父目录行随模型异步陆续插入：新到的兄弟行若非工程根则继续隐藏（见 SetRoot）；
    // 同时做目录级隐藏——命中 HiddenDirs 清单的目录（build/.git 等）整棵不显示。
    connect(m_pModel, &QFileSystemModel::rowsInserted, this,
            [this](const QModelIndex& a_stParent, int a_iFirst, int a_iLast) {
        HideGeneratedDirs(a_stParent, a_iFirst, a_iLast);
        if (!m_stRootIdx.isValid() || a_stParent != m_stRootIdx.parent()) {
            return;
        }
        HideSiblingRows(a_stParent, a_iFirst, a_iLast);
    });
    pV->addWidget(m_pTree);
}

void CProjectExplorerPanel::SetRoot(const QString& a_sDir) {
    m_sRootDir = a_sDir;
    m_stRootIdx = QPersistentModelIndex();
    if (a_sDir.isEmpty()) {
        m_pTree->setRootIndex(QModelIndex());
        return;
    }
    // 工程目录作为树的可见根节点（而不是把它的子目录直接铺在顶层）：
    // 视图根设为工程目录的父目录，再把父目录里除工程目录外的兄弟行全部隐藏，
    // 树顶层即只剩工程目录一项，随后自动展开。
    // setRootPath 后模型异步加载（NFR-6 不阻塞）：不主动 fetchMore 全量展开；
    // 父目录里迟到的兄弟行由构造里连接的 rowsInserted 钩子继续隐藏。
    const QModelIndex stRoot = m_pModel->setRootPath(a_sDir);
    m_stRootIdx = QPersistentModelIndex(stRoot);
    m_pTree->setRootIndex(stRoot.parent());
    HideSiblingRows(stRoot.parent(), 0, m_pModel->rowCount(stRoot.parent()) - 1);
    m_pTree->expand(stRoot);
}

void CProjectExplorerPanel::HideSiblingRows(const QModelIndex& a_stParent,
                                            int a_iFirst, int a_iLast) {
    if (!a_stParent.isValid()) {
        return;
    }
    const int iFirst = a_iFirst < 0 ? 0 : a_iFirst;
    const int iCount = m_pModel->rowCount(a_stParent);
    for (int iRow = iFirst; iRow <= a_iLast && iRow < iCount; ++iRow) {
        // 工程根行恢复可见、其余隐藏（同时覆盖同父目录下切工程的场景）。
        m_pTree->setRowHidden(iRow, a_stParent,
                              m_pModel->index(iRow, 0, a_stParent) != m_stRootIdx);
    }
}

void CProjectExplorerPanel::HideGeneratedDirs(const QModelIndex& a_stParent,
                                              int a_iFirst, int a_iLast) {
    if (!a_stParent.isValid()) {
        return;
    }
    const int iFirst = a_iFirst < 0 ? 0 : a_iFirst;
    const int iCount = m_pModel->rowCount(a_stParent);
    for (int iRow = iFirst; iRow <= a_iLast && iRow < iCount; ++iRow) {
        const QModelIndex stRow = m_pModel->index(iRow, 0, a_stParent);
        if (stRow == m_stRootIdx || !m_pModel->isDir(stRow)) {
            continue;   // 工程根本身与文件不走目录隐藏
        }
        if (HiddenDirs().contains(m_pModel->fileName(stRow))) {
            m_pTree->setRowHidden(iRow, a_stParent, true);
        }
    }
}

QString CProjectExplorerPanel::SelectedAbsPath() const {
    const QModelIndex stIdx = m_pTree->currentIndex();
    if (!stIdx.isValid()) {
        return QString();
    }
    const QString sPath = m_pModel->filePath(stIdx);
    if (sPath.isEmpty()) {
        return QString();
    }
    return sPath;
}

bool CProjectExplorerPanel::IsUnderRoot(const QString& a_sAbsPath) const {
    if (m_sRootDir.isEmpty() || a_sAbsPath.isEmpty()) {
        return false;
    }
    const QString sRoot = QDir(m_sRootDir).canonicalPath();
    const QString sAbs  = QDir(a_sAbsPath).canonicalPath();
    if (sRoot.isEmpty() || sAbs.isEmpty()) {
        // 路径尚不存在（如目标尚未创建）→ 用规范化路径做前缀比较兜底。
        const QString sRootN = QDir::cleanPath(m_sRootDir);
        const QString sAbsN  = QDir::cleanPath(a_sAbsPath);
        return sAbsN == sRootN || sAbsN.startsWith(sRootN + QLatin1Char('/'));
    }
    return sAbs == sRoot || sAbs.startsWith(sRoot + QLatin1Char('/'));
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
    QAction* pActExplorer  = menu.addAction(QStringLiteral("通过资源管理器打开"));
    // 2026.08.21 「打开方式」子菜单：列出该文件类型在注册表注册过的程序（仅文件）。
    QMenu* pMenuOpenWith = nullptr;
    if (QFileInfo(sSel).isFile()) {
        pMenuOpenWith = menu.addMenu(QStringLiteral("打开方式"));
        BuildOpenWithMenu(pMenuOpenWith, sSel);
    }
    QAction* pActCut       = menu.addAction(QStringLiteral("剪切"));
    QAction* pActCopy      = menu.addAction(QStringLiteral("复制"));
    QAction* pActPaste     = menu.addAction(QStringLiteral("粘贴"));
    QAction* pActRename    = menu.addAction(QStringLiteral("改名"));
    menu.addSeparator();
    QAction* pActDelete    = menu.addAction(QStringLiteral("删除"));
    menu.addSeparator();
    QAction* pActCopyPath  = menu.addAction(QStringLiteral("复制路径"));
    QAction* pActCopyRel   = menu.addAction(QStringLiteral("复制相对路径"));
    QAction* pActAddChat   = menu.addAction(QStringLiteral("添加到聊天框"));

    // 粘贴依赖工程根已设；其余依赖选中节点在根内。
    pActPaste->setEnabled(!m_sRootDir.isEmpty() && !m_sClipboardPath.isEmpty());
    if (!bUnderRoot) {
        // 选中根外节点：禁用所有写操作与路径操作（NFR-7）。
        for (QAction* pA : {pActCut, pActCopy, pActPaste, pActRename,
                            pActDelete, pActCopyPath, pActCopyRel, pActAddChat,
                            pActExplorer}) {
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
    if (pSel->data().isValid() && pSel->data().canConvert<QString>()) {
        LaunchOpenWithCommand(pSel->data().toString(), sSel);
        return;
    }
    if      (pSel == pActExplorer) { OpenInExplorer(); }
    else if (pSel == pActCut)      { DoCut(); }
    else if (pSel == pActCopy)     { DoCopy(); }
    else if (pSel == pActPaste)    { DoPaste(); }
    else if (pSel == pActRename)   { DoRename(); }
    else if (pSel == pActDelete)   { DoDelete(); }
    else if (pSel == pActCopyPath) { CopyPath(); }
    else if (pSel == pActCopyRel)  { CopyRelativePath(); }
    else if (pSel == pActAddChat)  { AddToChat(); }
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
    const QString sPath = m_pModel->filePath(a_stIdx);
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
    const QString sPath = m_pModel->filePath(a_stIdx);
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
    //   附带收益：失败（返回值≤32）弹窗可见真因，旧 openUrl 只写调试输出。
    const QString sNative = QDir::toNativeSeparators(a_sAbsPath);
    const QString sDir = QDir::toNativeSeparators(
        QFileInfo(a_sAbsPath).absolutePath());
    const HINSTANCE hInst = ShellExecuteW(
        nullptr, nullptr,
        reinterpret_cast<LPCWSTR>(sNative.utf16()),
        nullptr,
        reinterpret_cast<LPCWSTR>(sDir.utf16()),
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
            QStringLiteral("无法用系统默认程序打开（错误码 %1）：\n%2")
                .arg(QString::number(reinterpret_cast<INT_PTR>(hInst)))
                .arg(sNative));
    }
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
        const QString sPath = m_pModel->filePath(stIdx);
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
    const QIcon stIcon = m_pModel->fileIcon(QModelIndex(m_stDragIdx));
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
    if (stIdx.isValid() && m_pModel->isDir(stIdx)) {
        sDstDir = m_pModel->filePath(stIdx);
    } else if (stIdx.isValid()) {
        sDstDir = QFileInfo(m_pModel->filePath(stIdx)).absolutePath();
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
    QFile stFile(a_sAbsPath);
    if (!stFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray aHead = stFile.read(4096);
    return !aHead.isEmpty() && !aHead.contains('\0');
}

void CProjectExplorerPanel::DoCut() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    m_sClipboardPath = sPath;
    m_bCutMode = true;
}

void CProjectExplorerPanel::DoCopy() {
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath)) {
        return;
    }
    m_sClipboardPath = sPath;
    m_bCutMode = false;
}

void CProjectExplorerPanel::DoPaste() {
    if (m_sClipboardPath.isEmpty() || m_sRootDir.isEmpty()) {
        return;
    }
    if (!IsUnderRoot(m_sClipboardPath)) {
        return;
    }
    const QFileInfo stSrc(m_sClipboardPath);
    if (!stSrc.exists()) {
        return;
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
    const QString sDstPath = QDir(sDstDir).filePath(stSrc.fileName());
    if (QFileInfo::exists(sDstPath)) {
        QMessageBox::warning(this, QStringLiteral("粘贴"),
            QStringLiteral("目标已存在：%1").arg(sDstPath));
        return;
    }
    bool bOk = false;
    if (stSrc.isDir()) {
        bOk = CopyDirRecursively(m_sClipboardPath, sDstPath);
    } else {
        bOk = QFile::copy(m_sClipboardPath, sDstPath);
    }
    if (bOk && m_bCutMode) {
        // 剪切：源删除（目录用 rmdir，文件 remove）。
        if (stSrc.isDir()) {
            QDir(m_sClipboardPath).removeRecursively();
        } else {
            QFile::remove(m_sClipboardPath);
        }
        m_sClipboardPath.clear();
        m_bCutMode = false;
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
    const QString sPath = SelectedAbsPath();
    if (!IsUnderRoot(sPath) || m_sRootDir.isEmpty()) {
        return;
    }
    const QString sRel = QDir(m_sRootDir).relativeFilePath(sPath);
    QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(sRel));
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
    QStringList aArgs;
    if (QFileInfo(sPath).isDir()) {
        aArgs << QDir::toNativeSeparators(sPath);
    } else {
        aArgs << QStringLiteral("/select,") + QDir::toNativeSeparators(sPath);
    }
    QProcess::startDetached(QStringLiteral("explorer.exe"), aArgs);
}

// 展开命令模板里的 %VAR% 环境变量引用（定义见文末）。
static QString ExpandEnvVars(const QString& a_sCmd);

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
        QAction* pAct = a_pMenu->addAction(sName);
        pAct->setData(sCmd);
        bAddedAny = true;
    }
    Q_UNUSED(bAddedAny);
}

void CProjectExplorerPanel::BuildOpenWithMenu(QMenu* a_pParentMenu,
                                              const QString& a_sFilePath) {
    // 枚举口径（与系统「打开方式」列表同源，全部在 HKCR）：
    //   1) HKCR\.<ext> 默认值 → 主 ProgID（当前默认程序）的 shell verbs；
    //   2) HKCR\.<ext>\OpenWithProgids → 各关联 ProgID 的 shell verbs；
    //   3) HKCR\.<ext>\OpenWithList\<exe> → XP 式列表（command 在
    //      HKCR\Applications\<exe>\shell\open\command 或就地 shell 下）。
    const QString sSuffix = QFileInfo(a_sFilePath).suffix().toLower();
    if (sSuffix.isEmpty()) {
        a_pParentMenu->setEnabled(false);
        return;
    }
    const QString sExtKey = QStringLiteral(".") + sSuffix;
    QSet<QString> aSeenCmds;
    QSettings stExt(QStringLiteral("HKEY_CLASSES_ROOT\\%1").arg(sExtKey),
                    QSettings::NativeFormat);
    const QString sMainProgId = RegDefaultValue(stExt);
    if (!sMainProgId.isEmpty()) {
        AddProgIdVerbs(a_pParentMenu, sMainProgId, &aSeenCmds);
    }
    // OpenWithProgids：子键名即 ProgID（值是空串/时间戳，不用读值）。
    QSettings stOwpi(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\OpenWithProgids")
                         .arg(sExtKey),
                     QSettings::NativeFormat);
    const QStringList aOwpiIds = stOwpi.childGroups();
    for (const QString& sId : aOwpiIds) {
        AddProgIdVerbs(a_pParentMenu, sId, &aSeenCmds);
    }
    // OpenWithList：<exe> 子键就地找 shell\open/edit\command，找不到再查
    // HKCR\Applications\<exe>。
    QSettings stOwl(QStringLiteral("HKEY_CLASSES_ROOT\\%1\\OpenWithList")
                        .arg(sExtKey),
                    QSettings::NativeFormat);
    const QStringList aOwlExes = stOwl.childGroups();
    for (const QString& sExe : aOwlExes) {
        AddProgIdVerbs(a_pParentMenu,
                       QStringLiteral("%1\\OpenWithList\\%2").arg(sExtKey, sExe),
                       &aSeenCmds);
        AddProgIdVerbs(a_pParentMenu,
                       QStringLiteral("Applications\\%1").arg(sExe), &aSeenCmds);
    }
    // 无任何注册程序：子菜单置灰（Windows 语义下几乎不发生，扩展名总有默认项）。
    if (a_pParentMenu->isEmpty()) {
        a_pParentMenu->setEnabled(false);
    }
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
