// CCodeSyntaxView.cpp —— CodeStyle 源码语法渲染宿主实现
// 加载点：exe 同目录 CodeParser.dll / CodeView.dll（便携原则：Execute 目录拷走即
//   自包含；缺 DLL 时本视图不可用，CFilePreviewWidget 回退纯文本，功能闸门关闭）。
// 语法 DLL（tree-sitter-c/cpp.dll）由 CodeParser.dll 在 CpsCreate 时按相对名自行
//   加载（Windows 加载器首先搜索宿主 exe 目录，与 exe 同目录即满足——手册 §2.2）。

#include "CCodeSyntaxView.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDir>
#include <QResizeEvent>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ————————————————————————————————————————————————
// SDK 函数指针（签名与 CodeStyle/include/codeparser_c.h、codeview_c.h 逐字一致；
//   不 include 官方头——其非导出侧声明 dllimport 会引入 __imp_ 链接依赖，与
//   review/CReviewSdkClient 同口径自带 C ABI 等价定义）
// ————————————————————————————————————————————————
namespace {

// —— Scintilla SCI_* 常量（取自 CodeStyle/third_party/scintilla/include/Scintilla.h）——
constexpr unsigned int kSciGetLength           = 2006;
constexpr unsigned int kSciGetCurrentPos       = 2008;
constexpr unsigned int kSciGetAnchor           = 2009;
constexpr unsigned int kSciGotoLine            = 2024;
constexpr unsigned int kSciPositionFromLine    = 2167;
constexpr unsigned int kSciGetLineEndPosition  = 2136;
constexpr unsigned int kSciGetSelectionStart   = 2143;
constexpr unsigned int kSciGetSelectionEnd     = 2145;
constexpr unsigned int kSciSetSel              = 2160;
constexpr unsigned int kSciLineFromPos         = 2166;
constexpr unsigned int kSciSetTargetStart      = 2190;
constexpr unsigned int kSciSetTargetEnd        = 2192;
constexpr unsigned int kSciSearchInTarget      = 2197;
constexpr unsigned int kSciSetSearchFlags      = 2198;
constexpr unsigned int kSciScrollCaret         = 2169;
// 2026-11-07 kSciGetLine/kSciLineLength 撤除：CvcSendMsg 通道不回拷缓冲（探针
//   实证），行定位改自算（LineByteRange，基于 ShowSource 存的 m_aU8Last）。
constexpr uintptr_t kScFindMatchCase           = 0x4;

typedef void*       (*FnCpsCreate)(int);
typedef void        (*FnCpsDestroy)(void*);
typedef int         (*FnCpsSetText)(void*, const char*, unsigned int);
typedef unsigned int (*FnCpsCollect)(void*, void*, unsigned int, unsigned int*);

typedef void*       (*FnCvcCreate)(void*, int, int, int, int);
typedef void        (*FnCvcDestroy)(void*);
typedef void        (*FnCvcSetRect)(void*, int, int, int, int);
typedef int         (*FnCvcSetText)(void*, const char*, unsigned int);
typedef int         (*FnCvcApplySpans)(void*, const void*, unsigned int);
typedef intptr_t    (*FnCvcSendMsg)(void*, unsigned int, uintptr_t, intptr_t);

// 进程级 SDK 单例（首用加载一次；此后只读，全部调用在 UI 线程无需锁）。
struct SCodeSdk {
    bool            bTried     = false;
    bool            bOk        = false;
    HMODULE         hParserDll = nullptr;
    HMODULE         hViewDll   = nullptr;
    FnCpsCreate     pfnCpsCreate       = nullptr;
    FnCpsDestroy    pfnCpsDestroy      = nullptr;
    FnCpsSetText    pfnCpsSetText      = nullptr;
    FnCpsCollect    pfnCpsCollect      = nullptr;
    FnCvcCreate     pfnCvcCreate       = nullptr;
    FnCvcDestroy    pfnCvcDestroy      = nullptr;
    FnCvcSetRect    pfnCvcSetRect      = nullptr;
    FnCvcSetText    pfnCvcSetText      = nullptr;
    FnCvcApplySpans pfnCvcApplySpans   = nullptr;
    FnCvcSendMsg    pfnCvcSendMsg      = nullptr;
};

SCodeSdk& Sdk() {
    static SCodeSdk s_stSdk;
    return s_stSdk;
}

// CpsSpan（与 codeparser_c.h 二进制布局一致：3×uint32）
struct SCpsSpan {
    unsigned int uStart;
    unsigned int uLen;
    unsigned int eStyle;
};

bool EnsureSdk() {
#ifdef Q_OS_WIN
    SCodeSdk& st = Sdk();
    if (st.bTried) {
        return st.bOk;
    }
    st.bTried = true;

    const QString sDir = QCoreApplication::applicationDirPath();
    const std::wstring wsParser = QDir::toNativeSeparators(
        sDir + QStringLiteral("/CodeParser.dll")).toStdWString();
    const std::wstring wsView = QDir::toNativeSeparators(
        sDir + QStringLiteral("/CodeView.dll")).toStdWString();
    st.hParserDll = ::LoadLibraryW(wsParser.c_str());
    st.hViewDll   = ::LoadLibraryW(wsView.c_str());
    if (st.hParserDll == nullptr || st.hViewDll == nullptr) {
        return false;   // 缺 DLL：功能闸门关闭（设置项开启时预览回退纯文本）
    }

    st.pfnCpsCreate     = reinterpret_cast<FnCpsCreate>(
        reinterpret_cast<void*>(::GetProcAddress(st.hParserDll, "CpsCreate")));
    st.pfnCpsDestroy    = reinterpret_cast<FnCpsDestroy>(
        reinterpret_cast<void*>(::GetProcAddress(st.hParserDll, "CpsDestroy")));
    st.pfnCpsSetText    = reinterpret_cast<FnCpsSetText>(
        reinterpret_cast<void*>(::GetProcAddress(st.hParserDll, "CpsSetText")));
    st.pfnCpsCollect    = reinterpret_cast<FnCpsCollect>(
        reinterpret_cast<void*>(::GetProcAddress(st.hParserDll, "CpsCollectSpans")));
    st.pfnCvcCreate     = reinterpret_cast<FnCvcCreate>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcCreate")));
    st.pfnCvcDestroy    = reinterpret_cast<FnCvcDestroy>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcDestroy")));
    st.pfnCvcSetRect    = reinterpret_cast<FnCvcSetRect>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcSetRect")));
    st.pfnCvcSetText    = reinterpret_cast<FnCvcSetText>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcSetText")));
    st.pfnCvcApplySpans = reinterpret_cast<FnCvcApplySpans>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcApplySpans")));
    st.pfnCvcSendMsg    = reinterpret_cast<FnCvcSendMsg>(
        reinterpret_cast<void*>(::GetProcAddress(st.hViewDll, "CvcSendMsg")));

    // 任一导出缺失即判不可用（防半加载状态；CvcSetCallback 等未用导出不强制）。
    st.bOk = st.pfnCpsCreate != nullptr && st.pfnCpsDestroy != nullptr
          && st.pfnCpsSetText != nullptr && st.pfnCpsCollect != nullptr
          && st.pfnCvcCreate != nullptr && st.pfnCvcDestroy != nullptr
          && st.pfnCvcSetRect != nullptr && st.pfnCvcSetText != nullptr
          && st.pfnCvcApplySpans != nullptr && st.pfnCvcSendMsg != nullptr;
    if (!st.bOk) {
        // 半加载回滚：卸载句柄防资源泄露（导出指针随句柄失效一并清零）。
        if (st.hParserDll != nullptr) {
            ::FreeLibrary(st.hParserDll);
            st.hParserDll = nullptr;
        }
        if (st.hViewDll != nullptr) {
            ::FreeLibrary(st.hViewDll);
            st.hViewDll = nullptr;
        }
        st.pfnCpsCreate = nullptr; st.pfnCpsDestroy = nullptr;
        st.pfnCpsSetText = nullptr; st.pfnCpsCollect = nullptr;
        st.pfnCvcCreate = nullptr; st.pfnCvcDestroy = nullptr;
        st.pfnCvcSetRect = nullptr; st.pfnCvcSetText = nullptr;
        st.pfnCvcApplySpans = nullptr; st.pfnCvcSendMsg = nullptr;
    }
    return st.bOk;
#else
    return false;
#endif
}

} // namespace

#ifdef Q_OS_WIN
// 原生消息过滤器：Scintilla 子窗口持焦时 Qt 快捷键链收不到消息——线程级
//   WM_KEYDOWN 里代听 Ctrl+F（仅本视图窗口树），命中转发 sigFindRequested。
//   挂 qApp（进程级单过滤器，随本视图析构卸下）。
class CCodeKeyFilter : public QAbstractNativeEventFilter {
public:
    explicit CCodeKeyFilter(CCodeSyntaxView* a_pHost) : m_pHost(a_pHost) {}
    bool nativeEventFilter(const QByteArray& a_sType, void* a_pMsg, qintptr*) override {
        if (a_sType != QByteArrayLiteral("windows_generic_MSG") || m_pHost == nullptr) {
            return false;
        }
        const MSG* pMsg = static_cast<const MSG*>(a_pMsg);
        if (pMsg->message == WM_KEYDOWN && pMsg->wParam == 'F'
                && (::GetKeyState(VK_CONTROL) & 0x8000) != 0
                && m_pHost->internalWinId() != 0
                && ::IsChild(reinterpret_cast<HWND>(m_pHost->internalWinId()), pMsg->hwnd)) {
            emit m_pHost->sigFindRequested();
            return true;   // 吃掉，防 Scintilla 再处理
        }
        return false;
    }
private:
    CCodeSyntaxView* m_pHost;   // 借用（宿主析构前 removeFilter）
};
#endif

CCodeSyntaxView::CCodeSyntaxView(QWidget* a_pParent) : QWidget(a_pParent) {
#ifdef Q_OS_WIN
    if (EnsureSdk()) {
        // winId() 强制本 Widget 落原生 HWND（CvcCreate 需要父窗口句柄）。
        m_hView = Sdk().pfnCvcCreate(reinterpret_cast<void*>(winId()),
                                     0, 0, qMax(width(), 16), qMax(height(), 16));
        if (m_hView == nullptr) {
            // POC 单实例限制：他处已持视图（本进程理论单预览，防御分支）。
        } else {
            m_pKeyFilter = new CCodeKeyFilter(this);
            qApp->installNativeEventFilter(m_pKeyFilter);
        }
    }
#endif
}

CCodeSyntaxView::~CCodeSyntaxView() {
#ifdef Q_OS_WIN
    if (m_pKeyFilter != nullptr) {
        qApp->removeNativeEventFilter(m_pKeyFilter);   // 先卸监听再毁视图（防悬垂回调）
        delete m_pKeyFilter;
        m_pKeyFilter = nullptr;
    }
    if (m_hView != nullptr && Sdk().pfnCvcDestroy != nullptr) {
        Sdk().pfnCvcDestroy(m_hView);   // 释放全局唯一视图名额（后续可再建）
        m_hView = nullptr;
    }
#endif
}

bool CCodeSyntaxView::ModuleAvailable() {
#ifdef Q_OS_WIN
    return EnsureSdk();
#else
    return false;
#endif
}

void CCodeSyntaxView::resizeEvent(QResizeEvent* a_e) {
    QWidget::resizeEvent(a_e);
#ifdef Q_OS_WIN
    if (m_hView != nullptr && Sdk().pfnCvcSetRect != nullptr) {
        Sdk().pfnCvcSetRect(m_hView, 0, 0, qMax(a_e->size().width(), 0),
                            qMax(a_e->size().height(), 0));
    }
#endif
}

bool CCodeSyntaxView::ShowSource(const QByteArray& a_aU8Text, bool a_bCpp, bool a_bSemantic) {
#ifdef Q_OS_WIN
    if (m_hView == nullptr || Sdk().pfnCvcSetText == nullptr) {
        return false;
    }
    // 顺序契约（手册 §3）：先 CvcSetText（清旧样式 + 词法着色 + 置只读），
    //   语义 span 后灌；文本更新后两步都要重做。
    // 2026-11-07 修追加缺陷（HotNews 实害）：CodeView.dll 的清空用的是
    //   SCI_SETTEXT(0,0)，本版 Scintilla 对 NULL lParam 不清空 → 每次预览都
    //   追加（探针 probe_hotnews 实锤 viewLen=48503×7：乱码=内容重复拼接、
    //   首开后定位全错、文档无限膨胀至卡死）。宿主侧先解锁只读并 SCI_CLEARALL，
    //   兼容已部署的旧 DLL（DLL 源头同步修）。
    if (Sdk().pfnCvcSendMsg != nullptr) {
        Sdk().pfnCvcSendMsg(m_hView, 2191 /*SCI_SETREADONLY*/, 0, 0);
        Sdk().pfnCvcSendMsg(m_hView, 2180 /*SCI_CLEARALL*/, 0, 0);
    }
    if (Sdk().pfnCvcSetText(m_hView, a_aU8Text.constData(),
                            static_cast<unsigned int>(a_aU8Text.size())) != 0) {
        return false;
    }
    // 2026-11-07 存最近文本：GotoLine/GotoPos 的行/列→字节换算全部自算——
    //   探针实证 CvcSendMsg 通道 SCI_GETLINE 不回拷缓冲、SCI_LINEFROMPOS 参数
    //   在 wParam（旧代码传 lParam 恒返行 0 → 审查定位一直选中第 1 行）。
    m_aU8Last = a_aU8Text;
    if (a_bSemantic && Sdk().pfnCpsCreate != nullptr && Sdk().pfnCpsSetText != nullptr
            && Sdk().pfnCpsCollect != nullptr && Sdk().pfnCvcApplySpans != nullptr) {
        // 解析器随用随析（含一次语法 DLL 加载与查询编译，POC 无增量；UI 线程
        //   同步调用——手册 §7.5 与渲染同线程最稳）。
        void* hPs = Sdk().pfnCpsCreate(a_bCpp ? 1 : 0);
        if (hPs != nullptr) {
            if (Sdk().pfnCpsSetText(hPs, a_aU8Text.constData(),
                                    static_cast<unsigned int>(a_aU8Text.size())) == 0) {
                unsigned int uTotal = 0;
                Sdk().pfnCpsCollect(hPs, nullptr, 0, &uTotal);   // 第一段：取总数
                if (uTotal > 0) {
                    QByteArray aSpans;
                    aSpans.resize(static_cast<qsizetype>(uTotal)
                                  * static_cast<qsizetype>(sizeof(SCpsSpan)));
                    const unsigned int uGot = Sdk().pfnCpsCollect(
                        hPs, aSpans.data(), uTotal, &uTotal);    // 第二段：取数据
                    if (uGot > 0) {
                        Sdk().pfnCvcApplySpans(m_hView, aSpans.constData(), uGot);
                    }
                }
            }
            Sdk().pfnCpsDestroy(hPs);
        }
    }
    // 光标归零（对齐 QTextBrowser 语义：CvcSetText 后光标在文末，首个“向下查找”
    //   区间会空导致必 miss；归零后首查从文档头命中第一处）。
    Sdk().pfnCvcSendMsg(m_hView, kSciSetSel, 0, 0);
    return true;
#else
    Q_UNUSED(a_aU8Text); Q_UNUSED(a_bCpp); Q_UNUSED(a_bSemantic);
    return false;
#endif
}

bool CCodeSyntaxView::FindText(const QString& a_sText, bool a_bBackward,
                               bool a_bCase, bool a_bWrap) {
#ifdef Q_OS_WIN
    if (m_hView == nullptr || Sdk().pfnCvcSendMsg == nullptr || a_sText.isEmpty()) {
        return false;
    }
    const QByteArray aU8 = a_sText.toUtf8();
    const auto fnSend = [this](unsigned int iMsg, uintptr_t wp = 0, intptr_t lp = 0) {
        return Sdk().pfnCvcSendMsg(m_hView, iMsg, wp, lp);
    };
    const intptr_t iDocLen = fnSend(kSciGetLength);
    if (iDocLen <= 0 || aU8.isEmpty()) {
        return false;
    }
    // 搜索起点：当前选区边界（向后=选区尾，向前=选区首），与 QTextBrowser::find 的
    //   「命中后下一跳从选区后继续」口径对齐。
    const intptr_t iSelStart = fnSend(kSciGetSelectionStart);
    const intptr_t iSelEnd   = fnSend(kSciGetSelectionEnd);
    fnSend(kSciSetSearchFlags, a_bCase ? kScFindMatchCase : uintptr_t(0));
    // 注：Scintilla 单参数消息（SETTARGETSTART/END 等）的实参在 wParam（手册口径），
    //   仅 SETSEL（双参数）/SEARCHINTARGET（串指针）用 lParam。

    const auto fnSearch = [&](intptr_t iFrom, intptr_t iTo) {
        fnSend(kSciSetTargetStart, static_cast<uintptr_t>(iFrom));
        fnSend(kSciSetTargetEnd, static_cast<uintptr_t>(iTo));
        return fnSend(kSciSearchInTarget,
                      static_cast<uintptr_t>(aU8.size()),
                      reinterpret_cast<intptr_t>(aU8.constData()));
    };
    intptr_t iFound = -1;
    if (!a_bBackward) {
        iFound = fnSearch(iSelEnd, iDocLen);
        if (iFound < 0 && a_bWrap) {
            iFound = fnSearch(0, iDocLen);
        }
    } else {
        iFound = fnSearch(iSelStart, 0);
        if (iFound < 0 && a_bWrap) {
            iFound = fnSearch(iDocLen, 0);
        }
    }
    if (iFound < 0) {
        return false;
    }
    // 命中即选中并滚动（向后选区=前锚后拖；向前选区方向反转，与编辑器习惯一致）。
    const intptr_t iHitEnd = iFound + aU8.size();
    if (!a_bBackward) {
        fnSend(kSciSetSel, static_cast<uintptr_t>(iFound), iHitEnd);
    } else {
        fnSend(kSciSetSel, static_cast<uintptr_t>(iHitEnd), iFound);
    }
    fnSend(kSciScrollCaret);
    return true;
#else
    Q_UNUSED(a_sText); Q_UNUSED(a_bBackward); Q_UNUSED(a_bCase); Q_UNUSED(a_bWrap);
    return false;
#endif
}

void CCodeSyntaxView::LineByteRange(int a_iLine, qsizetype& a_iStart,
                                    qsizetype& a_iEnd) const {
    // 2026-11-07 自算行字节界：基于 m_aU8Last 找第 a_iLine-1 个换行符（'\n'）。
    //   行号≤1 钉首行、越界钉末行（空文本=空区间）。
    a_iStart = 0;
    a_iEnd = m_aU8Last.size();
    if (a_iLine <= 1 || m_aU8Last.isEmpty()) {
        if (a_iLine <= 1) {
            const qsizetype iNl = m_aU8Last.indexOf('\n');
            if (iNl >= 0) {
                a_iEnd = iNl;
            }
        }
        return;
    }
    for (int i = 1; i < a_iLine; ++i) {
        const qsizetype iNl = m_aU8Last.indexOf('\n', a_iStart);
        if (iNl < 0) {   // 行号超尾：钉末行
            a_iEnd = m_aU8Last.size();
            return;
        }
        a_iStart = iNl + 1;
    }
    const qsizetype iNl = m_aU8Last.indexOf('\n', a_iStart);
    a_iEnd = (iNl >= 0) ? iNl : m_aU8Last.size();
}

void CCodeSyntaxView::GotoLine(int a_iLine) {
#ifdef Q_OS_WIN
    // 2026-11-07 重写：行字节界自算（旧实现经 CvcSendMsg 回读 LINEFROMPOS 恒返行 0
    //   ——参数应在 wParam——导致定位恒选第 1 行；探针 probe_findpos 实证）。选中
    //   整行滚动可见的口径不变。
    if (m_hView == nullptr || Sdk().pfnCvcSendMsg == nullptr || a_iLine < 1) {
        return;
    }
    const auto fnSend = [this](unsigned int iMsg, uintptr_t wp = 0, intptr_t lp = 0) {
        return Sdk().pfnCvcSendMsg(m_hView, iMsg, wp, lp);
    };
    qsizetype iStart = 0, iEnd = 0;
    LineByteRange(a_iLine, iStart, iEnd);
    fnSend(kSciSetSel, static_cast<uintptr_t>(iEnd),
           static_cast<uintptr_t>(iStart));
    fnSend(kSciScrollCaret);
#else
    Q_UNUSED(a_iLine);
#endif
}

void CCodeSyntaxView::GotoPos(int a_iLine, int a_iCol, int a_iLen) {
#ifdef Q_OS_WIN
    // 2026-11-07 重写：行界自算 + 行文本本地解码换算字符列→字节偏移（不再经
    //   CvcSendMsg 回读——GETLINE 不回拷/LINEFROMPOS 参数错位双坑，见 ShowSource 注）。
    //   列/长越界钉在行内；命中跨行尾时选到行尾。
    if (m_hView == nullptr || Sdk().pfnCvcSendMsg == nullptr || a_iLine < 1) {
        return;
    }
    const auto fnSend = [this](unsigned int iMsg, uintptr_t wp = 0, intptr_t lp = 0) {
        return Sdk().pfnCvcSendMsg(m_hView, iMsg, wp, lp);
    };
    qsizetype iLineStart = 0, iLineEnd = 0;
    LineByteRange(a_iLine, iLineStart, iLineEnd);
    const QString sLine = QString::fromUtf8(m_aU8Last.constData() + iLineStart,
                                            iLineEnd - iLineStart);
    const qsizetype iCol0 = qBound<qsizetype>(0, qsizetype(a_iCol - 1),
                                              sLine.size());
    const qsizetype iLenC = (a_iLen > 0)
        ? qBound<qsizetype>(0, qsizetype(a_iLen), sLine.size() - iCol0) : 0;
    const qsizetype cbPrefix = QStringView(sLine).left(iCol0).toUtf8().size();
    const qsizetype cbMatch = QStringView(sLine).mid(iCol0, iLenC).toUtf8().size();
    const qsizetype iSelStart = iLineStart + cbPrefix;
    fnSend(kSciSetSel, static_cast<uintptr_t>(iSelStart + cbMatch),
           static_cast<uintptr_t>(iSelStart));
    fnSend(kSciScrollCaret);
#else
    Q_UNUSED(a_iLine); Q_UNUSED(a_iCol); Q_UNUSED(a_iLen);
#endif
}
