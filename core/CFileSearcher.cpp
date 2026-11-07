// CFileSearcher.cpp —— 「在文件中查找/替换」引擎实现（2026-11-07）。
#include "CFileSearcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringEncoder>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ===========================================================================
// SFindMatcher —— 三开关归一的匹配器
// ===========================================================================
SFindMatcher SFindMatcher::Create(const SFileSearchQuery& a_stQuery, QString* a_pErr) {
    SFindMatcher stM;
    stM.m_bCase = a_stQuery.m_bCase;
    stM.m_bWord = a_stQuery.m_bWord;
    stM.m_bRegex = a_stQuery.m_bRegex;
    stM.m_sPattern = a_stQuery.m_sPattern;
    if (a_stQuery.m_sPattern.isEmpty()) {
        if (a_pErr != nullptr) {
            *a_pErr = QStringLiteral("查找串为空");
        }
        return stM;
    }
    if (a_stQuery.m_bRegex) {
        // 大小写不敏感=PatternOption（QRegularExpression 自身无运行期开关）。
        auto* pRe = new QRegularExpression(
            a_stQuery.m_sPattern,
            a_stQuery.m_bCase ? QRegularExpression::NoPatternOption
                              : QRegularExpression::CaseInsensitiveOption);
        if (!pRe->isValid()) {
            if (a_pErr != nullptr) {
                *a_pErr = QStringLiteral("正则表达式无效：%1")
                              .arg(pRe->errorString());
            }
            delete pRe;
            return stM;
        }
        stM.m_pRe = pRe;
    } else {
        // 纯文本/全字：indexOf 大小写档直查（Qt 内部按字符比较，不复制折叠串）。
        stM.m_sPattern = a_stQuery.m_sPattern;
    }
    stM.m_bValid = true;
    return stM;
}

SFindMatcher::~SFindMatcher() {
    delete m_pRe;
}

SFindMatcher::SFindMatcher(SFindMatcher&& a_stOther) noexcept
    : m_sPattern(std::move(a_stOther.m_sPattern))
    , m_pRe(a_stOther.m_pRe)
    , m_bValid(a_stOther.m_bValid)
    , m_bCase(a_stOther.m_bCase)
    , m_bWord(a_stOther.m_bWord)
    , m_bRegex(a_stOther.m_bRegex) {
    a_stOther.m_pRe = nullptr;
    a_stOther.m_bValid = false;
}

SFindMatcher& SFindMatcher::operator=(SFindMatcher&& a_stOther) noexcept {
    if (this != &a_stOther) {
        delete m_pRe;
        m_sPattern = std::move(a_stOther.m_sPattern);
        m_pRe = a_stOther.m_pRe;
        m_bValid = a_stOther.m_bValid;
        m_bCase = a_stOther.m_bCase;
        m_bWord = a_stOther.m_bWord;
        m_bRegex = a_stOther.m_bRegex;
        a_stOther.m_pRe = nullptr;
        a_stOther.m_bValid = false;
    }
    return *this;
}

// 全字边界字符（字母 / 数字 / 下划线；对 Unicode 字母数字生效）。
static bool FnIsWordChar(QChar a_c) {
    return a_c.isLetterOrNumber() || a_c == QLatin1Char('_');
}

bool SFindMatcher::FindNext(const QString& a_sText, qsizetype a_iFrom,
                            qsizetype& a_iPos0, qsizetype& a_iLen,
                            QString& a_sMatched) const {
    if (!m_bValid || a_iFrom < 0 || a_iFrom >= a_sText.size()) {
        return false;
    }
    if (m_bRegex) {
        // 整篇 globalMatch 从头迭代太浪费——match(subject, offset) 从指定偏移起。
        qsizetype iCur = a_iFrom;
        while (iCur <= a_sText.size()) {
            const QRegularExpressionMatch m = m_pRe->match(a_sText, iCur);
            if (!m.hasMatch()) {
                return false;
            }
            const qsizetype iPos = m.capturedStart(0);
            const qsizetype iLen = m.capturedLength(0);
            if (iLen > 0) {
                // 全字校验（正则模式同样尊重全字开关）。
                if (m_bWord) {
                    const bool bLeftOk = iPos == 0
                        || !FnIsWordChar(a_sText.at(iPos - 1));
                    const bool bRightOk = iPos + iLen >= a_sText.size()
                        || !FnIsWordChar(a_sText.at(iPos + iLen));
                    if (!bLeftOk || !bRightOk) {
                        iCur = iPos + 1;   // 边界不符：从命中首字符右移一位再试
                        continue;
                    }
                }
                a_iPos0 = iPos;
                a_iLen = iLen;
                a_sMatched = m.captured(0);
                return true;
            }
            // 零宽命中（如 "a*" 空匹配）：右移一位防死循环。
            iCur = iPos + 1;
        }
        return false;
    }
    // 纯文本 / 全字：indexOf 循环（大小写档由 CaseSensitivity 表达，无复制）。
    const Qt::CaseSensitivity eCs = m_bCase ? Qt::CaseSensitive
                                            : Qt::CaseInsensitive;
    const qsizetype iPatLen = m_sPattern.size();
    qsizetype iPos = a_sText.indexOf(m_sPattern, a_iFrom, eCs);
    while (iPos >= 0) {
        if (!m_bWord) {
            a_iPos0 = iPos;
            a_iLen = iPatLen;
            a_sMatched = a_sText.mid(iPos, iPatLen);
            return true;
        }
        const bool bLeftOk = iPos == 0 || !FnIsWordChar(a_sText.at(iPos - 1));
        const bool bRightOk = iPos + iPatLen >= a_sText.size()
            || !FnIsWordChar(a_sText.at(iPos + iPatLen));
        if (bLeftOk && bRightOk) {
            a_iPos0 = iPos;
            a_iLen = iPatLen;
            a_sMatched = a_sText.mid(iPos, iPatLen);
            return true;
        }
        iPos = a_sText.indexOf(m_sPattern, iPos + 1, eCs);
    }
    return false;
}

// ===========================================================================
// CFileSearcher
// ===========================================================================
QStringList CFileSearcher::LogFileNames() {
    // .autodev 布局的四个规范名：助手日志 assistant.json（根级+任务级）、
    //   任务对话 chat.json、任务运行 run.log + verify_run.log（Verify 阶段运行
    //   日志，2026-11-07 补——首版清单遗漏，实测工程有 6 个；一次构造进程内复用）。
    static const QStringList s_aNames = {
        QStringLiteral("assistant.json"),
        QStringLiteral("chat.json"),
        QStringLiteral("run.log"),
        QStringLiteral("verify_run.log"),
    };
    return s_aNames;
}

QString CFileSearcher::RegexError(const QString& a_sPattern, bool a_bCase) {
    if (a_sPattern.isEmpty()) {
        return QString();
    }
    QRegularExpression re(a_sPattern,
                          a_bCase ? QRegularExpression::NoPatternOption
                                  : QRegularExpression::CaseInsensitiveOption);
    return re.isValid() ? QString() : re.errorString();
}

CFileSearcher::ETextEnc CFileSearcher::DecodeText(const QByteArray& a_aBytes,
                                                  QString& a_sOut) {
    // ① BOM：UTF-16LE/BE
    if (a_aBytes.size() >= 2) {
        if (uchar(a_aBytes[0]) == 0xFF && uchar(a_aBytes[1]) == 0xFE) {
            QStringDecoder d(QStringLiteral("UTF-16LE"));
            a_sOut = d.decode(QByteArrayView(a_aBytes).sliced(2));
            return ETextEnc::Utf16LE;
        }
        if (uchar(a_aBytes[0]) == 0xFE && uchar(a_aBytes[1]) == 0xFF) {
            QStringDecoder d(QStringLiteral("UTF-16BE"));
            a_sOut = d.decode(QByteArrayView(a_aBytes).sliced(2));
            return ETextEnc::Utf16BE;
        }
    }
    // ② BOM：UTF-8
    if (a_aBytes.size() >= 3 &&
        uchar(a_aBytes[0]) == 0xEF && uchar(a_aBytes[1]) == 0xBB
        && uchar(a_aBytes[2]) == 0xBF) {
        QStringDecoder d(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        a_sOut = d.decode(QByteArrayView(a_aBytes).sliced(3));
        return ETextEnc::Utf8Bom;
    }
    // ③ UTF-8 严格（无 BOM）
    {
        QStringDecoder dUtf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        QString s = dUtf8.decode(a_aBytes);
        if (!dUtf8.hasError()) {
            a_sOut = std::move(s);
            return ETextEnc::Utf8;
        }
    }
#ifdef Q_OS_WIN
    // ④ GB18030（覆盖 GB2312/GBK/ANSI）：Windows API 严格校验；含 '\0' 不走
    //    GB 分支（NUL 在 GB18030 合法，会把空字节密集的未知格式误判为文本）。
    if (!a_aBytes.contains('\0')) {
        const int nLen = int(a_aBytes.size());
        const int nW = MultiByteToWideChar(54936 /*GB18030*/, MB_ERR_INVALID_CHARS,
                                           a_aBytes.constData(), nLen, nullptr, 0);
        if (nW > 0 || (nW == 0 && nLen == 0)) {
            std::wstring sBuf(nW, L'\0');
            if (nW > 0) {
                MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS,
                                    a_aBytes.constData(), nLen, sBuf.data(), nW);
            }
            a_sOut = QString::fromStdWString(sBuf);
            return ETextEnc::Gb;
        }
    }
#endif
    // ⑤ 空字节比例判二进制（UTF-8/GB 均非法的未知格式）
    {
        int iZero = 0;
        for (char c : a_aBytes) {
            if (c == '\0') {
                ++iZero;
            }
        }
        if (!a_aBytes.isEmpty() && iZero * 10 > a_aBytes.size()) {
            a_sOut.clear();
            return ETextEnc::Binary;
        }
    }
    // ⑥ Latin1 兜底（不丢字节）
    a_sOut = QString::fromLatin1(a_aBytes);
    return ETextEnc::Latin1;
}

bool CFileSearcher::EncodeText(const QString& a_sText, ETextEnc a_eEnc,
                               QByteArray& a_aOut) {
    switch (a_eEnc) {
    case ETextEnc::Utf8Bom: {
        a_aOut = QByteArrayLiteral("\xEF\xBB\xBF") + a_sText.toUtf8();
        return true;
    }
    case ETextEnc::Utf8:
        a_aOut = a_sText.toUtf8();
        return true;
    case ETextEnc::Utf16LE: {
        QStringEncoder e(QStringConverter::Utf16LE);
        a_aOut = QByteArrayLiteral("\xFF\xFE") + e.encode(a_sText);
        return !e.hasError();
    }
    case ETextEnc::Utf16BE: {
        QStringEncoder e(QStringConverter::Utf16BE);
        a_aOut = QByteArrayLiteral("\xFE\xFF") + e.encode(a_sText);
        return !e.hasError();
    }
#ifdef Q_OS_WIN
    case ETextEnc::Gb: {
        // GB18030 全 Unicode 可表达；WC_ERR_INVALID_CHARS 拦截孤立代理对等坏串。
        const int nLen = WideCharToMultiByte(54936, WC_ERR_INVALID_CHARS,
            reinterpret_cast<const wchar_t*>(a_sText.utf16()),
            int(a_sText.size()), nullptr, 0, nullptr, nullptr);
        if (nLen <= 0 && !a_sText.isEmpty()) {
            return false;
        }
        QByteArray aBuf(nLen, Qt::Uninitialized);
        if (nLen > 0) {
            WideCharToMultiByte(54936, WC_ERR_INVALID_CHARS,
                reinterpret_cast<const wchar_t*>(a_sText.utf16()),
                int(a_sText.size()), aBuf.data(), nLen, nullptr, nullptr);
        }
        a_aOut = aBuf;
        return true;
    }
#else
    case ETextEnc::Gb:
        Q_UNUSED(a_sText);
        return false;   // 非 Windows 平台无 GB 回写（与解码侧 #ifdef 对称）
#endif
    case ETextEnc::Latin1: {
        // 解码来源保证原字节 ≤0xFF；替换文本超出 Latin1 范围时拒绝写（防 '?' 丢字）。
        for (const QChar c : a_sText) {
            if (c.unicode() > 0xFF) {
                return false;
            }
        }
        a_aOut = a_sText.toLatin1();
        return true;
    }
    case ETextEnc::Binary:
        return false;
    }
    return false;
}

bool CFileSearcher::IsBinaryExt(const QString& a_sExtLower) {
    // 常见二进制产物后缀黑名单（一次构造，进程内复用）。
    static const QSet<QString> s_aBinExt = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("ico"),
        QStringLiteral("icns"), QStringLiteral("svgz"), QStringLiteral("psd"),
        QStringLiteral("ai"),   QStringLiteral("exe"),  QStringLiteral("dll"),
        QStringLiteral("lib"),  QStringLiteral("obj"),  QStringLiteral("o"),
        QStringLiteral("a"),    QStringLiteral("so"),   QStringLiteral("dylib"),
        QStringLiteral("pyd"),  QStringLiteral("node"), QStringLiteral("wasm"),
        QStringLiteral("class"), QStringLiteral("jar"), QStringLiteral("pdb"),
        QStringLiteral("ilk"),  QStringLiteral("idb"),  QStringLiteral("ipdb"),
        QStringLiteral("iobj"), QStringLiteral("exp"),  QStringLiteral("res"),
        QStringLiteral("zip"),  QStringLiteral("7z"),   QStringLiteral("rar"),
        QStringLiteral("gz"),   QStringLiteral("bz2"),  QStringLiteral("xz"),
        QStringLiteral("zst"),  QStringLiteral("tar"),  QStringLiteral("cab"),
        QStringLiteral("iso"),  QStringLiteral("msi"),  QStringLiteral("pdf"),
        QStringLiteral("doc"),  QStringLiteral("docx"), QStringLiteral("xls"),
        QStringLiteral("xlsx"), QStringLiteral("ppt"),  QStringLiteral("pptx"),
        QStringLiteral("mdb"),  QStringLiteral("accdb"), QStringLiteral("mp3"),
        QStringLiteral("mp4"),  QStringLiteral("avi"),  QStringLiteral("mkv"),
        QStringLiteral("mov"),  QStringLiteral("wmv"),  QStringLiteral("flv"),
        QStringLiteral("wav"),  QStringLiteral("flac"), QStringLiteral("ogg"),
        QStringLiteral("mid"),  QStringLiteral("ttf"),  QStringLiteral("otf"),
        QStringLiteral("woff"), QStringLiteral("woff2"), QStringLiteral("eot"),
        QStringLiteral("db"),   QStringLiteral("sqlite"), QStringLiteral("sqlite3"),
        QStringLiteral("wtp"),  QStringLiteral("mtp"),  QStringLiteral("bin"),
        QStringLiteral("pak"),  QStringLiteral("lnk"),  QStringLiteral("cur"),
        QStringLiteral("ani"),  QStringLiteral("blend"), QStringLiteral("fbx"),
    };
    return s_aBinExt.contains(a_sExtLower);
}

void CFileSearcher::SearchFile(const QString& a_sAbsPath,
                               const SFileSearchQuery& a_stQuery,
                               const SFindMatcher& a_stMatcher,
                               QList<SFindHit>& a_aHits, bool& a_bSkipped) {
    a_bSkipped = false;
    const QFileInfo stInfo(a_sAbsPath);
    if (stInfo.size() > s_kiMaxFileBytes) {
        a_bSkipped = true;   // 巨型产物（虚拟磁盘/转储等）不拖搜索
        return;
    }
    if (IsBinaryExt(stInfo.suffix().toLower())) {
        a_bSkipped = true;
        return;
    }
    QFile f(a_sAbsPath);
    if (!f.open(QIODevice::ReadOnly)) {
        a_bSkipped = true;
        return;
    }
    const QByteArray aBytes = f.readAll();
    f.close();
    QString sText;
    if (DecodeText(aBytes, sText) == ETextEnc::Binary) {
        a_bSkipped = true;
        return;
    }
    if (sText.isEmpty()) {
        return;   // 空文件无命中（不算跳过，算已扫描）
    }
    // 行起点表：lineStarts[i] = 第 i+1 行（1-based）的全篇字符偏移。
    QVector<qsizetype> aLineStarts;
    aLineStarts.reserve(64);
    aLineStarts.append(0);
    for (qsizetype i = 0; i < sText.size(); ++i) {
        if (sText.at(i) == QLatin1Char('\n')) {
            aLineStarts.append(i + 1);
        }
    }
    // 同文同串出现序计数（md/html 渲染定位用）。
    QHash<QString, int> aOrdCount;
    // 整篇枚举命中 → 折算行/列 + 行文本。
    qsizetype iPos = 0, iLen = 0;
    QString sMatched;
    qsizetype iCur = 0;
    while (a_stMatcher.FindNext(sText, iCur, iPos, iLen, sMatched)) {
        SFindHit stHit;
        stHit.m_sFilePath = QDir::fromNativeSeparators(a_sAbsPath);
        stHit.m_iPos = iPos;
        stHit.m_iLen = int(iLen);
        stHit.m_sMatched = sMatched;
        // 行号 = 「小于等于 iPos 的最后行起点」序 +1（二分：aLineStarts 升序）。
        const auto it = std::lower_bound(aLineStarts.cbegin(), aLineStarts.cend(),
                                         iPos);
        const int iLineIdx = int((it == aLineStarts.cend() || *it > iPos)
                                     ? (it - aLineStarts.cbegin() - 1)
                                     : (it - aLineStarts.cbegin()));
        stHit.m_iLine = iLineIdx + 1;
        stHit.m_iCol = int(iPos - aLineStarts.at(iLineIdx)) + 1;
        // 行文本：行起点到行尾（不含 '\n'，去 '\r'）。
        const qsizetype iLineBegin = aLineStarts.at(iLineIdx);
        const qsizetype iLineEnd = (iLineIdx + 1 < aLineStarts.size())
            ? (aLineStarts.at(iLineIdx + 1) - 1)   // 退过 '\n'
            : sText.size();
        QString sLine = sText.mid(iLineBegin, iLineEnd - iLineBegin);
        if (sLine.endsWith(QLatin1Char('\r'))) {
            sLine.chop(1);
        }
        stHit.m_sLineText = sLine;
        // 同一匹配文本的第几次出现（md/html 渲染定位用；见 SFindHit 注）。
        stHit.m_iMatchOrd = aOrdCount[sMatched]++;
        a_aHits.append(stHit);
        iCur = iPos + qMax<qsizetype>(iLen, 1);   // 防零长匹配死循环
        if (iLen == 0) {
            break;   // 零宽命中只记一条（FindNext 内部已右移，防御双保险）
        }
    }
    Q_UNUSED(a_stQuery);
}

// 根清单规整：cleanPath + 存在性过滤 + 嵌套根去重（互含的根只留大的那个，
//   防同一子树扫两遍）；保持调用方顺序（工程根在前）。
static QStringList FnNormalizeRoots(const QStringList& a_aRoots) {
    QStringList aOut;
    for (const QString& sRaw : a_aRoots) {
        QString sClean = QDir::cleanPath(
            QDir::fromNativeSeparators(sRaw.trimmed()));
        if (sClean.isEmpty()) {
            continue;
        }
        if (!QFileInfo(sClean).isDir()) {
            continue;   // 已被删除的关联根跳过
        }
        while (sClean.size() > 3 && sClean.endsWith(QLatin1Char('/'))) {
            sClean.chop(1);   // 去尾斜杠（保留 "C:/" 盘根形态）
        }
        bool bCovered = false;
        for (int i = aOut.size() - 1; i >= 0; --i) {
            const QString& sDone = aOut.at(i);
            // Windows 路径比较大小写不敏感（口径同工程内其他路径比较）。
            if (QString::compare(sDone, sClean, Qt::CaseInsensitive) == 0) {
                bCovered = true;   // 重复根：留先到的
                break;
            }
            if (sClean.size() > sDone.size()
                && sClean.startsWith(sDone + QLatin1Char('/'),
                                     Qt::CaseInsensitive)) {
                bCovered = true;   // 本根被已收录根包含 → 剔除
                break;
            }
            if (sDone.size() > sClean.size()
                && sDone.startsWith(sClean + QLatin1Char('/'),
                                    Qt::CaseInsensitive)) {
                aOut.removeAt(i);   // 已收录根被本根包含 → 让位给大的
            }
        }
        if (!bCovered) {
            aOut.append(sClean);
        }
    }
    return aOut;
}

// 日志根规整：cleanPath + 存在性过滤 + 重复去重（与搜索根分开规整——嵌套根
//   去重会吞掉「工程根/.autodev」这类内层根；日志根互不嵌套，去重即可）。
static QStringList FnNormalizeLogRoots(const QStringList& a_aLogRoots) {
    QStringList aOut;
    for (const QString& sRaw : a_aLogRoots) {
        QString sClean = QDir::cleanPath(
            QDir::fromNativeSeparators(sRaw.trimmed()));
        if (sClean.isEmpty() || !QFileInfo(sClean).isDir()) {
            continue;   // 无 .autodev 的根直接跳过
        }
        bool bDup = false;
        for (const QString& sDone : aOut) {
            if (QString::compare(sDone, sClean, Qt::CaseInsensitive) == 0) {
                bDup = true;   // 重复日志根：留先到的
                break;
            }
        }
        if (!bDup) {
            aOut.append(sClean);
        }
    }
    return aOut;
}

void CFileSearcher::Search(const SFileSearchQuery& a_stQuery, QList<SFindHit>& a_aHits,
                           SFileSearchStats& a_stStats, const std::atomic_bool* a_pCancel) {
    a_aHits.clear();
    a_stStats = SFileSearchStats();
    QString sErr;
    SFindMatcher stMatcher = SFindMatcher::Create(a_stQuery, &sErr);
    if (!stMatcher.Valid()) {
        return;   // 非法模式=零命中（调用方应已用 RegexError 预拦）
    }
    // 排除目录名集合（小写比较）。
    QSet<QString> aExcl;
    for (const QString& s : std::as_const(a_stQuery.m_aExcludeDirs)) {
        const QString sTrim = s.trimmed();
        if (!sTrim.isEmpty()) {
            aExcl.insert(sTrim.toLower());
        }
    }
    // 日志文件名白名单（原始序枚举 + 小写集比对；空=按 LogFileNames() 全量）。
    QStringList aLogNames = a_stQuery.m_aLogFileNames;
    if (aLogNames.isEmpty()) {
        aLogNames = LogFileNames();
    }
    QSet<QString> aLogNameSet;
    for (const QString& s : std::as_const(aLogNames)) {
        const QString sTrim = s.trimmed();
        if (!sTrim.isEmpty()) {
            aLogNameSet.insert(sTrim.toLower());
        }
    }
    // 主遍历已扫描的白名单名文件（排除清单放开时防日志通道二次上报同一文件）。
    QSet<QString> aScannedLogFiles;
    const QStringList aRoots = FnNormalizeRoots(a_stQuery.m_aRoots);
    const QStringList aLogRoots = FnNormalizeLogRoots(a_stQuery.m_aLogRoots);
    if (aRoots.isEmpty() && aLogRoots.isEmpty()) {
        return;
    }
    // 迭代式 DFS（显式栈；QDirIterator 无法按目录名剪枝，会先递进排除目录）。
    if (!aRoots.isEmpty()) {
        QStringList aStack = aRoots;
        while (!aStack.isEmpty()) {
            if (a_pCancel != nullptr && a_pCancel->load(std::memory_order_relaxed)) {
                return;
            }
            const QString sDir = aStack.takeLast();
            ++a_stStats.m_iDirs;
            const QFileInfoList aEntries = QDir(sDir).entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
            // 子目录先入栈后访问（LIFO 反转=子目录按字母序先于文件批处理，无碍——
            //   命中按文件主序分组呈现，同文件内偏移升序稳定）。
            for (const QFileInfo& stE : aEntries) {
                if (a_pCancel != nullptr
                    && a_pCancel->load(std::memory_order_relaxed)) {
                    return;
                }
                if (stE.isDir()) {
                    if (!aExcl.contains(stE.fileName().toLower())) {
                        aStack.append(stE.absoluteFilePath());
                    }
                } else if (stE.isFile()) {
                    if (aLogNameSet.contains(stE.fileName().toLower())) {
                        aScannedLogFiles.insert(QDir::fromNativeSeparators(
                            stE.absoluteFilePath()).toLower());
                    }
                    bool bSkipped = false;
                    SearchFile(stE.absoluteFilePath(), a_stQuery, stMatcher,
                               a_aHits, bSkipped);
                    if (bSkipped) {
                        ++a_stStats.m_iFilesSkipped;
                    } else {
                        ++a_stStats.m_iFilesScanned;
                    }
                }
            }
        }
    }
    // —— 任务/助手日志通道（查找范围「全部」/「任务和助手日志」）——
    //   日志在 <root>/.autodev 下：默认排除清单含 .autodev（主遍历不可达），且
    //   FnNormalizeRoots 的嵌套去重会吞内层根——故绕过排除清单独立枚举固定布局：
    //   <logroot>/assistant.json 与 <logroot>/tasks/<id>/{白名单名}，存在才扫。
    for (const QString& sLogRoot : aLogRoots) {
        if (a_pCancel != nullptr && a_pCancel->load(std::memory_order_relaxed)) {
            return;
        }
        ++a_stStats.m_iDirs;
        QDir stLogDir(sLogRoot);
        auto fnScanLogFile = [&](const QString& a_sPath) {
            QFileInfo stFi(a_sPath);
            if (!stFi.isFile()) {
                return;   // 布局里没有该文件（存在性检查）
            }
            const QString sNorm = QDir::fromNativeSeparators(
                stFi.absoluteFilePath());
            if (aScannedLogFiles.contains(sNorm.toLower())) {
                return;   // 主遍历已上报（排除清单放开时），防重不双报
            }
            bool bSkipped = false;
            SearchFile(sNorm, a_stQuery, stMatcher, a_aHits, bSkipped);
            if (bSkipped) {
                ++a_stStats.m_iFilesSkipped;
            } else {
                ++a_stStats.m_iFilesScanned;
            }
        };
        if (aLogNameSet.contains(QStringLiteral("assistant.json"))) {
            fnScanLogFile(stLogDir.filePath(QStringLiteral("assistant.json")));
        }
        const QFileInfoList aTaskDirs = QDir(
            stLogDir.filePath(QStringLiteral("tasks"))).entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
        for (const QFileInfo& stTd : aTaskDirs) {
            if (a_pCancel != nullptr
                && a_pCancel->load(std::memory_order_relaxed)) {
                return;
            }
            ++a_stStats.m_iDirs;
            for (const QString& sName : std::as_const(aLogNames)) {
                fnScanLogFile(QDir(stTd.absoluteFilePath()).filePath(
                    sName.trimmed()));
            }
        }
    }
}

// 读文件并解码（ReplaceOne/ReplaceAll 共用）。
static bool FnLoadDecode(const QString& a_sPath, QByteArray& a_aBytes,
                         QString& a_sText, CFileSearcher::ETextEnc& a_eEnc) {
    QFile f(a_sPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    a_aBytes = f.readAll();
    f.close();
    a_eEnc = CFileSearcher::DecodeText(a_aBytes, a_sText);
    return a_eEnc != CFileSearcher::ETextEnc::Binary;
}

// 编码 + 原子写回（QSaveFile：替换中途断电不留半截文件）。
static bool FnSaveEncoded(const QString& a_sPath, const QString& a_sText,
                          CFileSearcher::ETextEnc a_eEnc, QString& a_sErr) {
    QByteArray aOut;
    if (!CFileSearcher::EncodeText(a_sText, a_eEnc, aOut)) {
        a_sErr = QStringLiteral("替换文本无法按原编码（%1）写回，文件未改动")
                     .arg(a_eEnc == CFileSearcher::ETextEnc::Gb
                              ? QStringLiteral("GB18030")
                              : QStringLiteral("Latin1"));
        return false;
    }
    QSaveFile f(a_sPath);
    if (!f.open(QIODevice::WriteOnly)) {
        a_sErr = QStringLiteral("无法写入文件：%1").arg(f.errorString());
        return false;
    }
    if (f.write(aOut) != aOut.size()) {
        a_sErr = QStringLiteral("写入不完整：%1").arg(f.errorString());
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        a_sErr = QStringLiteral("落盘失败：%1").arg(f.errorString());
        return false;
    }
    return true;
}

bool CFileSearcher::ReplaceOne(const SFindHit& a_stHit, const QString& a_sReplace,
                               QString& a_sErr) {
    a_sErr.clear();
    QByteArray aBytes;
    QString sText;
    ETextEnc eEnc = ETextEnc::Binary;
    if (!FnLoadDecode(a_stHit.m_sFilePath, aBytes, sText, eEnc)) {
        a_sErr = QStringLiteral("读取失败或内容为二进制：%1").arg(a_stHit.m_sFilePath);
        return false;
    }
    // 命中漂移校验：搜索后文件被外部改动（行/列错位）时拒绝盲替换。
    if (a_stHit.m_iPos < 0 || a_stHit.m_iPos + a_stHit.m_iLen > sText.size()
        || QStringView(sText).mid(a_stHit.m_iPos, a_stHit.m_iLen)
               != QStringView(a_stHit.m_sMatched)) {
        a_sErr = QStringLiteral("文件内容已变化（命中处不再是原匹配），请重新搜索");
        return false;
    }
    QString sNew = sText.left(a_stHit.m_iPos) + a_sReplace
        + sText.mid(a_stHit.m_iPos + a_stHit.m_iLen);
    return FnSaveEncoded(a_stHit.m_sFilePath, sNew, eEnc, a_sErr);
}

int CFileSearcher::ReplaceAll(QList<SFindHit> a_aHits, const QString& a_sReplace,
                              QStringList* a_pErrs) {
    // 按文件分组（保持首见顺序），文件内按偏移降序应用（前面的偏移不受影响）。
    int iTotal = 0;
    QList<SFindHit>* pPending = &a_aHits;
    while (!pPending->isEmpty()) {
        const QString sFile = pPending->first().m_sFilePath;
        QList<SFindHit> aOfFile;
        for (int i = pPending->size() - 1; i >= 0; --i) {
            if (QString::compare(pPending->at(i).m_sFilePath, sFile,
                                 Qt::CaseInsensitive) == 0) {
                aOfFile.prepend(pPending->takeAt(i));
            }
        }
        if (aOfFile.isEmpty()) {
            break;   // 防御（不可能走到）
        }
        // 同文件内按 m_iPos 降序。
        std::sort(aOfFile.begin(), aOfFile.end(),
                  [](const SFindHit& a, const SFindHit& b) {
                      return a.m_iPos > b.m_iPos;
                  });
        QByteArray aBytes;
        QString sText;
        ETextEnc eEnc = ETextEnc::Binary;
        QString sErr;
        if (!FnLoadDecode(sFile, aBytes, sText, eEnc)) {
            if (a_pErrs != nullptr) {
                a_pErrs->append(QStringLiteral("读取失败：%1").arg(sFile));
            }
            continue;
        }
        int iDone = 0;
        for (const SFindHit& stHit : std::as_const(aOfFile)) {
            if (stHit.m_iPos < 0
                || stHit.m_iPos + stHit.m_iLen > sText.size()
                || QStringView(sText).mid(stHit.m_iPos, stHit.m_iLen)
                       != QStringView(stHit.m_sMatched)) {
                continue;   // 单点漂移：跳过该处，其余照常
            }
            sText.replace(stHit.m_iPos, stHit.m_iLen, a_sReplace);
            ++iDone;
        }
        if (iDone > 0) {
            if (FnSaveEncoded(sFile, sText, eEnc, sErr)) {
                iTotal += iDone;
            } else if (a_pErrs != nullptr) {
                a_pErrs->append(sErr + QStringLiteral("（%1）").arg(sFile));
            }
        }
    }
    return iTotal;
}
