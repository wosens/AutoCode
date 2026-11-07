#pragma once

// ============================================================================
// CFileSearcher —— 「在文件中查找/替换」引擎（2026-11-07 编辑菜单查找/替换）
// ----------------------------------------------------------------------------
// 纯逻辑（QtCore + Windows API 的 GB18030 转换），不依赖 QWidget，可单测：
//   - 多根递归遍历（工程根 + 关联项目根），按目录名排除 .autodev/.git/.svn 等
//     （清单由调用方从 config.ini [Find] ExcludeDirs 读取，缺省内置默认）；
//   - 任务/助手日志通道（m_aLogRoots，2026-11-07）：.autodev 默认在排除清单内、
//     且嵌套根去重会吞内层根，主遍历不可达——按 <root>/.autodev 固定布局
//     （assistant.json + tasks/<id>/{assistant.json,chat.json,run.log}）独立枚举；
//   - 「文本文件」识别：已知二进制后缀黑名单 + 字节级解码探测（BOM → UTF-8
//     严格 → GB18030 → 空字节比例判二进制 → Latin1 兜底，口径同
//     CFilePreviewWidget::DetectAndDecode），判为二进制即跳过；
//   - 匹配三开关：区分大小写 / 全字匹配（字母数字下划线边界）/ 正则表达式
//     （在整篇解码文本上匹配，跨行正则天然支持）；
//   - 替换：按命中（全篇 0-based 字符偏移 + 长度）就地改写，替换前校验命中处
//     文本未漂移；原编码写回（UTF-8 BOM/无 BOM、UTF-16 LE/BE BOM、GB18030、
//     Latin1 全保持；目标编码表达不了的替换文本按失败处理不写盘）。
// 命中带全篇字符偏移 m_iPos——替换不受行尾风格（CRLF/LF）影响。
// ============================================================================

#include <QString>
#include <QStringList>
#include <QList>
#include <QVector>
#include <QSet>
#include <atomic>

// 单条命中（查找结果的原子单位，也是替换的最小操作单元）。
struct SFindHit {
    QString m_sFilePath;    // 绝对路径（'/' 分隔规范）
    int     m_iLine = 0;    // 1-based 行号
    int     m_iCol  = 0;    // 1-based 列（解码后字符计）
    int     m_iLen  = 0;    // 匹配长度（字符）
    qsizetype m_iPos = 0;   // 全篇 0-based 字符偏移（替换用；与行/列同源）
    QString m_sLineText;    // 命中行文本（不含行尾符；列表提示/漂移校验辅助）
    QString m_sMatched;     // 实际匹配到的文本（替换前校验/定位用）
    int     m_iMatchOrd = 0;// 同一匹配文本在本文件的第几次出现（0-based；md/html
                            //   渲染后行列不对应源文，只能按「第 N 次出现」文本定位）
};

// 搜索请求（查找与替换共用：替换文本由调用方另行持有，引擎搜索时不需要）。
struct SFileSearchQuery {
    QString m_sPattern;         // 查找串（空=调用方先拦）
    bool m_bCase = false;       // 区分大小写
    bool m_bWord = false;       // 全字匹配
    bool m_bRegex = false;      // 使用正则表达式
    QStringList m_aRoots;       // 绝对根目录（多根依次遍历；嵌套根自动去重）
    QStringList m_aExcludeDirs; // 排除目录名（大小写不敏感）
    QStringList m_aLogRoots;    // 任务/助手日志根（各根的 .autodev 绝对路径；
                                //   独立通道不受排除清单约束，与 m_aRoots 空
                                //   布尔组合表达查找范围：仅日志=m_aRoots 空）
    QStringList m_aLogFileNames;// 日志文件名白名单（空=按 LogFileNames() 全量）；
                                //   主遍历已扫到的同名文件自动防重不双报
};

// 搜索统计（结果页状态行展示）。
struct SFileSearchStats {
    int m_iFilesScanned = 0;    // 实际解码扫描的文件数
    int m_iFilesSkipped = 0;    // 二进制/超限/读取失败跳过的文件数
    int m_iDirs = 0;            // 遍历的目录数
};

// 匹配器：三开关归一为「整篇文本上枚举命中」的统一接口（每文件构造一次的
//   预处理已提前完成——正则编译/大小写档），Search 内部与单测共用。
class SFindMatcher {
public:
    // 由查询构造；正则不可编译时返回 Invalid（a_pErr 可空，接收错误描述）。
    static SFindMatcher Create(const SFileSearchQuery& a_stQuery,
                               QString* a_pErr = nullptr);
    bool Valid() const { return m_bValid; }

    // 在 a_sText 全篇从 a_iFrom(0-based) 起找下一个命中；命中时填
    //   a_iPos0(0-based)/a_iLen(字符)/a_sMatched 并返回 true。无命中返回 false。
    bool FindNext(const QString& a_sText, qsizetype a_iFrom,
                  qsizetype& a_iPos0, qsizetype& a_iLen, QString& a_sMatched) const;

private:
    QString m_sPattern;                    // 纯文本/全字模式的查找串（原样）
    class QRegularExpression* m_pRe = nullptr;   // 正则模式（构造时编译；析构释放）
    bool m_bValid = false;
    bool m_bCase = false;
    bool m_bWord = false;
    bool m_bRegex = false;

public:
    // 默认构造=Invalid（Create 前不可用）；移动专用类，禁拷贝。
    SFindMatcher() = default;
    ~SFindMatcher();
    SFindMatcher(const SFindMatcher&) = delete;             // 持正则对象，禁拷贝
    SFindMatcher& operator=(const SFindMatcher&) = delete;
    SFindMatcher(SFindMatcher&& a_stOther) noexcept;        // 可移动（容器/返回值用）
    SFindMatcher& operator=(SFindMatcher&& a_stOther) noexcept;
};

class CFileSearcher {
public:
    // 任务/助手日志规范文件名（.autodev 布局三个名：assistant.json / chat.json /
    //   run.log）——日志通道（m_aLogRoots）的枚举白名单，主遍历防重同口径。
    static QStringList LogFileNames();

    // 正则预检（对话框「查找所有」前拦）：返回空串=合法/非正则模式；否则错误描述。
    static QString RegexError(const QString& a_sPattern, bool a_bCase);

    // 同步搜索（调用方负责放后台线程；a_pCancel 可空——置 true 提前收工，
    //   已收集的部分命中保持有效）。命中顺序：根遍历序（文件主序）× 文件内偏移
    //   升序；主遍历（m_aRoots）完成后按 m_aLogRoots 枚举日志通道（m_aRoots 空
    //   + m_aLogRoots 非空 = 只搜日志；两者均空 = 直接返回零命中）。
    static void Search(const SFileSearchQuery& a_stQuery, QList<SFindHit>& a_aHits,
                       SFileSearchStats& a_stStats, const std::atomic_bool* a_pCancel = nullptr);

    // 单点替换：校验命中处文本未漂移 → 就地替换 → 原子写回（原编码原行尾）。
    //   成功返回 true；失败 a_sErr 给原因（文件变化/编码回写不出/写盘失败）。
    static bool ReplaceOne(const SFindHit& a_stHit, const QString& a_sReplace,
                           QString& a_sErr);

    // 批量替换：按文件分组、文件内自后向前应用（前面的偏移不受影响），逐文件一次写回。
    //   返回成功替换的处数；单文件失败不阻断其余（a_pErrs 可空，收集失败描述）。
    static int ReplaceAll(QList<SFindHit> a_aHits, const QString& a_sReplace,
                          QStringList* a_pErrs = nullptr);

    // —— 编解码（探测口径同 CFilePreviewWidget::DetectAndDecode；独立实现保持
    //    core 层不依赖 ui）——
    enum class ETextEnc { Utf8Bom, Utf8, Utf16LE, Utf16BE, Gb, Latin1, Binary };
    static ETextEnc DecodeText(const QByteArray& a_aBytes, QString& a_sOut);
    // 按原编码回写：目标编码表达不了的字符（Latin1>0xFF / GB 转换失败）返回 false。
    static bool EncodeText(const QString& a_sText, ETextEnc a_eEnc, QByteArray& a_aOut);

    // 已知二进制后缀（小写、无点；黑名单命中直接跳过不读盘）。
    static bool IsBinaryExt(const QString& a_sExtLower);

    // 单文件搜索（Search 内部逐文件调用；公开便于单测小粒度构造）。
    static void SearchFile(const QString& a_sAbsPath, const SFileSearchQuery& a_stQuery,
                           const SFindMatcher& a_stMatcher, QList<SFindHit>& a_aHits,
                           bool& a_bSkipped);

    // 单文件大小上限（超过按跳过处理，防整目录巨型产物拖死搜索）。
    static constexpr qint64 s_kiMaxFileBytes = 16 * 1024 * 1024;
};
