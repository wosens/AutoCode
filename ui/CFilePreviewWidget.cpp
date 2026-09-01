#include "CFilePreviewWidget.h"

#include <QStackedWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QScrollBar>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDir>

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

QPlainTextEdit* MakeMonoEdit(QWidget* a_pParent) {
    auto* p = new QPlainTextEdit(a_pParent);
    p->setReadOnly(true);
    p->setLineWrapMode(QPlainTextEdit::NoWrap);
    p->setTabChangesFocus(true);
    QFont f = p->font();
    f.setFamily(QStringLiteral("Consolas"));
    f.setPointSize(10);
    p->setFont(f);
    return p;
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
    m_pStack->addWidget(m_pText);

    // [1] Markdown
    m_pMd = new QTextBrowser(m_pStack);
    m_pMd->setReadOnly(true);
    m_pMd->setOpenExternalLinks(true);
    QFont fMd;
    fMd.setPointSize(11);
    m_pMd->setFont(fMd);
    m_pStack->addWidget(m_pMd);

    // [2] HTML
    m_pHtml = new QTextBrowser(m_pStack);
    m_pHtml->setReadOnly(true);
    m_pHtml->setOpenExternalLinks(true);
    m_pHtml->setFont(fMd);
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
    pLay->addWidget(m_pStack, 1);
    pLay->addWidget(m_pStat);
}

void CFilePreviewWidget::ShowFile(const QString& a_sAbsPath) {
    m_sCurPath = a_sAbsPath;
    const QFileInfo stInfo(a_sAbsPath);
    QFile stFile(a_sAbsPath);
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

QString CFilePreviewWidget::DetectAndDecode(const QByteArray& a_aBytes, QString& a_sText, bool& a_bBinary) const {
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
