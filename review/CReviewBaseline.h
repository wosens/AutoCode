#pragma once

// ============================================================================
// CReviewBaseline —— 手动代码审查「自上次审查以来修改」基线仓（2026-10-08）
// ----------------------------------------------------------------------------
// 配合审查窗口 Tab 改造（不勾「全量审查」= 仅列/仅审修改部分）：
//   - 基线 = 每个文件**上次审查成功时**的盘上内容快照（mtime/size/内容 SHA1）；
//   - 「修改过」判定：无基线条目=新增；mtime/size 与基线不符=修改；
//   - 增量审查取旧内容：按 SHA1 从 blob 仓读回原始字节（内容寻址，未变文件跨轮
//     自动去重，重复审查零冗余拷贝）；
//   - 落位：exe 同级 review_cache/<目标哈希>.bl/（meta.json + f/<sha1>），与结果
//     缓存同域不污染被审工程（CodeReview.exe 的 .review/ 方案的便携版）。
// 纯 header-only inline：CReviewService worker（写）与 CCodeReviewDialog（读）
//   共用，免入 CMake 源清单。
// 线程：worker 线程逐文件写（QSaveFile 原子替换）、主线程仅在空闲期读 meta；
//   同一对话框内两者不并发（清单只在开窗/切过滤时重建）。
// ============================================================================

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

// 单文件基线条目：上次审查成功时的盘上快照。
struct SReviewBaseEntry {
    qint64  iMtimeMs = 0;
    qint64  iSize    = 0;
    QString sSha1;      // 内容 SHA1 十六进制（40 位，blob 仓寻址键）
};

namespace CReviewBaseline {

// 基线表：键=工程相对路径小写（NormKey），值=上次审查成功时快照。
typedef QHash<QString, SReviewBaseEntry> TMap;

// meta 键统一：工程相对路径（正斜杠）小写（Windows 盘径大小写不敏感）。
inline QString NormKey(const QString& a_sRel) {
    return a_sRel.toLower();
}

inline QString MetaPath(const QString& a_sDir) {
    return a_sDir + QStringLiteral("/meta.json");
}

inline QString BlobPath(const QString& a_sDir, const QString& a_sSha1) {
    return a_sDir + QStringLiteral("/f/") + a_sSha1;
}

inline QHash<QString, SReviewBaseEntry> LoadMeta(const QString& a_sDir) {
    QHash<QString, SReviewBaseEntry> mapOut;
    QFile stFile(MetaPath(a_sDir));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return mapOut;   // 无基线（首次审查）——空表即「全部新增」
    }
    const QJsonObject oRoot = QJsonDocument::fromJson(stFile.readAll()).object();
    stFile.close();
    if (oRoot.value(QStringLiteral("version")).toInt() != 1) {
        return mapOut;
    }
    const QJsonObject oFiles = oRoot.value(QStringLiteral("files")).toObject();
    for (auto it = oFiles.begin(); it != oFiles.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        SReviewBaseEntry stE;
        stE.iMtimeMs = static_cast<qint64>(o.value(QStringLiteral("mt")).toDouble());
        stE.iSize    = static_cast<qint64>(o.value(QStringLiteral("sz")).toDouble());
        stE.sSha1    = o.value(QStringLiteral("sha")).toString();
        if (!stE.sSha1.isEmpty()) {
            mapOut.insert(NormKey(it.key()), stE);
        }
    }
    return mapOut;
}

inline bool SaveMeta(const QString& a_sDir, const QHash<QString, SReviewBaseEntry>& a_map) {
    QDir().mkpath(a_sDir);
    QJsonObject oFiles;
    for (auto it = a_map.begin(); it != a_map.end(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("mt"), static_cast<double>(it.value().iMtimeMs));
        o.insert(QStringLiteral("sz"), static_cast<double>(it.value().iSize));
        o.insert(QStringLiteral("sha"), it.value().sSha1);
        oFiles.insert(it.key(), o);
    }
    QJsonObject oRoot;
    oRoot.insert(QStringLiteral("version"), 1);
    oRoot.insert(QStringLiteral("files"), oFiles);
    QSaveFile stFile(MetaPath(a_sDir));
    if (!stFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stFile.write(QJsonDocument(oRoot).toJson(QJsonDocument::Compact));
    return stFile.commit();
}

inline QString Sha1Of(const QByteArray& a_byBytes) {
    return QString::fromLatin1(QCryptographicHash::hash(
        a_byBytes, QCryptographicHash::Sha1).toHex());
}

// 内容写 blob 仓（已存在即跳过——内容寻址天然去重）。失败静默（仅丢失增量能力）。
inline void WriteBlob(const QString& a_sDir, const QString& a_sSha1, const QByteArray& a_byBytes) {
    const QString sPath = BlobPath(a_sDir, a_sSha1);
    if (QFile::exists(sPath)) {
        return;
    }
    QDir().mkpath(a_sDir + QStringLiteral("/f"));
    QSaveFile stFile(sPath);
    if (stFile.open(QIODevice::WriteOnly)) {
        stFile.write(a_byBytes);
        stFile.commit();
    }
}

inline bool ReadBlob(const QString& a_sDir, const QString& a_sSha1, QByteArray& a_byOut) {
    QFile stFile(BlobPath(a_sDir, a_sSha1));
    if (!stFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    a_byOut = stFile.readAll();
    return true;
}

}   // namespace CReviewBaseline
