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

const QStringList& CProjectExplorerPanel::HiddenDirs() {
    // Q1 默认：隐藏常见生成目录（后续可由 FR-8 设置调）。
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
    // 过滤常见生成目录里的中间产物文件（目录级隐藏见上注释，本轮取文件级过滤）。
    m_pModel->setNameFilters({
        QStringLiteral("*.o"), QStringLiteral("*.obj"), QStringLiteral("*.exe"),
        QStringLiteral("*.pdb"), QStringLiteral("*.ilk"), QStringLiteral("*.lib")
    });
    m_pModel->setNameFilterDisables(false);   // 命中过滤器直接隐藏（非禁用）

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
    pV->addWidget(m_pTree);
}

void CProjectExplorerPanel::SetRoot(const QString& a_sDir) {
    m_sRootDir = a_sDir;
    if (a_sDir.isEmpty()) {
        m_pTree->setRootIndex(QModelIndex());
        return;
    }
    // setRootPath 后模型异步加载（NFR-6 不阻塞）：不主动 fetchMore 全量展开。
    const QModelIndex stRoot = m_pModel->setRootPath(a_sDir);
    m_pTree->setRootIndex(stRoot);
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
    QAction* pActOpen      = menu.addAction(QStringLiteral("打开"));
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
                            pActDelete, pActCopyPath, pActCopyRel, pActAddChat, pActOpen}) {
            pA->setEnabled(false);
        }
    }

    QAction* pSel = menu.exec(m_pTree->viewport()->mapToGlobal(a_stPos));
    if (pSel == nullptr) {
        return;
    }
    if      (pSel == pActOpen)     { OnDoubleClicked(stIdx); }
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
    // Q1 默认：双击调系统默认程序打开（目录则用资源管理器打开）。
    if (!a_stIdx.isValid()) {
        return;
    }
    const QString sPath = m_pModel->filePath(a_stIdx);
    if (sPath.isEmpty() || !IsUnderRoot(sPath)) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(sPath));
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
