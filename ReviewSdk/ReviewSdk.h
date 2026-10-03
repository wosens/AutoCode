#ifndef LUSTER_REVIEW_SDK_H_INCLUDED
#define LUSTER_REVIEW_SDK_H_INCLUDED

// ============================================================================
// ReviewSdk —— CodeReview 审查能力动态库公共头（C ABI）
// ----------------------------------------------------------------------------
// 定位：把主工程 CodeReview 的「审查」环节抽成可独立复用的动态库——
//   语言/规则匹配（16 个内置规则 md 已内嵌进 DLL 资源）→ 合并模式 prompt 拼装
//   → 【调用方注入的大模型回调】→ 输出解析（findings JSON）。
// 本库「单纯」承诺：不含 claude CLI 子进程、不含 UI、不含配置读写、不含
// sqlite 基线库、不建线程不发信号——大模型调用完全由宿主经 LCR_LlmInvokeFn
// 回调注入（同步、借用指针、无跨 CRT 分配），任何语言/框架（C++/C#/Python/
// Delphi…）均可经本 C ABI 接入。
// 头文件自包含：不 include 任何 Qt 头，可同时被 C 与 C++ 消费。
//
// 内存约定（务必遵守）：
//   - LCR_Version / LCR_SupportedLanguages 返回库内静态存储，勿释放、勿修改；
//   - 其余出参字符串（findings JSON / 错误描述 / 语言名）由库内 malloc 分配，
//     宿主用完必须调 LCR_FreeString 释放（分配与释放在库内同侧，跨 CRT 安全）；
//   - 回调返回的响应指针为「借用」：只须保持到本次 LCR_Review* 返回（或回调
//     再次被调），库内即时拷贝，不释放、不持有。
//
// 线程约定：全部导出函数无共享可变状态，可多线程并发调用；回调在调用线程内
// 同步执行，宿主自行保证回调自身的线程安全。回调执行期间不得再调本库函数。
//
// 字符串编码：入出参一律 UTF-8（路径分隔符 '/' 或 '\\' 均可）。
// ============================================================================

#ifdef REVIEWSDK_EXPORTS
#define LCR_API __declspec(dllexport)
#else
#define LCR_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// —— 返回码（LCR_Review* 的返回值）——
#define LCR_OK            0   // 成功（findings 可为空数组，空数组=无问题）
#define LCR_ERR_ARGS      1   // 参数非法（空指针 / 回调为空 / 相对路径为空）
#define LCR_ERR_NO_RULES  2   // 语言未命中内置规则（未知语言或扩展名无法判定）
#define LCR_ERR_READ      3   // 文件读取失败（LCR_ReviewFile 专用）
#define LCR_ERR_LLM       4   // 大模型回调失败（回调返回 NULL）
#define LCR_ERR_NOMEM     5   // 库内内存分配失败

// 大模型调用回调（宿主实现）：
//   a_pUserCtx        ：宿主上下文指针，原样透传（可为 NULL）
//   a_szPromptUtf8    ：完整审查 prompt（含全部适用规则与待审内容），UTF-8
//   a_ppszErrorUtf8   ：失败时写错误描述（借用指针，同响应约定；可为 NULL）
//   返回值            ：非 NULL = 模型响应全文（UTF-8，借用指针）；NULL = 失败
// 响应格式要求：与 CodeReview 主程序一致——```json 围栏（或裸 JSON）包
//   { "findings": [ { file, line, severity, rule, message, suggestion } ] }；
//   库内解析失败时整段响应降级为一条 low 级 finding（不丢信息）。
typedef const char* (*LCR_LlmInvokeFn)(void* a_pUserCtx,
                                       const char* a_szPromptUtf8,
                                       const char** a_ppszErrorUtf8);

// 库版本号（"v大.次.编译号"，静态存储，勿释放）
LCR_API const char* LCR_Version(void);

// 支持的审查语言清单（JSON 字符串数组，静态存储，勿释放），
// 如 ["basic","cpp","csharp","delphi","go","java","javascript","python","rust","typescript"]
LCR_API const char* LCR_SupportedLanguages(void);

// 语言 → 适用规则名清单（含跨语言规则附加、cpp 附加 qt）：返回 malloc 的 JSON
// 字符串数组（须 LCR_FreeString 释放）；空语言/未知语言返回 "[]"
// （主程序在派发前用它做「无规则不调模型」预判，与库内判定同源）
LCR_API char* LCR_RulesForLanguage(const char* a_pszLanguage);

// 按扩展名判定语言（全语言自动判定，不依赖宿主白名单）：
// 返回 malloc 语言名（如 "cpp"，小写；用完 LCR_FreeString 释放）；
// 未知扩展名返回 NULL。C++ 文件恒判 "cpp"（qt 规则由库内自动附加）
LCR_API char* LCR_LanguageForFile(const char* a_szFilePath);

// 审查内存内容（宿主自带源码文本，不经磁盘）：
//   a_szLanguage      ：语言名（LCR_SupportedLanguages 之一，小写；NULL/""=按
//                       a_szRelativePath 扩展名自动判定）
//   a_szRelativePath  ：文件标注路径（findings 的 file 字段与 prompt 引用，
//                       仅作展示/定位用途，不读盘；NULL/"" → LCR_ERR_ARGS）
//   a_szContentUtf8   :待审源码全文（UTF-8；NULL 视为空内容）
//   a_ppszFindingsJson：出参，成功时为 findings JSON 数组（malloc，见下 schema；
//                       无问题时为 "[]"），须 LCR_FreeString 释放
//   a_ppszError       :出参，非 LCR_OK 时为错误描述中文（malloc），须释放；可 NULL
// 返回 LCR_OK / LCR_ERR_*。schema：
//   [{"file":"相对路径","line":12,"severity":"high|medium|low",
//     "rule":"规则名","message":"问题描述","suggestion":"处理建议"}]
LCR_API int LCR_ReviewContent(const char* a_szLanguage,
                              const char* a_szRelativePath,
                              const char* a_szContentUtf8,
                              LCR_LlmInvokeFn a_pfnLlm,
                              void* a_pUserCtx,
                              char** a_ppszFindingsJson,
                              char** a_ppszError);

// LCR_ReviewContent 的增量扩展（主程序增量审查用）：a_iIncremental 非 0 时
// a_pszContentUtf8 应传 LCR_BuildIncrementalScope 产出的「修改段」文本
// （prompt 按增量契约拼装——findings 行号为当前文件真实 1-based 行号，
// 未列出部分明确不审）；为 0 时与 LCR_ReviewContent 完全等价
LCR_API int LCR_ReviewContentEx(const char* a_pszLanguage,
                                const char* a_pszRelativePath,
                                const char* a_pszContentUtf8,
                                int a_iIncremental,
                                LCR_LlmInvokeFn a_pfnLlm,
                                void* a_pUserCtx,
                                char** a_ppszFindingsJson,
                                char** a_ppszError);

// 增量审查范围提取（纯函数，不调模型）：旧/新两版全文（UTF-8）→「修改段」
// 文本（malloc，须 LCR_FreeString 释放；行号=当前文件 1-based、含上下文与
// 被删旧内容）。返回空串 "" = 两版无差异（调用方据此跳过模型调用）；
// 入参空指针返回 NULL
LCR_API char* LCR_BuildIncrementalScope(const char* a_pszOldContentUtf8,
                                        const char* a_pszNewContentUtf8);

// 审查磁盘文件：路径须存在可读；编码自适应（BOM → UTF-8 严格校验 → GBK），
// 透明加密密文自动回退 git 暂存区/HEAD 明文（与主程序同链路），可选经
// fileread.dll 服务进程读取（与 exe 同目录部署三件套时自动启用，缺省退回直读）。
// 语言按文件扩展名自动判定；findings 的 file 字段 = a_szFilePath 原样
LCR_API int LCR_ReviewFile(const char* a_szFilePath,
                           LCR_LlmInvokeFn a_pfnLlm,
                           void* a_pUserCtx,
                           char** a_ppszFindingsJson,
                           char** a_ppszError);

// 释放库内分配的字符串（出参串 / LCR_LanguageForFile 结果）；传 NULL 安全
LCR_API void LCR_FreeString(char* a_psz);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LUSTER_REVIEW_SDK_H_INCLUDED
