#include "CFilePreviewWidget.h"

#include <QStackedWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QScrollBar>
#include <QFile>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QShortcut>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBlock>
#include <QPalette>
#include "core/CFrwDevice.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QDir>
#include "CMenuIcon.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ————————————————————————————————————————————————
// 十六进制视图：QSplitter 左右双区（等宽 QPlainTextEdit），滚动同步。
//   左区：偏移(8位hex) + 每行 16 字节 hex；右区：每行 16 字节 ASCII（不可印显示 '.'）
//   两区首行均为 00..0F 表头。大文件分页：每页 256KB，状态栏提示翻页。
// ————————————————————————————————————————————————
namespace {

constexpr qsizetype kiHexPageSize = 256 * 1024;   // 每页字节数（16K 行）

QString HexHeaderRow() {
    QString s = QStringLiteral("偏移      ");
    for (int i = 0; i < 16; ++i) {
        s += QStringLiteral(" %1").arg(i, 2, 16, QChar(u'0'));
    }
    return s.toUpper();
}

QString AsciiHeaderRow() {
    QString s = QStringLiteral("        ");
    for (int i = 0; i < 16; ++i) {
        s += QStringLiteral("%1").arg(i, 2, 16, QChar(u'0'));
    }
    return s.toUpper();
}

// 生成 [a_uOff, a_uOff+a_aBytes) 的 hex/ascii 两列文本
void BuildHexPage(const QByteArray& a_aAll, qsizetype a_iOff, QString& a_sHex, QString& a_sAscii) {
    a_sHex = HexHeaderRow();
    a_sAscii = AsciiHeaderRow();
    const qsizetype iEnd = qMin<qsizetype>(a_aAll.size(), a_iOff + kiHexPageSize);
    for (qsizetype i = a_iOff; i < iEnd; i += 16) {
        const qsizetype n = qMin<qsizetype>(16, iEnd - i);
        QString sHex = QStringLiteral("%1 ").arg(uint(i), 8, 16, QChar(u'0')).toUpper();
        QString sAsc;
        for (qsizetype j = 0; j < 16; ++j) {
            if (j == 8) {
                sHex += u' ';   // 第 8 字节后加空隙，16 字节两组更好读
            }
            if (j < n) {
                const uchar c = uchar(a_aAll.at(i + j));
                sHex += QStringLiteral(" %1").arg(c, 2, 16, QChar(u'0')).toUpper();
                sAsc += (c >= 0x20 && c <= 0x7E) ? QChar(c) : QChar(u'.');
            } else {
                sHex += QStringLiteral("   ");
                sAsc += u' ';
            }
        }
        a_sHex += u'\n';
        a_sHex += sHex;
        a_sAscii += u'\n';
        a_sAscii += sAsc;
    }
}

// 2026.09.22 文件内查找高亮：命中后编辑器无焦点（焦点在查找输入框），QTextEdit
//   /QPlainTextEdit 对无焦点选区按 QPalette::Inactive 组的 Highlight 绘制，Windows
//   缺省为浅灰，命中文字看不清；这里把 Inactive 组改为蓝底白字（有焦点时仍走
//   系统 Active 原生蓝，两态观感一致且始终可读）。
void ApplyFindSelectionPalette(QWidget* a_pEdit) {
    QPalette stPal = a_pEdit->palette();
    const QColor clrBlue(0x00, 0x78, 0xD7);   // 系统选中蓝（Win11 强调色）
    stPal.setColor(QPalette::Inactive, QPalette::Highlight, clrBlue);
    stPal.setColor(QPalette::Inactive, QPalette::HighlightedText, Qt::white);
    a_pEdit->setPalette(stPal);
}

QPlainTextEdit* MakeMonoEdit(QWidget* a_pParent) {
    auto* p = new QPlainTextEdit(a_pParent);
    p->setReadOnly(true);
    p->setLineWrapMode(QPlainTextEdit::NoWrap);
    p->setTabChangesFocus(true);
    QFont f = p->font();
    f.setFamily(QStringLiteral("Consolas"));
    f.setPointSize(10);
    p->setFont(f);
    ApplyFindSelectionPalette(p);   // 十六进制 ASCII 列查找命中同样需要蓝底高亮
    return p;
}

// 2026.09.22 文件内查找执行体：QTextEdit/QPlainTextEdit 共用的 find 包装——命中
//   即选中并滚动（编辑器原生行为）；循环开启时未命中则回卷到文档首/尾重试一次。
//   模板双型：QTextBrowser(纯文本/MD/HTML) 与 QPlainTextEdit(hex ASCII 列)。
template<class T>
bool FindInEditor(T* a_pEdit, const QString& a_sText, bool a_bBackward, bool a_bCase, bool a_bWrap) {
    if (a_pEdit == nullptr || a_sText.isEmpty()) {
        return false;
    }
    QTextDocument::FindFlags fl;
    if (a_bBackward) {
        fl |= QTextDocument::FindBackward;
    }
    if (a_bCase) {
        fl |= QTextDocument::FindCaseSensitively;
    }
    if (a_pEdit->find(a_sText, fl)) {
        return true;
    }
    if (!a_bWrap) {
        return false;
    }
    QTextCursor c = a_pEdit->textCursor();
    c.movePosition(a_bBackward ? QTextCursor::End : QTextCursor::Start);
    a_pEdit->setTextCursor(c);
    return a_pEdit->find(a_sText, fl);
}

} // namespace

// 双区滚动同步的轻量包装（互连 verticalScrollBar，标志位防回环）
class CFileHexView : public QWidget {
public:
    explicit CFileHexView(QWidget* a_pParent = nullptr) : QWidget(a_pParent) {
        auto* pSplit = new QSplitter(Qt::Horizontal, this);
        m_pHex  = MakeMonoEdit(pSplit);
        m_pAsc  = MakeMonoEdit(pSplit);
        pSplit->addWidget(m_pHex);
        pSplit->addWidget(m_pAsc);
        pSplit->setStretchFactor(0, 3);
        pSplit->setStretchFactor(1, 2);
        pSplit->setChildrenCollapsible(false);
        auto* pLay = new QVBoxLayout(this);
        pLay->setContentsMargins(0, 0, 0, 0);
        pLay->addWidget(pSplit);

        // 垂直滚动同步（用户滚任一侧，另一侧跟随；程序设值不重入）
        connect(m_pHex->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
            if (!m_bSync && m_pAsc->verticalScrollBar()->value() != v) {
                m_bSync = true;
                m_pAsc->verticalScrollBar()->setValue(v);
                m_bSync = false;
            }
        });
        connect(m_pAsc->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
            if (!m_bSync && m_pHex->verticalScrollBar()->value() != v) {
                m_bSync = true;
                m_pHex->verticalScrollBar()->setValue(v);
                m_bSync = false;
            }
        });
    }

    void SetContent(const QString& a_sHex, const QString& a_sAsc) {
        m_pHex->setPlainText(a_sHex);
        m_pAsc->setPlainText(a_sAsc);
        m_pHex->verticalScrollBar()->setValue(0);
        m_pAsc->verticalScrollBar()->setValue(0);
    }

    // 2026.09.22 查找落在 ASCII 列（可读列，命中即选中；hex 列是字节码无查找意义）。
    bool FindText(const QString& a_sText, bool a_bBackward, bool a_bCase, bool a_bWrap) {
        return FindInEditor(m_pAsc, a_sText, a_bBackward, a_bCase, a_bWrap);
    }



private:
    QPlainTextEdit* m_pHex = nullptr;
    QPlainTextEdit* m_pAsc = nullptr;
    bool            m_bSync = false;
};

// ————————————————————————————————————————————————
// CFilePreviewWidget
// ————————————————————————————————————————————————
CFilePreviewWidget::CFilePreviewWidget(QWidget* a_pParent) : QWidget(a_pParent) {
    BuildUi();
}

void CFilePreviewWidget::BuildUi() {
    m_pStack = new QStackedWidget(this);

    // 公共等宽字体
    QFont fMono;
    fMono.setFamily(QStringLiteral("Consolas"));
    fMono.setPointSize(11);

    // [0] 纯文本
    m_pText = new QTextBrowser(m_pStack);
    m_pText->setReadOnly(true);
    m_pText->setLineWrapMode(QTextBrowser::NoWrap);
    m_pText->setFont(fMono);
    m_pText->setOpenExternalLinks(false);
    ApplyFindSelectionPalette(m_pText);   // 查找命中无焦点时蓝底可读
    m_pStack->addWidget(m_pText);

    // [1] Markdown
    m_pMd = new QTextBrowser(m_pStack);
    m_pMd->setReadOnly(true);
    m_pMd->setOpenExternalLinks(true);
    QFont fMd;
    fMd.setPointSize(11);
    m_pMd->setFont(fMd);
    ApplyFindSelectionPalette(m_pMd);
    m_pStack->addWidget(m_pMd);

    // [2] HTML
    m_pHtml = new QTextBrowser(m_pStack);
    m_pHtml->setReadOnly(true);
    m_pHtml->setOpenExternalLinks(true);
    m_pHtml->setFont(fMd);
    ApplyFindSelectionPalette(m_pHtml);
    m_pStack->addWidget(m_pHtml);

    // [3] 十六进制
    m_pHexWrap = new CFileHexView(m_pStack);
    m_pStack->addWidget(m_pHexWrap);

    // 底部状态栏
    m_pStat = new QLabel(this);
    m_pStat->setContentsMargins(6, 2, 6, 2);
    QFont fStat = m_pStat->font();
    fStat.setPointSize(9);
    m_pStat->setFont(fStat);
    m_pStat->setStyleSheet(QStringLiteral("color:#666; border-top:1px solid #ddd;"));

    auto* pLay = new QVBoxLayout(this);
    pLay->setContentsMargins(0, 0, 0, 0);
    pLay->setSpacing(0);
    BuildFindUi(pLay);   // 2026.09.22 顶部查找工具栏 + 可展开查找栏
    pLay->addWidget(m_pStack, 1);
    pLay->addWidget(m_pStat);
}

// 2026.09.22 顶部工具栏（常驻「查找」按钮）+ 查找栏（Ctrl+F/按钮展开，Esc 关闭）。
//   执行链：按钮/回车 → OnFindTriggered → FindText（当前视图分发）。选项：
//   ↑上一个/↓下一个（↓缺省，回车=↓、Shift+回车=↑）、区分大小写（缺省关）、
//   循环查找（缺省关，关时到底未命中即报「未找到」）。
void CFilePreviewWidget::BuildFindUi(QVBoxLayout* a_pLay) {
    // —— 工具栏（常驻） ——
    m_pToolBar = new QWidget(this);
    auto* pTbLay = new QHBoxLayout(m_pToolBar);
    pTbLay->setContentsMargins(4, 2, 4, 2);
    pTbLay->setSpacing(4);
    QFont fTb = m_pToolBar->font();
    fTb.setPointSize(11);
    m_pToolBar->setFont(fTb);

    m_pBtnFind = new QToolButton(m_pToolBar);
    m_pBtnFind->setIcon(MenuIcon(QStringLiteral("ow-search")));
    m_pBtnFind->setText(QStringLiteral("查找"));
    m_pBtnFind->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_pBtnFind->setAutoRaise(true);
    m_pBtnFind->setToolTip(QStringLiteral(
        "打开查找栏（快捷键 Ctrl+F）"));
    pTbLay->addWidget(m_pBtnFind);
    pTbLay->addStretch(1);
    connect(m_pBtnFind, &QToolButton::clicked, this, &CFilePreviewWidget::OnFindOpen);
    // Ctrl+F（WidgetWithChildrenShortcut：焦点在本预览页内任一子控件时可用）。
    //   唯一快捷键源（不在按钮上再设 setShortcut，防歧义双触发）。
    auto* pScFind = new QShortcut(QKeySequence::Find, this);
    pScFind->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pScFind, &QShortcut::activated, this, &CFilePreviewWidget::OnFindOpen);

    // —— 查找栏（初始隐藏） ——
    m_pFindBar = new QWidget(this);
    auto* pFLay = new QHBoxLayout(m_pFindBar);
    pFLay->setContentsMargins(4, 2, 4, 2);
    pFLay->setSpacing(4);
    m_pFindBar->setFont(fTb);

    pFLay->addWidget(new QLabel(QStringLiteral("查找:"), m_pFindBar));
    m_pEditFind = new QLineEdit(m_pFindBar);
    m_pEditFind->setPlaceholderText(
        QStringLiteral("输入内容，回车下一个，Shift+回车上一个"));
    m_pEditFind->setClearButtonEnabled(true);
    m_pEditFind->setMinimumWidth(180);
    pFLay->addWidget(m_pEditFind, 1);

    m_pChkCase = new QCheckBox(QStringLiteral("区分大小写"), m_pFindBar);
    m_pChkCase->setToolTip(QStringLiteral("勾选后按大小写精确匹配（缺省不区分）"));
    pFLay->addWidget(m_pChkCase);
    m_pChkWrap = new QCheckBox(QStringLiteral("循环"), m_pFindBar);
    m_pChkWrap->setToolTip(
        QStringLiteral("到底后回卷到另一端继续查找（缺省不循环，到底即停）"));
    pFLay->addWidget(m_pChkWrap);

    m_pBtnPrev = new QPushButton(QStringLiteral("↑ 上一个"), m_pFindBar);
    m_pBtnPrev->setToolTip(QStringLiteral("向上查找（Shift+回车）"));
    pFLay->addWidget(m_pBtnPrev);
    m_pBtnNext = new QPushButton(QStringLiteral("↓ 下一个"), m_pFindBar);
    m_pBtnNext->setToolTip(QStringLiteral("向下查找（缺省方向，回车）"));
    m_pBtnNext->setDefault(false);
    pFLay->addWidget(m_pBtnNext);

    m_pLblFindSt = new QLabel(m_pFindBar);
    m_pLblFindSt->setStyleSheet(QStringLiteral("color:#c0392b;"));
    pFLay->addWidget(m_pLblFindSt);

    auto* pBtnClose = new QToolButton(m_pFindBar);
    pBtnClose->setText(QStringLiteral("✕"));
    pBtnClose->setToolTip(QStringLiteral("关闭查找栏（Esc）"));
    pBtnClose->setAutoRaise(true);
    pFLay->addWidget(pBtnClose);

    m_pFindBar->hide();
    connect(m_pEditFind, &QLineEdit::returnPressed, this,
            [this]() { OnFindTriggered(false); });
    // Shift+回车 = 上一个（QLineEdit 无原生修饰键信号，快捷键补位）。
    auto* pScPrev = new QShortcut(QKeySequence(QStringLiteral("Shift+Return")), m_pEditFind);
    pScPrev->setContext(Qt::WidgetShortcut);
    connect(pScPrev, &QShortcut::activated, this, [this]() { OnFindTriggered(true); });
    auto* pScEsc = new QShortcut(QKeySequence(Qt::Key_Escape), m_pFindBar);
    pScEsc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pScEsc, &QShortcut::activated, this, [this]() {
        m_pFindBar->hide();
        m_pLblFindSt->clear();
        // 焦点还给当前视图（继续滚动/复制等键盘操作）。
        if (m_pStack->currentWidget() != nullptr) {
            m_pStack->currentWidget()->setFocus();
        }
    });
    connect(pBtnClose, &QToolButton::clicked, this, [this]() {
        m_pFindBar->hide();
        m_pLblFindSt->clear();
        if (m_pStack->currentWidget() != nullptr) {
            m_pStack->currentWidget()->setFocus();
        }
    });
    connect(m_pBtnPrev, &QPushButton::clicked, this, [this]() { OnFindTriggered(true); });
    connect(m_pBtnNext, &QPushButton::clicked, this, [this]() { OnFindTriggered(false); });
    // 查找词变化 → 清陈旧「未找到」提示（命中状态随词刷新）。
    connect(m_pEditFind, &QLineEdit::textChanged, this,
            [this]() { m_pLblFindSt->clear(); });

    a_pLay->addWidget(m_pToolBar);
    a_pLay->addWidget(m_pFindBar);
}

void CFilePreviewWidget::OnFindOpen() {
    m_pFindBar->show();
    m_pEditFind->setFocus();
    m_pEditFind->selectAll();
}

void CFilePreviewWidget::OnFindTriggered(bool a_bBackward) {
    m_pLblFindSt->clear();
    const QString sText = m_pEditFind->text();
    if (sText.isEmpty()) {
        return;
    }
    if (!FindText(sText, a_bBackward, m_pChkCase->isChecked(), m_pChkWrap->isChecked())) {
        m_pLblFindSt->setText(QStringLiteral("未找到"));
    }
}

bool CFilePreviewWidget::FindText(const QString& a_sText, bool a_bBackward,
                                   bool a_bCase, bool a_bWrap) {
    QWidget* pCur = m_pStack->currentWidget();
    if (pCur == m_pHexWrap && m_pHexWrap != nullptr) {
        return static_cast<CFileHexView*>(m_pHexWrap)
            ->FindText(a_sText, a_bBackward, a_bCase, a_bWrap);
    }
    if (QTextBrowser* pView = qobject_cast<QTextBrowser*>(pCur)) {
        return FindInEditor(pView, a_sText, a_bBackward, a_bCase, a_bWrap);
    }
    return false;
}

void CFilePreviewWidget::ShowFile(const QString& a_sAbsPath) {
    m_sCurPath = a_sAbsPath;
    if (m_pLblFindSt != nullptr) {
        m_pLblFindSt->clear();   // 2026.09.22 换文件清陈旧「未找到」提示
    }
    const QFileInfo stInfo(a_sAbsPath);
    CFrwDevice stFile(a_sAbsPath);
    if (stInfo.isDir() || !stFile.open(QIODevice::ReadOnly)) {
        m_pText->setPlainText(QStringLiteral("无法读取文件：\n%1").arg(a_sAbsPath));
        m_pStat->setText(QString());
        m_pStack->setCurrentWidget(m_pText);
        return;
    }
    constexpr qint64 kiMaxBytes = 4 * 1024 * 1024;
    QByteArray aBytes = stFile.read(kiMaxBytes + 1);
    const bool bTrunc = aBytes.size() > kiMaxBytes;
    if (bTrunc) {
        aBytes.truncate(kiMaxBytes);
    }

    const QString sSuffix = stInfo.suffix().toLower();
    const QString sName = stInfo.fileName();

    // 二进制判定：已知二进制后缀，或字节流探测（空字节比例 > 10%）
    static const QSet<QString> s_aBinExt = {
        QStringLiteral("exe"), QStringLiteral("dll"), QStringLiteral("obj"), QStringLiteral("lib"),
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
        QStringLiteral("bmp"), QStringLiteral("ico"), QStringLiteral("zip"), QStringLiteral("7z"),
        QStringLiteral("rar"), QStringLiteral("gz"), QStringLiteral("tar"), QStringLiteral("pdf"),
        QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"), QStringLiteral("xlsx"),
        QStringLiteral("pdb"), QStringLiteral("ilk"), QStringLiteral("dmp"), QStringLiteral("bin"),
        QStringLiteral("dat"), QStringLiteral("db"), QStringLiteral("wtp"), QStringLiteral("mtp"),
    };
    const bool bBinExt = s_aBinExt.contains(sSuffix);
    int iZero = 0;
    for (char c : aBytes) { if (c == '\0') ++iZero; }
    bool bBinary = bBinExt || (!aBytes.isEmpty() && iZero * 10 > aBytes.size());

    QString sEnc, sText;
    if (!bBinary) {
        sEnc = DetectAndDecode(aBytes, sText, bBinary);
    }
    if (bBinary) {
        sEnc = QStringLiteral("二进制");
    }

    QString sStat = QStringLiteral("%1 · %2 字节").arg(sEnc).arg(aBytes.size());
    if (bTrunc) {
        sStat += QStringLiteral(" · 超过 4MB 已截断");
    }
    m_pStat->setText(sName.isEmpty() ? sStat : QStringLiteral("%1 · %2").arg(sName, sStat));

    if (bBinary) {
        ShowHex(aBytes);
        return;
    }
    if (bTrunc) {
        sText += QStringLiteral("\n\n……（文件超过 4MB，已截断显示）");
    }
    // 按后缀分发（md/html 带格式，其余纯文本）
    if (sSuffix == QStringLiteral("md") || sSuffix == QStringLiteral("markdown")) {
        m_pMd->setMarkdown(RewriteLocalUrls(sText, stInfo.absolutePath()));
        m_pStack->setCurrentWidget(m_pMd);
    } else if (sSuffix == QStringLiteral("html") || sSuffix == QStringLiteral("htm")) {
        m_pHtml->setHtml(RewriteLocalUrls(sText, stInfo.absolutePath()));
        m_pStack->setCurrentWidget(m_pHtml);
    } else {
        m_pText->setPlainText(sText);
        m_pStack->setCurrentWidget(m_pText);
    }
}

QString CFilePreviewWidget::DetectAndDecode(const QByteArray& a_aBytes, QString& a_sText, bool& a_bBinary) {
    // ① BOM：UTF-16LE/BE
    if (a_aBytes.size() >= 2) {
        if (uchar(a_aBytes[0]) == 0xFF && uchar(a_aBytes[1]) == 0xFE) {
            QStringDecoder d(QStringLiteral("UTF-16LE"));
            a_sText = d.decode(QByteArrayView(a_aBytes).sliced(2));
            return QStringLiteral("UTF-16LE BOM");
        }
        if (uchar(a_aBytes[0]) == 0xFE && uchar(a_aBytes[1]) == 0xFF) {
            QStringDecoder d(QStringLiteral("UTF-16BE"));
            a_sText = d.decode(QByteArrayView(a_aBytes).sliced(2));
            return QStringLiteral("UTF-16BE BOM");
        }
    }
    // ② BOM：UTF-8
    if (a_aBytes.size() >= 3 &&
        uchar(a_aBytes[0]) == 0xEF && uchar(a_aBytes[1]) == 0xBB && uchar(a_aBytes[2]) == 0xBF) {
        QStringDecoder d(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        a_sText = d.decode(QByteArrayView(a_aBytes).sliced(3));
        return QStringLiteral("UTF-8 BOM");
    }
    // ③ UTF-8 严格解码
    {
        QStringDecoder dUtf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        QString s = dUtf8.decode(a_aBytes);
        if (!dUtf8.hasError()) {
            a_sText = std::move(s);
            return QStringLiteral("UTF-8");
        }
    }
    // ④ GB18030（覆盖 GB2312/GBK/ANSI）：Qt6 无内建，用 Windows API 严格校验+转换。
    //   2026.08.26 含 '\0' 不走 GB 分支：NUL 在 GB18030 里合法，先命中会把空字节密集的
    //   未知格式当文本解出，应在 ⑤ 空字节比例判二进制之后再兜底。
#ifdef Q_OS_WIN
    if (!a_aBytes.contains('\0'))
    {
        const int nLen = int(a_aBytes.size());
        const int nW = MultiByteToWideChar(54936 /*GB18030*/, MB_ERR_INVALID_CHARS,
                                           a_aBytes.constData(), nLen, nullptr, 0);
        if (nW > 0 || (nW == 0 && nLen == 0)) {
            std::wstring sBuf(nW, L'\0');
            if (nW > 0) {
                MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS,
                                    a_aBytes.constData(), nLen, sBuf.data(), nW);
            }
            a_sText = QString::fromStdWString(sBuf);
            return QStringLiteral("GB18030/GBK");
        }
    }
#endif
    // ⑤ 空字节比例判二进制（UTF-8/GB 均非法的未知格式）
    {
        int iZero = 0;
        for (char c : a_aBytes) { if (c == '\0') ++iZero; }
        if (!a_aBytes.isEmpty() && iZero * 10 > a_aBytes.size()) {
            a_bBinary = true;
            a_sText.clear();
            return QString();
        }
    }
    // ⑥ Latin1 兜底（不丢字节）
    a_sText = QString::fromLatin1(a_aBytes);
    return QStringLiteral("Latin1");
}

QString CFilePreviewWidget::RewriteLocalUrls(const QString& a_sText, const QString& a_sBaseDir) {
    // 相对路径 src/href → file:/// 绝对路径（QTextBrowser 以文档 URL 为基，
    // 这里未设 documentUrl，统一重写让本地图片可见）。Windows 路径转 forward slash。
    const QDir stBase(a_sBaseDir);
    QString sResult = a_sText;
    static const QRegularExpression s_reSrc(
        QStringLiteral("(src|href)\\s*=\\s*[\"']([^\"':]+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = s_reSrc.globalMatch(sResult);
    QString sOut;
    qsizetype iPos = 0;
    while (it.hasNext()) {
        const auto m = it.next();
        const QString sPath = m.captured(2);
        if (QFileInfo(sPath).isAbsolute() || sPath.startsWith(u'/')) {
            continue;
        }
        const QString sAbs = QDir::cleanPath(stBase.absoluteFilePath(sPath));
        QString sUrl = QDir::toNativeSeparators(sAbs).replace(u'\\', u'/');
        sUrl = QStringLiteral("file:///") + sUrl;
        sOut += QStringView(sResult).mid(iPos, m.capturedStart(2) - iPos);
        sOut += sUrl;
        iPos = m.capturedEnd(2);
    }
    sOut += QStringView(sResult).mid(iPos);
    return sOut;
}

void CFilePreviewWidget::ShowHex(const QByteArray& a_aBytes) {
    QString sHex, sAsc;
    BuildHexPage(a_aBytes, 0, sHex, sAsc);
    static_cast<CFileHexView*>(m_pHexWrap)->SetContent(sHex, sAsc);
    if (a_aBytes.size() > kiHexPageSize) {
        m_pStat->setText(m_pStat->text() +
            QStringLiteral(" · 十六进制仅显示前 %1KB").arg(kiHexPageSize / 1024));
    }
    m_pStack->setCurrentWidget(m_pHexWrap);
}

// 2026-09-30 代码审查定位：滚动到 1-based 指定行并整行选中（文本/MD/HTML 视图；
// 十六进制无行号语义忽略）。当前栈页非文本类时优先在对应浏览器上定位（文件已加载）。
void CFilePreviewWidget::GotoLine(int a_iLine) {
    if (a_iLine < 1) {
        return;
    }
    QTextBrowser* pView = qobject_cast<QTextBrowser*>(m_pStack->currentWidget());
    if (pView == nullptr) {
        // 十六进制页等：切回纯文本页再定位（审查目标均为文本代码文件）。
        m_pStack->setCurrentWidget(m_pText);
        pView = m_pText;
        if (pView == nullptr) {
            return;
        }
    }
    QTextDocument* pDoc = pView->document();
    if (pDoc == nullptr || pDoc->blockCount() < 1) {
        return;
    }
    const int iLine = qMin(a_iLine, pDoc->blockCount());
    const QTextBlock stBlk = pDoc->findBlockByNumber(iLine - 1);
    if (!stBlk.isValid()) {
        return;
    }
    QTextCursor cur(pDoc);
    cur.setPosition(stBlk.position());
    cur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    pView->setTextCursor(cur);
    pView->ensureCursorVisible();
}
