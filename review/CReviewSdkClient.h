#pragma once

// ============================================================================
// CReviewSdkClient —— ReviewSdk.dll 动态加载客户端（2026-09-30 代码审查接入）
// ----------------------------------------------------------------------------
// CodeReview 工程把「审查」环节抽成了可复用动态库（说明：CodeReview/Doc/
// 审查动态库ReviewSdk说明.md）：语言/规则匹配、合并模式 prompt 拼装、模型输出
// 解析都在库内；大模型调用经 LCR_LlmInvokeFn 回调由宿主注入。
// 本类按 CodeReview.exe 同款接入方式（LoadLibrary/GetProcAddress，免 .lib 免
// 链接期依赖）动态加载 exe 同目录 ReviewSdk.dll：
//   - 缺失/导出不全：程序正常启动、审查入口明确报因并跳过（功能闸门）；
//   - 在位：提供 ReviewFile/LanguageForFile 等包装（QString 出入参）。
// 注意：**不 include ReviewSdk/ReviewSdk.h**——该头在非导出侧声明
// __declspec(dllimport)，直接调用会引入 __imp_ 链接依赖；此处自带与 C ABI
// 二进制等价的函数指针定义（返回码/回调签名与官方头逐字一致）。
// 线程：EnsureLoaded 进程内首次调用线程加载一次（此后只读），可多线程并发使用
// （库本体无共享可变状态，官方 smoke 已验 8 线程并发）。
// ============================================================================

#include <QString>
#include <QStringList>

// —— 大模型回调（与 ReviewSdk.h 的 LCR_LlmInvokeFn 二进制等价）——
// 成功返回响应全文 UTF-8（借用指针，仅须保活到本次 LCR_Review* 返回）；
// 失败返回 nullptr 并经 a_ppszErrorUtf8 给出原因（借用指针）。
typedef const char* (*FnLcrLlmInvoke)(void* a_pUserCtx,
                                      const char* a_szPromptUtf8,
                                      const char** a_ppszErrorUtf8);

// —— 返回码（与 ReviewSdk.h 一致）——
#define AUTOCODE_LCR_OK           0   // 成功（findings 可为 []）
#define AUTOCODE_LCR_ERR_ARGS     1   // 参数非法
#define AUTOCODE_LCR_ERR_NO_RULES 2   // 语言未命中（自动判定失败=该文件不支持，静默跳过）
#define AUTOCODE_LCR_ERR_READ     3   // 文件读取失败
#define AUTOCODE_LCR_ERR_LLM      4   // 大模型回调失败
#define AUTOCODE_LCR_ERR_NOMEM    5   // 库内内存分配失败

class CReviewSdkClient {
public:
    // 进程内单例（首次调用触发加载）。
    static CReviewSdkClient* Get();

    // 确保 dll 已加载（exe 同目录 ReviewSdk.dll）；失败经 a_pErr 回带原因。
    // 幂等：已加载/已失败直接返回缓存结论。
    bool EnsureLoaded(QString* a_pErr = nullptr);

    bool IsLoaded() const { return m_bLoaded; }

    // 库版本号（如 "v1.9.52"；未加载返回空）。
    QString Version() const;

    // 支持的审查语言清单（未加载返回空表）。
    QStringList SupportedLanguages() const;

    // 按扩展名判定语言（小写，如 "cpp"；未知/未加载返回空串）。
    QString LanguageForFile(const QString& a_sAbsPath);

    // 语言 → 适用规则名清单（空语言/未知返回空表）。
    QStringList RulesForLanguage(const QString& a_sLanguage);

    // 审查磁盘文件（编码自适应/透明加密回退由库内读取链处理）。
    // a_sDisplayPath：findings 的 file 字段回显路径（传相对路径则报告里即相对）。
    // 返回 AUTOCODE_LCR_*；成功时 a_sFindingsJson 为 findings JSON 数组，
    // 失败时 a_sErr 为中文原因。
    int ReviewFile(const QString& a_sAbsPath, const QString& a_sDisplayPath,
                   FnLcrLlmInvoke a_pfnLlm, void* a_pUserCtx,
                   QString& a_sFindingsJson, QString& a_sErr);

    // 审查内存文本（任务阶段预审的备用入口；当前主路径走 ReviewFile）。
    int ReviewContent(const QString& a_sLanguage, const QString& a_sDisplayPath,
                      const QString& a_sContentUtf8,
                      FnLcrLlmInvoke a_pfnLlm, void* a_pUserCtx,
                      QString& a_sFindingsJson, QString& a_sErr);

    // —— 2026-10-08 增量审查两件套（手动审查「仅审修改部分」）——
    // 增量范围提取（纯函数不调模型）：旧/新全文 → 修改段文本；返回空串=两版无
    // 差异（跳过模型）；isNull()=入参空指针。未加载返回空串。
    QString BuildIncrementalScope(const QString& a_sOldContent, const QString& a_sNewContent);
    // 全量/增量统一入口：a_bIncremental=true 时 a_sContent 须传 BuildIncrementalScope
    // 产物（行号=当前文件真实 1-based、未列出部分不审）；false 等价 ReviewContent。
    int ReviewContentEx(const QString& a_sLanguage, const QString& a_sDisplayPath,
                        const QString& a_sContentUtf8, bool a_bIncremental,
                        FnLcrLlmInvoke a_pfnLlm, void* a_pUserCtx,
                        QString& a_sFindingsJson, QString& a_sErr);

    // 把磁盘绝对路径换算为工程相对路径（正斜杠；不在根下时回退绝对路径）。
    // 报告/注入块统一用相对路径（含 CReviewService/CCodeReviewDialog 两消费方）。
    static QString RelativeToRoot(const QString& a_sRootDir, const QString& a_sAbsPath);

private:
    CReviewSdkClient() = default;

    // 导出表（9 个 LCR_* 函数；任一解析失败视为不可用）
    const char* (*m_pfnVersion)(void)                                   = nullptr;
    const char* (*m_pfnSupportedLanguages)(void)                        = nullptr;
    char*       (*m_pfnRulesForLanguage)(const char*)                   = nullptr;
    char*       (*m_pfnLanguageForFile)(const char*)                    = nullptr;
    int         (*m_pfnReviewContent)(const char*, const char*, const char*,
                                      FnLcrLlmInvoke, void*, char**, char**)            = nullptr;
    int         (*m_pfnReviewContentEx)(const char*, const char*, const char*, int,
                                        FnLcrLlmInvoke, void*, char**, char**)          = nullptr;
    char*       (*m_pfnBuildIncrementalScope)(const char*, const char*) = nullptr;
    int         (*m_pfnReviewFile)(const char*, FnLcrLlmInvoke, void*, char**, char**)  = nullptr;
    void        (*m_pfnFreeString)(char*)                               = nullptr;

    bool        m_bLoaded    = false;
    bool        m_bTried     = false;    // 已尝试过加载（失败也缓存，防每文件重试弹错）
    QString     m_sLoadErr;              // 加载失败原因（日志/事件提示用）
};
