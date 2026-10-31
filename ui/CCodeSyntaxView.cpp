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
    if (Sdk().pfnCvcSetText(m_hView, a_aU8Text.constData(),
                            static_cast<unsigned int>(a_aU8Text.size())) != 0) {
        return false;
    }
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

void CCodeSyntaxView::GotoLine(int a_iLine) {
#ifdef Q_OS_WIN
    if (m_hView == nullptr || Sdk().pfnCvcSendMsg == nullptr || a_iLine < 1) {
        return;
    }
    // 与纯文本视图同口径：滚动到指定 1-based 行并整行选中（审查定位）。行号越界
    //   由 Scintilla 钉到首/末行，再从落位光标反查实际行号取行边界。
    const auto fnSend = [this](unsigned int iMsg, uintptr_t wp = 0, intptr_t lp = 0) {
        return Sdk().pfnCvcSendMsg(m_hView, iMsg, wp, lp);
    };
    fnSend(kSciGotoLine, static_cast<uintptr_t>(a_iLine - 1));
    const intptr_t iLine = fnSend(kSciLineFromPos, 0, fnSend(kSciGetCurrentPos));
    const intptr_t iStart = fnSend(kSciPositionFromLine, static_cast<uintptr_t>(iLine));
    const intptr_t iEnd = fnSend(kSciGetLineEndPosition, static_cast<uintptr_t>(iLine));
    fnSend(kSciSetSel, static_cast<uintptr_t>(iEnd), iStart);
    fnSend(kSciScrollCaret);
#else
    Q_UNUSED(a_iLine);
#endif
}
