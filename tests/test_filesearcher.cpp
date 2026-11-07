// test_filesearcher.cpp —— CFileSearcher 单元测试（2026-11-07 编辑→查找/替换）。
// 纯逻辑 + 临时目录：匹配三开关、多根遍历、排除目录、编码探测/原样回写、
// 替换漂移校验。不依赖 GUI。
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>   // GB18030 档生成（WideCharToMultiByte 54936）
#endif

#include "core/CFileSearcher.h"

class CTestFileSearcher : public QObject {
    Q_OBJECT

private:
    static void WriteBytes(const QString& a_sPath, const QByteArray& a_aBytes) {
        QDir().mkpath(QFileInfo(a_sPath).absolutePath());
        QFile f(a_sPath);
        QVERIFY2(f.open(QIODevice::WriteOnly), qUtf8Printable(a_sPath));
        f.write(a_aBytes);
        f.close();
    }
    static QByteArray ReadBytes(const QString& a_sPath) {
        QFile f(a_sPath);
        if (!f.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        return f.readAll();
    }

private slots:
    // 三开关基础匹配：纯文本/大小写/全字/正则（行/列/长度/行文本口径）。
    void testMatchOptions();
    // CRLF 行尾下的行/列/行文本（'\r' 不入行文本、列不受影响）。
    void testCrLfPositions();
    // 多根遍历 + 排除目录（.git 等命中黑名单不进结果）+ 嵌套根去重。
    void testTraversalAndExcludes();
    // 二进制跳过：后缀黑名单 + 空字节比例启发。
    void testBinarySkip();
    // 编码往返：UTF-8 BOM / UTF-16LE BOM / GB18030 内容搜索命中。
    void testEncodings();
    // 替换：单点（原编码/行尾保持）+ 全部（同文件多处自后向前）+ 漂移校验拒绝。
    void testReplaceOneAndAll();
    // GB18030 文件替换后仍为 GB（中文不转 UTF-8）。
    void testReplaceGbKeepsEncoding();
    // 搜索取消：原子置位后即刻收工。
    void testCancel();
    // 2026-11-07 任务/助手日志通道：①默认排除 .autodev 时「全部」仍命中两根的
    //   assistant.json/chat.json/run.log；②日志-only（m_aRoots 空）不遍历源码树；
    //   ③排除清单放开后同一日志文件不重复上报（主遍历已扫的防重）。
    void testLogScope();
};

void CTestFileSearcher::testMatchOptions() {
    QTemporaryDir stTmp;
    const QString sRoot = stTmp.path();
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("a.txt")),
               QByteArrayLiteral("Hello hello HELLO heLLo\nworld hello_world\n"));

    auto fnSearch = [&sRoot](bool bCase, bool bWord, bool bRegex) {
        SFileSearchQuery stQ;
        stQ.m_sPattern = QStringLiteral("hello");
        stQ.m_bCase = bCase;
        stQ.m_bWord = bWord;
        stQ.m_bRegex = bRegex;
        stQ.m_aRoots = QStringList{sRoot};
        QList<SFindHit> aHits;
        SFileSearchStats stStats;
        CFileSearcher::Search(stQ, aHits, stStats);
        return aHits;
    };

    // 纯文本不敏感：4 处（不含 hello_world 里的 hello？——含！不敏感全词关=5 处）。
    QList<SFindHit> aHits = fnSearch(false, false, false);
    QCOMPARE(aHits.size(), 5);   // 4 行首词 + hello_world 前缀
    QCOMPARE(aHits.first().m_iLine, 1);
    QCOMPARE(aHits.first().m_iCol, 1);
    QCOMPARE(aHits.at(1).m_iCol, 7);
    QCOMPARE(aHits.last().m_iLine, 2);   // hello_world 里的 hello（列 7）
    // 2026-11-07 同串出现序（md/html 渲染定位用）：同文件同匹配文本【区分大小写
    //   的同串】递增（渲染 find 按原文精确匹配）——“Hello/hello/HELLO/heLLo”
    //   各为 0，末尾第二个小写 “hello” 为 1。
    QCOMPARE(aHits.at(0).m_iMatchOrd, 0);   // Hello
    QCOMPARE(aHits.at(1).m_iMatchOrd, 0);   // hello（首个小写）
    QCOMPARE(aHits.at(2).m_iMatchOrd, 0);   // HELLO
    QCOMPARE(aHits.at(3).m_iMatchOrd, 0);   // heLLo
    QCOMPARE(aHits.at(4).m_iMatchOrd, 1);   // hello_world 的 hello（第 2 个小写）

    // 区分大小写：仅小写 "hello"（行 1 列 7 + 行 2 hello_world 前缀，全字关）。
    aHits = fnSearch(true, false, false);
    QCOMPARE(aHits.size(), 2);
    QCOMPARE(aHits.at(0).m_iLine, 1);
    QCOMPARE(aHits.at(0).m_iCol, 7);
    QCOMPARE(aHits.at(1).m_iLine, 2);

    // 全字 + 不敏感：4 处（hello_world 的 hello 不算全字）。
    aHits = fnSearch(false, true, false);
    QCOMPARE(aHits.size(), 4);
    for (const SFindHit& stHit : aHits) {
        QVERIFY(!stHit.m_sLineText.contains(QStringLiteral("hello_world")));
    }

    // 正则：he(l|L)o 不敏感 → 与纯文本不敏感同 5 处；正则+全字 → 4 处。
    aHits = fnSearch(false, false, true);
    QCOMPARE(aHits.size(), 5);
    aHits = fnSearch(false, true, true);
    QCOMPARE(aHits.size(), 4);

    // 中文全字（Unicode 字母边界）：中文无空格分词，全字对 CJK 子串按字母边界
    //   仍生效（紧邻汉字=字母 → 非边界）。
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("zh.txt")),
               QStringLiteral("查找功能 查找\n搜索查找中\n").toUtf8());
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("查找");
    stQ.m_aRoots = QStringList{sRoot};
    QList<SFindHit> aHits2;
    SFileSearchStats stStats2;
    CFileSearcher::Search(stQ, aHits2, stStats2);
    QCOMPARE(aHits2.size(), 3);   // a.txt 已无该词；zh.txt 三处
    stQ.m_bWord = true;
    aHits2.clear();
    CFileSearcher::Search(stQ, aHits2, stStats2);
    QCOMPARE(aHits2.size(), 1);   // 仅「查找功能」处独立词（前后均为非字母数字）
}

void CTestFileSearcher::testCrLfPositions() {
    QTemporaryDir stTmp;
    const QString sFile = QDir(stTmp.path()).filePath(QStringLiteral("crlf.txt"));
    // "ab needle cd\r\nxy needle\r\n"（CRLF）
    WriteBytes(sFile, QByteArrayLiteral("ab needle cd\r\nxy needle\r\n"));
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{stTmp.path()};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 2);
    QCOMPARE(aHits.at(0).m_iLine, 1);
    QCOMPARE(aHits.at(0).m_iCol, 4);
    QCOMPARE(aHits.at(0).m_sLineText, QStringLiteral("ab needle cd"));
    QCOMPARE(aHits.at(1).m_iLine, 2);
    QCOMPARE(aHits.at(1).m_iCol, 4);
    QCOMPARE(aHits.at(1).m_sLineText, QStringLiteral("xy needle"));
}

void CTestFileSearcher::testTraversalAndExcludes() {
    QTemporaryDir stTmp;
    const QString sRoot = stTmp.path();
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("keep.txt")),
               QByteArrayLiteral("needle in root\n"));
    WriteBytes(QDir(sRoot).filePath(QStringLiteral(".git/obj.txt")),
               QByteArrayLiteral("needle in git\n"));
    WriteBytes(QDir(sRoot).filePath(QStringLiteral(".svn/a.txt")),
               QByteArrayLiteral("needle in svn\n"));
    WriteBytes(QDir(sRoot).filePath(QStringLiteral(".autodev/tasks/t1/a.txt")),
               QByteArrayLiteral("needle in autodev\n"));

    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{sRoot};
    stQ.m_aExcludeDirs = QStringList{QStringLiteral(".git"),
                                     QStringLiteral(".svn"),
                                     QStringLiteral(".autodev")};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 1);
    QVERIFY(aHits.first().m_sFilePath.endsWith(QStringLiteral("keep.txt")));

    // 嵌套根去重：根 + 其子目录作第二根 = 只扫一遍。
    QTemporaryDir stTmp2;
    const QString sSub = QDir(stTmp2.path()).filePath(QStringLiteral("sub"));
    WriteBytes(QDir(stTmp2.path()).filePath(QStringLiteral("one.txt")),
               QByteArrayLiteral("needle one\n"));
    WriteBytes(QDir(sSub).filePath(QStringLiteral("two.txt")),
               QByteArrayLiteral("needle two\n"));
    SFileSearchQuery stQ2;
    stQ2.m_sPattern = QStringLiteral("needle");
    stQ2.m_aRoots = QStringList{stTmp2.path(), sSub};
    QList<SFindHit> aHits2;
    SFileSearchStats stStats2;
    CFileSearcher::Search(stQ2, aHits2, stStats2);
    QCOMPARE(aHits2.size(), 2);   // 不因双根翻倍
}

void CTestFileSearcher::testBinarySkip() {
    QTemporaryDir stTmp;
    const QString sRoot = stTmp.path();
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("blob.bin")),
               QByteArrayLiteral("needle\x00\x01\x02needle\x00\x03needle"));
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("img.png")),
               QByteArrayLiteral("needle not searched"));
    WriteBytes(QDir(sRoot).filePath(QStringLiteral("plain.txt")),
               QByteArrayLiteral("needle\n"));
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{sRoot};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 1);   // 仅 plain.txt
    QVERIFY(aHits.first().m_sFilePath.endsWith(QStringLiteral("plain.txt")));
    QVERIFY(stStats.m_iFilesSkipped >= 2);   // bin（空字节比例）+ png（后缀）
}

void CTestFileSearcher::testEncodings() {
    QTemporaryDir stTmp;
    const QString sRoot = stTmp.path();
    // UTF-8 BOM
    {
        QByteArray aBytes = QByteArrayLiteral("\xEF\xBB\xBF");
        aBytes += QStringLiteral("针 needle u8\n").toUtf8();
        WriteBytes(QDir(sRoot).filePath(QStringLiteral("u8bom.txt")), aBytes);
    }
    // UTF-16LE BOM
    {
        const QString sText = QStringLiteral("针 needle u16\n");
        QByteArray aBytes = QByteArrayLiteral("\xFF\xFE");
        for (const QChar c : sText) {
            aBytes.append(char(c.unicode() & 0xFF));
            aBytes.append(char(c.unicode() >> 8));
        }
        WriteBytes(QDir(sRoot).filePath(QStringLiteral("u16.txt")), aBytes);
    }
    // GB18030（中文 GB 字节 + ASCII）
    {
        QByteArray aBytes;
        const QString sText = QStringLiteral("针 needle gb\n");
        // 简易 GB 编码：用 Windows API 不便，借 QStringDecoder 对拍不可行——
        //   直接落 UTF-8 中文 + ASCII 混合会走 UTF-8 分支；这里用纯 ASCII
        //   + GB 中文由 testReplaceGbKeepsEncoding 用系统 API 生成。
        aBytes = QByteArrayLiteral("needle gb-ascii\n");
        WriteBytes(QDir(sRoot).filePath(QStringLiteral("gb.txt")), aBytes);
    }
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{sRoot};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 3);
    // 中文前缀下的列：UTF-8 BOM 文件「针 needle」→ needle 在第 3 列（字符口径）。
    for (const SFindHit& stHit : aHits) {
        if (stHit.m_sFilePath.endsWith(QStringLiteral("u8bom.txt"))
            || stHit.m_sFilePath.endsWith(QStringLiteral("u16.txt"))) {
            QCOMPARE(stHit.m_iCol, 3);
            QVERIFY(stHit.m_sLineText.startsWith(QStringLiteral("针 ")));
        }
    }
}

void CTestFileSearcher::testReplaceOneAndAll() {
    QTemporaryDir stTmp;
    const QString sFile = QDir(stTmp.path()).filePath(QStringLiteral("rep.txt"));
    WriteBytes(sFile, QByteArrayLiteral("aa needle bb\r\ncc needle dd\r\nee needle ff\r\n"));
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{stTmp.path()};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 3);

    // 单点替换：第 2 处 → 内容正确 + CRLF 保持。
    QString sErr;
    QVERIFY(CFileSearcher::ReplaceOne(aHits.at(1), QStringLiteral("N"), sErr));
    QCOMPARE(ReadBytes(sFile),
             QByteArrayLiteral("aa needle bb\r\ncc N dd\r\nee needle ff\r\n"));

    // 漂移校验：伪造偏移的命中必须被拒绝（不盲写）。
    SFindHit stFake = aHits.at(2);
    stFake.m_iPos += 3;   // 错位
    QVERIFY(!CFileSearcher::ReplaceOne(stFake, QStringLiteral("X"), sErr));
    QVERIFY(!sErr.isEmpty());

    // 替换全部：重搜后全替换（同文件多处一次写回，CRLF 保持）。
    aHits.clear();
    CFileSearcher::Search(stQ, aHits, stStats);
    QStringList aErrs;
    QCOMPARE(CFileSearcher::ReplaceAll(aHits, QStringLiteral("[n]"), &aErrs), 2);
    QVERIFY(aErrs.isEmpty());
    QCOMPARE(ReadBytes(sFile),
             QByteArrayLiteral("aa [n] bb\r\ncc N dd\r\nee [n] ff\r\n"));
}

void CTestFileSearcher::testReplaceGbKeepsEncoding() {
#ifdef Q_OS_WIN
    // 用系统 API 生成真 GB18030 档（中文 needle 前缀），替换后仍为 GB 字节。
    const QString sText = QStringLiteral("中文 needle 内容\nsecond needle line\n");
    const int nLen = WideCharToMultiByte(54936, WC_ERR_INVALID_CHARS,
        reinterpret_cast<const wchar_t*>(sText.utf16()), int(sText.size()),
        nullptr, 0, nullptr, nullptr);
    QVERIFY(nLen > 0);
    QByteArray aGb(nLen, Qt::Uninitialized);
    WideCharToMultiByte(54936, WC_ERR_INVALID_CHARS,
        reinterpret_cast<const wchar_t*>(sText.utf16()), int(sText.size()),
        aGb.data(), nLen, nullptr, nullptr);
    QVERIFY(!aGb.contains('\0'));

    QTemporaryDir stTmp;
    const QString sFile = QDir(stTmp.path()).filePath(QStringLiteral("gb.txt"));
    WriteBytes(sFile, aGb);

    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{stTmp.path()};
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats);
    QCOMPARE(aHits.size(), 2);
    QCOMPARE(aHits.first().m_iCol, 4);   // 「中文 」= 3 字符 → needle 第 4 列

    QString sErr;
    QVERIFY(CFileSearcher::ReplaceOne(aHits.first(), QStringLiteral("针"), sErr));
    // 回读再按 GB 解码验证（若被写成 UTF-8，解码将失败或乱码）。
    const QByteArray aBack = ReadBytes(sFile);
    QString sDecoded;
    QVERIFY(CFileSearcher::DecodeText(aBack, sDecoded) == CFileSearcher::ETextEnc::Gb);
    QVERIFY(sDecoded.startsWith(QStringLiteral("中文 针")));
#endif
}

void CTestFileSearcher::testCancel() {
    QTemporaryDir stTmp;
    const QString sRoot = stTmp.path();
    for (int i = 0; i < 20; ++i) {
        WriteBytes(QDir(sRoot).filePath(QStringLiteral("f%1.txt").arg(i)),
                   QByteArrayLiteral("needle\n"));
    }
    SFileSearchQuery stQ;
    stQ.m_sPattern = QStringLiteral("needle");
    stQ.m_aRoots = QStringList{sRoot};
    std::atomic_bool bCancel{true};   // 预置取消：首检即收工
    QList<SFindHit> aHits;
    SFileSearchStats stStats;
    CFileSearcher::Search(stQ, aHits, stStats, &bCancel);
    QVERIFY(aHits.isEmpty());
}

void CTestFileSearcher::testLogScope() {
    // 夹具：工程根 + 关联根各一套日志（.autodev 布局）与源码文件；再放非白名单
    //   名文件（other.txt/plan.md）验证日志通道只枚举白名单名。
    QTemporaryDir stTmp;
    QTemporaryDir stTmp2;
    const QString sRoot1 = QDir(stTmp.path()).filePath(QStringLiteral("proj"));
    const QString sRoot2 = QDir(stTmp2.path()).filePath(QStringLiteral("linked"));
    WriteBytes(QDir(sRoot1).filePath(QStringLiteral("src.txt")),
               QByteArrayLiteral("needle src1\n"));
    WriteBytes(QDir(sRoot1).filePath(QStringLiteral(".autodev/assistant.json")),
               QByteArrayLiteral("needle asst-root1\n"));
    WriteBytes(QDir(sRoot1).filePath(QStringLiteral(".autodev/other.txt")),
               QByteArrayLiteral("needle other\n"));
    WriteBytes(QDir(sRoot1).filePath(
                   QStringLiteral(".autodev/tasks/t1/assistant.json")),
               QByteArrayLiteral("needle asst-t1\n"));
    WriteBytes(QDir(sRoot1).filePath(
                   QStringLiteral(".autodev/tasks/t1/chat.json")),
               QByteArrayLiteral("needle chat-t1\n"));
    WriteBytes(QDir(sRoot1).filePath(
                   QStringLiteral(".autodev/tasks/t1/run.log")),
               QByteArrayLiteral("needle run-t1\n"));
    WriteBytes(QDir(sRoot1).filePath(
                   QStringLiteral(".autodev/tasks/t1/verify_run.log")),
               QByteArrayLiteral("needle vrun-t1\n"));
    WriteBytes(QDir(sRoot1).filePath(
                   QStringLiteral(".autodev/tasks/t1/plan.md")),
               QByteArrayLiteral("needle plan-t1\n"));
    WriteBytes(QDir(sRoot2).filePath(QStringLiteral("src.txt")),
               QByteArrayLiteral("needle src2\n"));
    WriteBytes(QDir(sRoot2).filePath(QStringLiteral(".autodev/assistant.json")),
               QByteArrayLiteral("needle asst-root2\n"));
    WriteBytes(QDir(sRoot2).filePath(
                   QStringLiteral(".autodev/tasks/t9/chat.json")),
               QByteArrayLiteral("needle chat-t9\n"));

    // 白名单口径：四个规范名（顺序稳定，引擎日志通道枚举序）。
    QCOMPARE(CFileSearcher::LogFileNames(),
             QStringList({QStringLiteral("assistant.json"),
                          QStringLiteral("chat.json"),
                          QStringLiteral("run.log"),
                          QStringLiteral("verify_run.log")}));

    auto fnSearch = [&](bool a_bLogOnly, const QStringList& a_aExcl) {
        SFileSearchQuery stQ;
        stQ.m_sPattern = QStringLiteral("needle");
        stQ.m_aExcludeDirs = a_aExcl;
        stQ.m_aLogRoots = QStringList{
            QDir(sRoot1).filePath(QStringLiteral(".autodev")),
            QDir(sRoot2).filePath(QStringLiteral(".autodev")),
        };
        stQ.m_aLogFileNames = CFileSearcher::LogFileNames();
        if (!a_bLogOnly) {
            stQ.m_aRoots = QStringList{sRoot1, sRoot2};
        }
        QList<SFindHit> aHits;
        SFileSearchStats stStats;
        CFileSearcher::Search(stQ, aHits, stStats);
        return aHits;
    };

    // ① 范围「全部」+ 默认排除（含 .autodev）：源码 2 + 日志 7（根1×5、根2×2）
    //    均命中；排除目录内的 other.txt/plan.md 不进结果。
    {
        QList<SFindHit> aHits = fnSearch(
            false, QStringList{QStringLiteral(".git"),
                               QStringLiteral(".autodev")});
        QCOMPARE(aHits.size(), 9);
        QSet<QString> aFiles;
        for (const SFindHit& stHit : aHits) {
            aFiles.insert(stHit.m_sFilePath);
        }
        QCOMPARE(aFiles.size(), 9);
        QVERIFY(aFiles.contains(QDir::fromNativeSeparators(
            QDir(sRoot1).filePath(QStringLiteral(".autodev/assistant.json")))));
        QVERIFY(aFiles.contains(QDir::fromNativeSeparators(
            QDir(sRoot1).filePath(
                QStringLiteral(".autodev/tasks/t1/run.log")))));
        QVERIFY(aFiles.contains(QDir::fromNativeSeparators(
            QDir(sRoot2).filePath(
                QStringLiteral(".autodev/tasks/t9/chat.json")))));
    }

    // ② 日志-only（m_aRoots 空）：仅 7 个日志文件命中，源码树不遍历。
    {
        QList<SFindHit> aHits = fnSearch(
            true, QStringList{QStringLiteral(".autodev")});
        QCOMPARE(aHits.size(), 7);
        for (const SFindHit& stHit : aHits) {
            QVERIFY(stHit.m_sFilePath.contains(QStringLiteral(".autodev/")));
            QVERIFY(stHit.m_sFilePath.endsWith(QStringLiteral(".json"))
                    || stHit.m_sFilePath.endsWith(QStringLiteral("run.log")));
        }
    }

    // ③ 排除清单放开（不含 .autodev）：主遍历扫到日志文件，日志通道全部防重
    //    跳过——同一文件不重复上报（2 源码 + other + plan + 7 日志 = 11）。
    {
        QList<SFindHit> aHits = fnSearch(
            false, QStringList{QStringLiteral(".git")});
        QCOMPARE(aHits.size(), 11);
        const QString sT1Chat = QDir::fromNativeSeparators(
            QDir(sRoot1).filePath(QStringLiteral(".autodev/tasks/t1/chat.json")));
        int iCnt = 0;
        for (const SFindHit& stHit : aHits) {
            if (stHit.m_sFilePath == sT1Chat) {
                ++iCnt;
            }
        }
        QCOMPARE(iCnt, 1);
    }
}

QTEST_MAIN(CTestFileSearcher)
#include "test_filesearcher.moc"
