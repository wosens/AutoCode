#include "CGuideFingerprintScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>

// —— 静态常量 ——
const QString CGuideFingerprintScanner::s_sBeginMarker =
    QStringLiteral("<!-- AUTOCODE_FINGERPRINTS_BEGIN -->");
const QString CGuideFingerprintScanner::s_sEndMarker =
    QStringLiteral("<!-- AUTOCODE_FINGERPRINTS_END -->");
const QString CGuideFingerprintScanner::s_sChangeLogHeader =
    QStringLiteral("## 变更日志（增量）");

// Q1：默认 size+mtime 方案（不读内容，NFR3）。开启哈希需自行在 ScanProject 补实现。
bool CGuideFingerprintScanner::s_bEnableHash = false;

// 缺省指纹扫描排除规则（2026.08.26 用户需求二次修正）：只排除隐藏文件/文件夹（名称以 '.'
// 开头，自然涵盖 .git / .vs / .gitignore 等），其余一律不排——.autodev 也入表（用户明确要求）。
// 唯一例外：.autodev/guide.md 是指纹表自身所在文件，不参与 Diff（否则每次回写都自我判定
// 修改→永久假变更；它在 Diff 内单独过滤，扫描层不排，见 Diff 注释）。
static QStringList DefaultScanExcludeDirs() {
    return {};
}

// 隐藏路径判定：相对路径任一层组件以 '.' 开头（即隐藏文件或位于隐藏文件夹内）→ 排除。
static bool IsHiddenRelPath(const QString& a_sRelPath) {
    int iStart = 0;
    const int n = a_sRelPath.size();
    for (int i = 0; i <= n; ++i) {
        if (i == n || a_sRelPath.at(i) == QLatin1Char('/')) {
            if (i > iStart && a_sRelPath.at(iStart) == QLatin1Char('.')) {
                return true;
            }
            iStart = i + 1;
        }
    }
    return false;
}

QStringList& CGuideFingerprintScanner::ExcludedDirs() {
    // 2026.08.15 便携配置：默认清单为 config.ini [Scan] ExcludeDirs 的兜底值；
    //   主窗口启动/设置变更时 SetExcludeDirs 覆盖（空清单=恢复默认）。
    static QStringList s_aExcluded = DefaultScanExcludeDirs();
    return s_aExcluded;
}

void CGuideFingerprintScanner::SetExcludeDirs(const QStringList& a_aDirs) {
    // 2026.08.15 [Scan] ExcludeDirs：覆盖默认清单（扫描在后台线程跑，
    //   主线程启动时/设置保存时写入，写读不同时发生，无锁可接受）。
    // 2026.08.18 语义修正：空清单 = 恢复内置默认（此前空被忽略，清空配置不生效）。
    ExcludedDirs() = a_aDirs.isEmpty() ? DefaultScanExcludeDirs() : a_aDirs;
}

qint64 CGuideFingerprintScanner::MtimeToUtcMs(const QDateTime& a_dt) {
    if (!a_dt.isValid()) {
        return 0;
    }
    return a_dt.toUTC().toMSecsSinceEpoch();
}

bool CGuideFingerprintScanner::IsExcluded(const QString& a_sRelPath) {
    // a_sRelPath 为正斜杠相对根路径。落在任一排除目录下 → 排除。
    // 比较前缀 "<dir>/" 或整路径 == "<dir>"，避免误伤同前缀名（如 buildfoo）。
    // 2026.08.26：隐藏文件/文件夹（含 .git/.vs）改为内置规则，与 [Scan] ExcludeDirs 无关。
    //   例外：首层 .autodev（本工具工作目录，点开头但用户明确要求不排除）不做隐藏判定，
    //   仅其内部更深层仍按隐藏规则（如 .autodev/.tmp/x）。
    if (IsHiddenRelPath(a_sRelPath)) {
        if (a_sRelPath != QStringLiteral(".autodev") &&
            !a_sRelPath.startsWith(QStringLiteral(".autodev/"))) {
            return true;
        }
        // .autodev 内部：只查第二层及以后的组件是否隐藏。
        const QString sInner = a_sRelPath.mid(QStringLiteral(".autodev/").size());
        if (!sInner.isEmpty() && IsHiddenRelPath(sInner)) {
            return true;
        }
    }
    for (const QString& sDir : ExcludedDirs()) {
        if (a_sRelPath == sDir ||
            a_sRelPath.startsWith(sDir + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

QString CGuideFingerprintScanner::FormatFpLine(const SFileFingerprint& a_stFp) {
    // 行格式：相对路径 | 字节数 | 修改时间(UTC ms) | hash(可选，无则 -)
    const QString sHash = a_stFp.sHash.isEmpty()
        ? QStringLiteral("-") : a_stFp.sHash;
    return QStringLiteral("%1 | %2 | %3 | %4")
        .arg(a_stFp.sRelPath)
        .arg(a_stFp.iSize)
        .arg(MtimeToUtcMs(a_stFp.dtMtime))
        .arg(sHash);
}

bool CGuideFingerprintScanner::HasAnyScannableFile(const QString& a_sRoot) {
    // 空工程快速探测：找到首个通过排除规则的文件即返回 true（提前退出）。
    // 用于 BuildGuideAsync 的空工程分支——大工程全量 ScanProject 枚举十万级文件
    // 会阻塞 UI 线程数秒（2026.08.26 卡死修复），空判只关心「有没有」，不需全表。
    const QDir stRoot(a_sRoot);
    if (!stRoot.exists()) {
        return false;
    }
    QDirIterator it(a_sRoot,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString sRel = stRoot.relativeFilePath(it.fileInfo().absoluteFilePath())
                                 .replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (!IsExcluded(sRel)) {
            // 空工程判定只关心用户源码：.autodev 是本工具自建工作目录（EnsureAutoDev 先建），
            // 不能把它的存在当成「工程非空」（2026.08.26 .autodev 不再排除后专门防护）。
            if (sRel == QStringLiteral(".autodev") ||
                sRel.startsWith(QStringLiteral(".autodev/"))) {
                continue;
            }
            return true;
        }
    }
    return false;
}

QList<SFileFingerprint> CGuideFingerprintScanner::ScanProject(const QString& a_sRoot) {
    QList<SFileFingerprint> aOut;
    const QDir stRoot(a_sRoot);
    if (!stRoot.exists()) {
        return aOut;   // 目录不存在 → 空表
    }
    // 递归迭代，仅文件（跳过目录自身）。QDirIterator 不跟进符号链接目录，避免环。
    QDirIterator it(a_sRoot,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo stFi = it.fileInfo();
        // 相对根的正斜杠路径（排除目录过滤的键）。
        const QString sRel = stRoot.relativeFilePath(stFi.absoluteFilePath())
                                 .replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (IsExcluded(sRel)) {
            continue;
        }
        SFileFingerprint stFp;
        stFp.sRelPath = sRel;
        stFp.iSize    = stFi.size();
        stFp.dtMtime  = stFi.lastModified();
        // s_bEnableHash 默认关；预留开启分支（当前不读内容，留空）。
        if (s_bEnableHash) {
            stFp.sHash = QStringLiteral("-");   // 哈希实现未启用，占位
        }
        aOut.append(stFp);
    }
    return aOut;
}

QList<SFileFingerprint> CGuideFingerprintScanner::ParseFingerprintTable(const QString& a_sGuideText) {
    QList<SFileFingerprint> aOut;
    if (a_sGuideText.isEmpty()) {
        return aOut;
    }
    // 定位 BEGIN..END 之间的 fenced 块。
    const int iBegin = a_sGuideText.indexOf(s_sBeginMarker);
    const int iEnd   = a_sGuideText.indexOf(s_sEndMarker);
    if (iBegin < 0 || iEnd <= iBegin) {
        return aOut;   // 无表或标记顺序错 → 空表
    }
    const QString sBlock = a_sGuideText.mid(iBegin + s_sBeginMarker.length(),
                                            iEnd - (iBegin + s_sBeginMarker.length()));
    // 逐行解析："路径 | 字节数 | 修改时间(UTC ms) | hash"。容错：非数据行（注释/空/fence）跳过。
    const QStringList aLines = sBlock.split(QLatin1Char('\n'));
    for (const QString& sRaw : aLines) {
        const QString sLine = sRaw.trimmed();
        if (sLine.isEmpty()) {
            continue;
        }
        if (sLine.startsWith(QLatin1Char('#'))) {
            continue;   // 注释行（如 # 相对路径 ...）
        }
        if (sLine.startsWith(QStringLiteral("```"))) {
            continue;   // fenced 代码块边界
        }
        const QStringList aParts = sLine.split(QLatin1Char('|'));
        if (aParts.size() < 3) {
            continue;   // 字段不足 → 容错跳过
        }
        SFileFingerprint stFp;
        stFp.sRelPath = aParts[0].trimmed();
        stFp.iSize    = aParts[1].trimmed().toLongLong();
        const qint64 iMs = aParts[2].trimmed().toLongLong();
        stFp.dtMtime  = (iMs > 0)
            ? QDateTime::fromMSecsSinceEpoch(iMs) : QDateTime();
        if (aParts.size() >= 4) {
            const QString sHash = aParts[3].trimmed();
            stFp.sHash = (sHash == QStringLiteral("-")) ? QString() : sHash;
        }
        if (!stFp.sRelPath.isEmpty()) {
            aOut.append(stFp);
        }
    }
    return aOut;
}

SGuideChangeSet CGuideFingerprintScanner::Diff(const QList<SFileFingerprint>& a_aPrev,
                                               const QList<SFileFingerprint>& a_aCur) {
    SGuideChangeSet stOut;

    // 2026.08.26 .autodev 不再缺省排除（用户需求）：但 .autodev/guide.md 是指纹表自身，
    //   写入动作本身就会改它，参与 Diff 会产生永久假变更（每次同步都报"修改：guide.md"
    //   并重写文件，永不收敛）。这里统一过滤：guide.md 不适 Added/Modified/Deleted，
    //   也不进指纹表（其余 .autodev 文件正常跟踪，写入后收敛）。
    auto isSelfFile = [](const QString& s) {
        return s == QStringLiteral(".autodev/guide.md");
    };

    // prev 按路径建索引（保序：相同路径取首个）。
    QHash<QString, const SFileFingerprint*> mapPrev;
    for (const SFileFingerprint& rFp : a_aPrev) {
        mapPrev.insert(rFp.sRelPath, &rFp);
    }
    // cur 按路径建索引。
    QHash<QString, const SFileFingerprint*> mapCur;
    for (const SFileFingerprint& rFp : a_aCur) {
        mapCur.insert(rFp.sRelPath, &rFp);
    }

    // 遍历 cur：prev 无 → Added；prev 有但指纹变 → Modified。
    for (const SFileFingerprint& rFp : a_aCur) {
        if (isSelfFile(rFp.sRelPath)) {
            continue;
        }
        const auto itPrev = mapPrev.constFind(rFp.sRelPath);
        if (itPrev == mapPrev.constEnd()) {
            stOut.aAdded.append(rFp);
        } else {
            const SFileFingerprint* pPrev = itPrev.value();
            const bool bSame = (pPrev->iSize == rFp.iSize &&
                                MtimeToUtcMs(pPrev->dtMtime) == MtimeToUtcMs(rFp.dtMtime) &&
                                pPrev->sHash == rFp.sHash);
            if (!bSame) {
                stOut.aModified.append(rFp);
            }
        }
    }
    // 遍历 prev：cur 无 → Deleted。
    for (const SFileFingerprint& rFp : a_aPrev) {
        if (isSelfFile(rFp.sRelPath)) {
            continue;
        }
        if (!mapCur.contains(rFp.sRelPath)) {
            stOut.aDeleted.append(rFp.sRelPath);
        }
    }
    return stOut;
}

bool CGuideFingerprintScanner::WriteIncremental(const QString& a_sGuidePath,
                                                const QList<SFileFingerprint>& a_aCur,
                                                const SGuideChangeSet& a_stChanges,
                                                const QString& a_sTimestampIso) {
    // 读 guide.md 全文。文件不存在 → 调用方走全量流程。
    QFile stIn(a_sGuidePath);
    if (!stIn.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stStream(&stIn);
    const QString sGuide = stStream.readAll();
    stIn.close();

    const int iBegin = sGuide.indexOf(s_sBeginMarker);
    const int iEnd   = sGuide.indexOf(s_sEndMarker, iBegin >= 0 ? iBegin + s_sBeginMarker.length() : 0);
    if (iBegin < 0 || iEnd <= iBegin) {
        return false;   // 无 BEGIN..END 段 → 调用方走全量
    }

    // —— §3.1.4「未变文件逐字节不变」实现：未变行从原文逐字复制 ——
    // 1) 取 fenced 块原文（含表体，不含 BEGIN/END 标记），按行切分，建「路径→原始行字符串」索引。
    //    原始行保留原文（含可能的尾部空白），保证逐字节复现。
    const int iBlockStart = iBegin + s_sBeginMarker.length();
    const QString sBlock = sGuide.mid(iBlockStart, iEnd - iBlockStart);
    QHash<QString, QString> mapOrigLine;   // 路径 → 原始行字符串
    const QStringList aBlockLines = sBlock.split(QLatin1Char('\n'));
    for (const QString& sRaw : aBlockLines) {
        const QString sLine = sRaw.trimmed();
        if (sLine.isEmpty() || sLine.startsWith(QLatin1Char('#')) ||
            sLine.startsWith(QStringLiteral("```"))) {
            continue;
        }
        const int iSep = sLine.indexOf(QLatin1Char('|'));
        if (iSep > 0) {
            mapOrigLine.insert(sLine.left(iSep).trimmed(), sRaw);
        }
    }

    // 2) aPrev（保序）用于决定行的输出顺序：未变行原样回填；变化行写新指纹；删除行跳过。
    //    a_aCur 按路径索引，供查指纹是否变化 / 路径是否仍存在。
    QHash<QString, const SFileFingerprint*> mapCur;
    for (const SFileFingerprint& rFp : a_aCur) {
        mapCur.insert(rFp.sRelPath, &rFp);
    }
    const QList<SFileFingerprint> aPrev = ParseFingerprintTable(sGuide);

    // 构造新表体行序列：先按 prev 顺序输出（未变→原文逐字；变化→新行；删除→跳过），
    // 再把 aAdded（新文件）追加到末尾，保持「增量补充」语义。
    QStringList aNewBodyLines;
    QSet<QString> setEmitted;   // 已输出的路径（防 aAdded 与 prev 重复）
    for (const SFileFingerprint& rPrev : aPrev) {
        // 2026.08.26 指纹表自身（.autodev/guide.md）不入表：旧表残留行也不回填（与 Diff 过滤同步）。
        if (rPrev.sRelPath == QStringLiteral(".autodev/guide.md")) {
            continue;
        }
        const auto itCur = mapCur.constFind(rPrev.sRelPath);
        if (itCur == mapCur.constEnd()) {
            continue;   // 已删除，跳过（不进新表）
        }
        const SFileFingerprint* pCur = itCur.value();
        const bool bSame = (rPrev.iSize == pCur->iSize &&
                            MtimeToUtcMs(rPrev.dtMtime) == MtimeToUtcMs(pCur->dtMtime) &&
                            rPrev.sHash == pCur->sHash);
        if (bSame) {
            // 原样回填原文行（逐字节保留，AC1.4）。
            const QString sOrig = mapOrigLine.value(rPrev.sRelPath);
            aNewBodyLines.append(sOrig.isEmpty() ? FormatFpLine(*pCur) : sOrig);
        } else {
            aNewBodyLines.append(FormatFpLine(*pCur));   // 变化行写新指纹
        }
        setEmitted.insert(rPrev.sRelPath);
    }
    // 追加新增文件行（aAdded 顺序）。
    for (const SFileFingerprint& rAdded : a_stChanges.aAdded) {
        if (!setEmitted.contains(rAdded.sRelPath)) {
            aNewBodyLines.append(FormatFpLine(rAdded));
            setEmitted.insert(rAdded.sRelPath);
        }
    }

    // 3) 组装新的 fenced 块（BEGIN + ```autocode-fp + 表头 + 表体 + ``` + END）。
    QString sNewBlock;
    sNewBlock += QStringLiteral("\n") + s_sBeginMarker + QStringLiteral("\n");
    sNewBlock += QStringLiteral("```autocode-fp\n");
    sNewBlock += QStringLiteral("# 相对路径 | 字节数 | 修改时间(UTC ms) | hash(可选)\n");
    // 不排序：未变行按 aPrev 顺序逐字回填，新增行追加末尾——满足 AC1.4「无变化逐字节不变」
    // （design §3.1.4：禁用整表 sort 后重写，排序会重排所有行破坏逐字节不变性）。
    sNewBlock += aNewBodyLines.join(QLatin1Char('\n'));
    if (!aNewBodyLines.isEmpty()) {
        sNewBlock += QLatin1Char('\n');
    }
    sNewBlock += QStringLiteral("```\n");
    sNewBlock += s_sEndMarker + QStringLiteral("\n");

    // 4) 替换原文中的 BEGIN..END 段（含其前导换行），其余描述段字节级保留。
    //    从 iBegin（含前导换行）到 iEnd + END 长度（含尾随换行）整段替换。
    int iCutFrom = iBegin;
    // 吸收 BEGIN 前的一个换行（使新块自然衔接，避免多出空行）。
    if (iCutFrom > 0 && sGuide.at(iCutFrom - 1) == QLatin1Char('\n')) {
        --iCutFrom;
    }
    int iCutTo = iEnd + s_sEndMarker.length();
    // 吸收 END 后的一个换行（若存在），由新块自带的尾随换行接续。
    if (iCutTo < sGuide.length() && sGuide.at(iCutTo) == QLatin1Char('\n')) {
        ++iCutTo;
    }
    QString sNewGuide = sGuide;
    sNewGuide.replace(iCutFrom, iCutTo - iCutFrom, sNewBlock);

    // 5) 追加一条变更日志（无变化则不追加，FR1.4）。
    const bool bHasChange = !(a_stChanges.aAdded.isEmpty() &&
                              a_stChanges.aModified.isEmpty() &&
                              a_stChanges.aDeleted.isEmpty());
    if (bHasChange) {
        QStringList aParts;
        if (!a_stChanges.aAdded.isEmpty()) {
            QStringList aNames;
            for (const SFileFingerprint& rFp : a_stChanges.aAdded) {
                aNames.append(rFp.sRelPath);
            }
            aParts.append(QStringLiteral("新增：%1").arg(aNames.join(QStringLiteral("、"))));
        }
        if (!a_stChanges.aModified.isEmpty()) {
            QStringList aNames;
            for (const SFileFingerprint& rFp : a_stChanges.aModified) {
                aNames.append(rFp.sRelPath);
            }
            aParts.append(QStringLiteral("修改：%1").arg(aNames.join(QStringLiteral("、"))));
        }
        if (!a_stChanges.aDeleted.isEmpty()) {
            aParts.append(QStringLiteral("删除：%1").arg(a_stChanges.aDeleted.join(QStringLiteral("、"))));
        }
        const QString sTs = a_sTimestampIso.isEmpty()
            ? QDateTime::currentDateTime().toString(Qt::ISODate) : a_sTimestampIso;
        const QString sLogLine = QStringLiteral("- [%1] %2").arg(sTs, aParts.join(QStringLiteral("；")));
        // 确保变更日志段存在；不存在则补标题。追加到文末。
        if (!sNewGuide.contains(s_sChangeLogHeader)) {
            if (!sNewGuide.endsWith(QLatin1Char('\n'))) {
                sNewGuide += QLatin1Char('\n');
            }
            sNewGuide += QStringLiteral("\n") + s_sChangeLogHeader + QStringLiteral("\n");
        }
        if (!sNewGuide.endsWith(QLatin1Char('\n'))) {
            sNewGuide += QLatin1Char('\n');
        }
        sNewGuide += sLogLine + QStringLiteral("\n");
    }

    // 6) 临时文件 + rename 原子替换（R2）。UTF-8 with BOM，与 guide.md 编码一致。
    const QString sTmp = a_sGuidePath + QStringLiteral(".tmp");
    QFile stTmp(sTmp);
    if (!stTmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    stTmp.write("\xEF\xBB\xBF");   // UTF-8 BOM
    stTmp.write(sNewGuide.toUtf8());
    stTmp.close();

    // Windows 上 rename 覆盖目标：QFile::rename 目标存在时失败，故先删原文件再改名。
    if (QFile::exists(a_sGuidePath)) {
        if (!QFile::remove(a_sGuidePath)) {
            QFile::remove(sTmp);
            return false;
        }
    }
    if (!QFile::rename(sTmp, a_sGuidePath)) {
        return false;   // 改名失败：临时文件残留，调用方可手动清理
    }
    return true;
}
