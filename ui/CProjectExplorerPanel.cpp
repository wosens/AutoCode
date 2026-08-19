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
#include <QDesktopServices>
#include <QUrl>
#include <QModelIndex>
#include <QFont>
#include <QVariant>
#include <QProcess>

const QStringList& CProjectExplorerPanel::HiddenDirs() {
    // Q1 默认：隐藏常见生成目录（后续可由 FR-8 设置调）。
    // 生效方式：rowsInserted 钩子 → HideGeneratedDirs 逐行隐藏（2026-08-17 接通，此前未挂接）。
    static const QStringList s_aHidden {
        QStringLiteral(".autodev"), QStringLiteral("build"),
        QStringLiteral("Execute"), QStringLiteral(".git"),
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
    m_pModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    // 文件不做类型过滤、全部显示（2026-08-17 修正：旧 setNameFilters(*.o 等)+
    // setNameFilterDisables(false) 的真实语义是「只显示匹配项」，曾导致源码/文档
    // 全部被隐藏、树里只剩构建产物，与意图相反）。生成目录（build/Execute/.git 等）
    // 仍由 HiddenDirs 按目录整体隐藏。

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
    connect(m_pTree, &QTreeView::customContextMenuRequested,
            this, &CProjectExplorerPanel::OnCustomContextMenu);
    connect(m_pTree, &QTreeView::doubleClicked,
            this, &CProjectExplorerPanel::OnDoubleClicked);
    // 父目录行随模型异步陆续插入：新到的兄弟行若非工程根则继续隐藏（见 SetRoot）；
    // 同时做目录级隐藏——命中 HiddenDirs 清单的目录（build/Execute/.git 等）整棵不显示。
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
    }

    QAction* pSel = menu.exec(m_pTree->viewport()->mapToGlobal(a_stPos));
    if (pSel == nullptr) {
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
    //   转发到执行面板）；目录/非文本 → 保持 Q1 原行为（系统默认程序/资源管理器打开）。
    if (!a_stIdx.isValid()) {
        return;
    }
    const QString sPath = m_pModel->filePath(a_stIdx);
    if (sPath.isEmpty() || !IsUnderRoot(sPath)) {
        return;
    }
    const QFileInfo stInfo(sPath);
    if (stInfo.isFile() && IsTextFile(sPath)) {
        emit sigFileOpen(sPath);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(sPath));
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
