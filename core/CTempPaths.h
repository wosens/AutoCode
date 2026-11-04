#pragma once
#include <QString>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCoreApplication>
#include <QAtomicInteger>

// 2026.10.28 临时文件统一根（用户指令）：AutoCode.exe 同级 ~Temperary\。
// 所有进程内临时文件（claude mcp 配置/模型 settings/node 探针/hermes 长 prompt
// 及后续新增）一律落这里，不再散落系统临时目录；SweepExpired(30) 启动时清掉
// 超 30 天的残留（覆盖崩溃/跳过收尾的泄漏），顺带清扫系统临时目录里历史遗留
// 的 autocode_* 老文件。头文件内联实现：各 agent runner 与测试目标零链接改动。
class CTempPaths {
public:
    // 根目录（无则创建；创建失败回退系统临时目录——功能优先不受阻）。
    // 2026-11-02 补 TryPrivateRoot：SweepExpired 的全量清扫只对确实可用的
    //   ~Temperary 私有根执行；回退到 %TEMP% 时绝不全量清（会误删其他程序
    //   的超期临时文件），只走 autocode_* 认领式清理。
    static QString Dir() {
        const QString sPriv = TryPrivateRoot();
        return !sPriv.isEmpty() ? sPriv : QDir::tempPath();
    }

    // 根内唯一文件全路径：前缀 + pid + 序号（撞名再探测 _0.._999，调用方拿到即可写）。
    // 回退态（私有根不可用）文件名统一加 autocode 标记，保证仍能被
    //   SweepLegacySystemTemp 认领清理，不产生永久残留。
    static QString FilePath(const QString& a_sPrefix, const QString& a_sSuffix) {
        static QAtomicInteger<int> s_iSeq { 0 };
        const QString sPriv = TryPrivateRoot();
        const QString sRoot = !sPriv.isEmpty() ? sPriv : QDir::tempPath();
        const QString sMark = sPriv.isEmpty() ? QStringLiteral("autocode") : QString();
        const QString sBase = sRoot + QLatin1Char('/') + sMark + a_sPrefix
                              + QString::number(QCoreApplication::applicationPid())
                              + QLatin1Char('_')
                              + QString::number(s_iSeq.fetchAndAddRelaxed(1) + 1);
        QString sPath = sBase + a_sSuffix;
        for (int i = 0; QFileInfo::exists(sPath) && i < 1000; ++i) {
            sPath = sBase + QLatin1Char('_') + QString::number(i) + a_sSuffix;
        }
        return sPath;
    }

    // 清理超期条目（根内文件删除、目录整体递归删除），返回清除数。
    //   逐条目按自身 lastModified 判定（目录 = 整体产物，不逐项内部深扫）。
    //   仅私有根可全量清扫（回退态只走 legacy 认领）；2026-11-02 改先收集后删除，
    //   避免枚举期间改动同目录导致 FindNextFile 跳条目。
    //   同时清扫系统临时目录里历史遗留的 autocode_* 老文件（改版前散落的）。
    static int SweepExpired(int a_iDays = 30) {
        const QDateTime oCut = QDateTime::currentDateTime().addDays(-a_iDays);
        int iN = 0;
        const QString sPriv = TryPrivateRoot();
        if (!sPriv.isEmpty()) {
            QStringList aVictims;
            QDirIterator it(sPriv, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            while (it.hasNext()) {
                const QString sPath = it.next();
                if (QFileInfo(sPath).lastModified() < oCut) {
                    aVictims.append(sPath);
                }
            }
            for (const QString& sPath : aVictims) {
                const QFileInfo oFi(sPath);
                if (oFi.isDir() ? QDir(sPath).removeRecursively()
                                : QFile::remove(sPath)) {
                    ++iN;
                }
            }
        }
        return iN + SweepLegacySystemTemp(oCut);
    }

private:
    static QString TryPrivateRoot() {
        const QString sDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/~Temperary");
        return QDir().mkpath(sDir) ? sDir : QString();
    }
    // 系统临时目录历史遗留的 autocode_* 老文件（改版前散落在临时根平铺的
    //   mcp/model/probe 及 QTemporaryFile 泄漏）：仅按 autocode 前缀认领，
    //   只扫顶层不递归（遗留全在根层；递归全 %TEMP% 会拖慢每次启动）。
    static int SweepLegacySystemTemp(const QDateTime& a_oCut) {
        int iN = 0;
        QDirIterator it(QDir::tempPath(), QStringList()
                        << QStringLiteral("autocode*"), QDir::Files);
        while (it.hasNext()) {
            const QString sPath = it.next();
            if (QFileInfo(sPath).lastModified() < a_oCut && QFile::remove(sPath)) {
                ++iN;
            }
        }
        return iN;
    }
};
