#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QDateTime>

// 单个文件的指纹（指纹表一行）。
struct SFileFingerprint {
    QString   sRelPath;          // 相对工程根的正斜杠路径（纯路径键，与 BOM/换行无关）
    qint64    iSize    = 0;      // 字节数
    QDateTime dtMtime;           // 最后修改时间（序列化为 UTC ms epoch）
    QString   sHash;             // 可选内容哈希（Q1 默认关；开启时填充，否则空/"-")
};

// 一次比对的结果（三类变化集合）。
struct SGuideChangeSet {
    QList<SFileFingerprint> aAdded;     // 指纹表里没有、磁盘上有的文件
    QList<SFileFingerprint> aModified;  // 指纹表里有、但 size/mtime/(hash) 变了的文件
    QStringList             aDeleted;   // 指纹表里有、磁盘上没有的文件（sRelPath）
};

// guide.md 文件指纹 + 增量变更跟踪扫描器（纯逻辑，无 Qt 控件、无 Q_OBJECT）。
// 依赖 Qt Core（QDir/QFile/QTextStream/QDateTime/QFileInfo），不调 Claude、不知晓状态机。
//
// 指纹表在 guide.md 中序列化为 fenced 代码块（介于 BEGIN/END 标记之间），变更日志段追加在块后：
//   <!-- AUTOCODE_FINGERPRINTS_BEGIN -->
//   ```autocode-fp
//   # 相对路径 | 字节数 | 修改时间(UTC ms) | hash(可选)
//   src/main.cpp | 2048 | 1755043200000 | -
//   ```
//   <!-- AUTOCODE_FINGERPRINTS_END -->
class CGuideFingerprintScanner {
public:
    // 指纹表 BEGIN/END 标记（定位 fenced 段，回写时只动此段 + 末尾追加变更日志）。
    static const QString s_sBeginMarker;   // "<!-- AUTOCODE_FINGERPRINTS_BEGIN -->"
    static const QString s_sEndMarker;     // "<!-- AUTOCODE_FINGERPRINTS_END -->"
    // 变更日志段标题（追加在指纹表之后）。
    static const QString s_sChangeLogHeader;

    // 排除目录（相对工程根，正斜杠）。集中常量，可扩展（design §0 Q5）。
    // 2026.08.15：默认清单可被 config.ini [Scan] ExcludeDirs 覆盖（SetExcludeDirs）。
    static QStringList& ExcludedDirs();
    // 覆盖默认清单；空清单 = 恢复内置默认（2026.08.18 语义修正，此前空被忽略）。
    static void SetExcludeDirs(const QStringList& a_aDirs);

    // 可选：是否启用内容哈希（design §0 Q1 默认 false）。运行期固定，预留开关。
    // 默认方案为 size+mtime，已知局限「改了又改回（相同大小+原 mtime）」会漏判，
    // 全量 BuildGuideAsync 仍可兜底，此开关供大工程/敏感场景开启。
    static bool s_bEnableHash;

    // 扫描工程目录，产出当前指纹列表（递归、按 ExcludedDirs 过滤）。
    // a_sRoot = 工程根绝对路径。失败（目录不存在）返回空表。仅取 QFileInfo（不读内容，NFR3）。
    static QList<SFileFingerprint> ScanProject(const QString& a_sRoot);

    // 从 guide.md 全文解析出既有指纹表。无表/格式错返回空表（不抛）。
    static QList<SFileFingerprint> ParseFingerprintTable(const QString& a_sGuideText);

    // 比对当前扫描结果与既有指纹表，得增/改/删三集合（FR1.2）。
    static SGuideChangeSet Diff(const QList<SFileFingerprint>& a_aPrev,
                                const QList<SFileFingerprint>& a_aCur);

    // 把新指纹表 + 一条变更日志写回 guide.md（FR1.3 增量回写）：
    //   - 替换 BEGIN..END 之间的指纹表（未变文件行逐字保留：见实现约定「行原样回填」）。
    //   - 追加一条变更日志（无变化则不追加，FR1.4）。
    //   - guide.md 不存在或无 BEGIN..END 段 → 返回 false（调用方走全量 BuildGuideAsync）。
    // 回写用临时文件 + QFile::rename 原子替换（R2）。返回 true=已落盘；false=IO 错误或无表可改。
    static bool WriteIncremental(const QString& a_sGuidePath,
                                 const QList<SFileFingerprint>& a_aCur,
                                 const SGuideChangeSet& a_stChanges,
                                 const QString& a_sTimestampIso);

private:
    // 把单条指纹序列化为一行指纹表文本：路径 | 字节数 | UTC ms | hash(-)
    static QString FormatFpLine(const SFileFingerprint& a_stFp);
    // 取文件 mtime 的 UTC 毫秒 epoch（序列化用）。
    static qint64 MtimeToUtcMs(const QDateTime& a_dt);
    // a_sPath 是否落在任一排除目录下（相对根，正斜杠比较）。
    static bool IsExcluded(const QString& a_sRelPath);
};
