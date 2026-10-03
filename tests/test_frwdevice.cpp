// test_frwdevice.cpp —— CFrwDevice（fileread.dll 适配器）离线单元测试（2026.09.13）
// 覆盖：三件套部署检出 / Text 模式 CRLF 转换与 QFile 逐字节对拍（纯 CRLF、孤立 \r、
// 尾部悬挂 \r、跨块悬挂）/ 二进制分块与 seek / QTextStream::readLine / 大文件（8MB）
// / UTF-8 中文路径 / 不存在文件同 QFile 失败语义。
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include "core/CFrwDevice.h"

class TestFrwDevice : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // 三件套随测试 exe 部署（CMake POST_BUILD 拷贝）；缺失即整个套件失败——
        // 兜底路径不是本测试的目标（可用性回归才是）。
        QVERIFY2(CFrwDevice::ServiceAvailable(),
                 "fileread.dll 未部署到测试 exe 目录（POST_BUILD 拷贝失效？）");
    }

    void textCrlfParity() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t1.txt");
        WriteBytes(sPath, "line1\r\nline2\r\nline3\r\n");
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(oRef.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray aFrw = oFrw.readAll();
        const QByteArray aRef = oRef.readAll();
        QCOMPARE(aFrw, aRef);
        QCOMPARE(aFrw, QByteArray("line1\nline2\nline3\n"));
        // size() 为原始字节快照（QFile 同口径；"line1\r\nline2\r\nline3\r\n" = 21B）
        QCOMPARE(oFrw.size(), qint64(21));
    }

    void textLoneCrParity() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t2.txt");
        // 孤立 \r、\r\n 对、裸 \n、EOF 前孤立 \r —— 全形态混排
        WriteBytes(sPath, "a\rb\r\nc\n\rd");
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(oRef.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(oFrw.readAll(), oRef.readAll());
    }

    void textTrailingCrEof() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t3.txt");
        WriteBytes(sPath, "xy\r");   // 尾部单 \r 到 EOF：补发孤立 \r
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(oRef.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(oFrw.readAll(), oRef.readAll());
        QCOMPARE(oFrw.readAll(), QByteArray());   // 第二次读：干净到头
    }

    void textCrHangAcrossChunks() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t4.txt");
        WriteBytes(sPath, "ab\r\ncd\r\nef");
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(oRef.open(QIODevice::ReadOnly | QIODevice::Text));
        // 2 字节小块：把 \r\n 对切在块边界（跨块悬挂路径）
        QByteArray aFrw, aRef;
        for (;;) { const QByteArray a = oFrw.read(2); if (a.isEmpty()) break; aFrw += a; }
        for (;;) { const QByteArray a = oRef.read(2); if (a.isEmpty()) break; aRef += a; }
        QCOMPARE(aFrw, aRef);
    }

    void binaryChunksAndSeek() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t5.bin");
        QByteArray aAll(4096, 0);
        for (int i = 0; i < aAll.size(); ++i) { aAll[i] = char(i % 251); }
        WriteBytes(sPath, aAll);
        CFrwDevice oFrw(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly));
        QCOMPARE(oFrw.size(), qint64(4096));
        QVERIFY(oFrw.seek(3000));                 // 尾段读取（Claude transcript 形态）
        QByteArray aTail = oFrw.readAll();
        QCOMPARE(aTail.size(), 1096);
        QCOMPARE(aTail, aAll.mid(3000));
        QVERIFY(oFrw.seek(0));                    // 回头再读
        QCOMPARE(oFrw.read(10), aAll.left(10));
        QVERIFY(oFrw.seek(4090));
        QCOMPARE(oFrw.read(100).size(), 6);       // 越界读截断
        QCOMPARE(oFrw.read(10), QByteArray());    // EOF 后读空
    }

    void textStreamReadLine() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t6.txt");
        WriteBytes(sPath, "first\r\nsecond\r\nthird\r\n");
        CFrwDevice oFrw(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QTextStream oTs(&oFrw);
        QCOMPARE(oTs.readLine(), QString("first"));
        QCOMPARE(oTs.readLine(), QString("second"));
        QCOMPARE(oTs.readLine(), QString("third"));
        QVERIFY(oTs.readLine().isEmpty());   // EOF 空行
    }

    void largeFileBinary() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t7.bin");
        QByteArray aBlk(65536, 0);
        for (int i = 0; i < aBlk.size(); ++i) { aBlk[i] = char(i * 7 % 256); }
        QFile oW(sPath);
        QVERIFY(oW.open(QIODevice::WriteOnly));
        for (int i = 0; i < 128; ++i) { oW.write(aBlk); }   // 8MB
        oW.close();
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(oFrw.open(QIODevice::ReadOnly));
        QVERIFY(oRef.open(QIODevice::ReadOnly));
        QCOMPARE(oFrw.size(), qint64(8388608));
        qint64 iOff = 0;
        while (!oFrw.atEnd() && !oRef.atEnd()) {
            const QByteArray a1 = oFrw.read(7777);
            const QByteArray a2 = oRef.read(7777);
            if (a1.isEmpty()) { break; }
            QCOMPARE(a1, a2);
            iOff += a1.size();
        }
        QCOMPARE(iOff, qint64(8388608));
        QVERIFY(oFrw.atEnd());
    }

    void utf8PathAndContent() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("中文路径测试.txt");
        const QByteArray aData = "中文内容：飞机大战\r\n第二行\r\n";
        WriteBytes(sPath, aData);
        CFrwDevice oFrw(sPath);
        QVERIFY(oFrw.exists());
        QVERIFY(oFrw.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(oFrw.readAll(), QByteArray("中文内容：飞机大战\n第二行\n"));
    }

    void missingFileParity() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("nope.txt");
        CFrwDevice oFrw(sPath);
        QFile oRef(sPath);
        QVERIFY(!oFrw.exists());
        QCOMPARE(oFrw.open(QIODevice::ReadOnly), false);
        QCOMPARE(oRef.open(QIODevice::ReadOnly), false);   // 双侧都失败（存在性已先证）
    }

    void writeModeRejected() {
        QTemporaryDir oDir;
        QVERIFY(oDir.isValid());
        const QString sPath = oDir.filePath("t9.txt");
        WriteBytes(sPath, "data");
        CFrwDevice oFrw(sPath);
        QVERIFY(!oFrw.open(QIODevice::WriteOnly));
        QVERIFY(!oFrw.open(QIODevice::ReadWrite));
        QVERIFY(!oFrw.open(QIODevice::Append));
        // 内容未被破坏
        QVERIFY(oFrw.open(QIODevice::ReadOnly));
        QCOMPARE(oFrw.readAll(), QByteArray("data"));
    }

private:
    static void WriteBytes(const QString& a_sPath, const QByteArray& a_aData) {
        QFile oW(a_sPath);
        QVERIFY(oW.open(QIODevice::WriteOnly));
        QCOMPARE(oW.write(a_aData), qint64(a_aData.size()));
        oW.close();
    }
};

QTEST_MAIN(TestFrwDevice)
#include "test_frwdevice.moc"
