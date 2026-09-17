#include "CFrwDevice.h"

// FRW 三件套接入（动态加载免 .lib；协议与错误码见 filereadwrite.h / RaadFile doc/usage.md）
#include "filereadwrite.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QtGlobal>
#include <windows.h>

namespace {

// FRW_ERR_*（与 filereadwrite 协议同源，usage.md §5）
enum EFrwErr {
    kFrwOk = 0,
    kFrwInvalidParam = 1,
    kFrwLengthExceeded = 3,
    kFrwFileNotFound = 4,
    kFrwEof = 10,
};

// FRW API 懒加载（进程一次；HMODULE 永不释放——usage.md：宿主须保持加载至全部调用返回，
// 本组件随进程生命周期即是；LoadLibrary 引用计数保证 code.exe 侧协议对象存活期匹配）
struct SFrwApi {
    PFN_FRW_OpenW pOpenW = nullptr;
    PFN_FRW_Read pRead = nullptr;
    PFN_FRW_Close pClose = nullptr;
    bool bAvailable = false;
};
const SFrwApi& FrwApi() {
    static SFrwApi s_st;
    static QMutex s_oMtx;
    QMutexLocker oLock(&s_oMtx);
    if (s_st.pOpenW != nullptr || s_st.bAvailable) {
        return s_st;   // 已初始化（bAvailable=true 表示结论为不可用，不再重试）
    }
    const QString sDll = QCoreApplication::applicationDirPath()
                         + QStringLiteral("/fileread.dll");
    const HMODULE hMod = ::LoadLibraryW(reinterpret_cast<const wchar_t*>(sDll.utf16()));
    if (hMod != nullptr) {
        s_st.pOpenW = reinterpret_cast<PFN_FRW_OpenW>(
            ::GetProcAddress(hMod, "FRW_OpenW"));
        s_st.pRead = reinterpret_cast<PFN_FRW_Read>(
            ::GetProcAddress(hMod, "FRW_Read"));
        s_st.pClose = reinterpret_cast<PFN_FRW_Close>(
            ::GetProcAddress(hMod, "FRW_Close"));
        if (s_st.pOpenW == nullptr || s_st.pRead == nullptr || s_st.pClose == nullptr) {
            // 符号缺失＝三件套错配/损坏：置空按不可用处理（不再 FreeLibrary，幂等安全）
            s_st.pOpenW = s_st.pRead = s_st.pClose = nullptr;
        }
    }
    s_st.bAvailable = (s_st.pOpenW != nullptr);
    return s_st;
}

// 兜底直读的告警只发一次（dll 缺失/拉起失败时避免每读一文件刷屏）
void WarnFallbackOnce(const QString& a_sWhy) {
    static QMutex s_oMtx;
    static bool s_bWarned = false;
    QMutexLocker oLock(&s_oMtx);
    if (!s_bWarned) {
        s_bWarned = true;
        qWarning() << "[CFrwDevice] fileread.dll 不可用（" << a_sWhy
                   << "），文件读取回退 QFile 直读";
    }
}

}   // namespace

CFrwDevice::CFrwDevice(const QString& a_sPath, QObject* a_pParent)
    : QIODevice(a_pParent), m_fileFallback(a_sPath) {
    m_sPath = a_sPath;
}

CFrwDevice::~CFrwDevice() {
    close();
}

bool CFrwDevice::ServiceAvailable() {
    return FrwApi().bAvailable;
}

bool CFrwDevice::exists() const {
    return QFile::exists(m_sPath);
}

bool CFrwDevice::open(OpenMode a_eMode) {
    // 只读组件：携带任何写标志直接拒绝（调用点全是 ReadOnly[|Text]）
    if (a_eMode & (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Truncate)) {
        return false;
    }
    const OpenMode eMode = a_eMode | QIODevice::ReadOnly;
    const bool bOk = OpenFrw() || OpenFallback();
    if (!bOk) {
        return false;
    }
    setOpenMode(eMode);
    return true;
}

bool CFrwDevice::OpenFrw() {
    const SFrwApi& stApi = FrwApi();
    if (stApi.pOpenW == nullptr) {
        WarnFallbackOnce(QStringLiteral("dll 未部署/符号缺失"));
        return false;
    }
    FRWHANDLE hFile = nullptr;
    unsigned long long ullSize = 0;
    const int iRc = stApi.pOpenW(reinterpret_cast<const wchar_t*>(m_sPath.utf16()),
                                 &hFile, &ullSize);
    if (iRc != kFrwOk || hFile == nullptr) {
        // 文件不存在/被锁等真实错误：QFile 兜底同样会失败（保持 QFile::open 同语义）；
        // 服务侧异常（拉起失败/协议错配/表满）也兜底，保证可用性优先。
        if (iRc != kFrwFileNotFound && iRc != kFrwInvalidParam) {
            WarnFallbackOnce(QStringLiteral("FRW_OpenW rc=%1").arg(iRc));
        }
        return false;
    }
    m_hFrw = hFile;
    m_ullSize = ullSize;
    m_bFallback = false;
    return true;
}

bool CFrwDevice::OpenFallback() {
    if (!m_fileFallback.open(QIODevice::ReadOnly)) {
        return false;   // QFile 也打不开：与 QFile::open 失败同语义
    }
    m_bFallback = true;
    return true;
}

void CFrwDevice::close() {
    QIODevice::close();   // 先复位基类缓冲/pos（内部会回调虚 close，无重入副作用）
    if (m_hFrw != nullptr) {
        const SFrwApi& stApi = FrwApi();
        if (stApi.pClose != nullptr) {
            stApi.pClose(m_hFrw);
        }
        m_hFrw = nullptr;
    }
    if (m_bFallback) {
        m_fileFallback.close();
        m_bFallback = false;
    }
    m_ullSize = 0;
    m_bCrPending = false;
}

qint64 CFrwDevice::size() const {
    if (m_bFallback) {
        return m_fileFallback.size();
    }
    return (m_hFrw != nullptr) ? static_cast<qint64>(m_ullSize) : qint64(0);
}

qint64 CFrwDevice::readData(char* a_pData, qint64 a_iMax) {
    if (a_iMax <= 0) {
        return 0;
    }
    // 兜底路径：QFile 直读（公开 read，自身缓冲；仅 FRW 不可用/打开失败时走到）
    if (m_bFallback) {
        return m_fileFallback.read(a_pData, a_iMax);
    }
    const SFrwApi& stApi = FrwApi();
    if (m_hFrw == nullptr || stApi.pRead == nullptr) {
        setErrorString(QStringLiteral("FRW 句柄缺失"));
        return -1;
    }
    // FRW 分块读（单次 ≤100MB 协议上限，取 64MB；短读=到尾终止条件，usage.md §6.2）
    const unsigned long long ullOff = static_cast<unsigned long long>(pos());
    qint64 iTotal = 0;
    while (iTotal < a_iMax) {
        const qint64 iWant = qMin<qint64>(a_iMax - iTotal, 64LL * 1024 * 1024);
        unsigned long long ullGot = 0;
        const int iRc = stApi.pRead(m_hFrw, ullOff + static_cast<unsigned long long>(iTotal),
                                    static_cast<unsigned long long>(iWant),
                                    a_pData + iTotal,
                                    static_cast<unsigned long long>(iWant), &ullGot);
        if (iRc == kFrwOk) {
            iTotal += static_cast<qint64>(ullGot);
            if (ullGot < static_cast<unsigned long long>(iWant)) {
                break;   // 尾部短读＝文件尾
            }
        } else if (iRc == kFrwEof) {
            break;       // offset ≥ 当前文件大小（正常到头信号）
        } else {
            setErrorString(QStringLiteral("FRW_Read rc=%1").arg(iRc));
            return (iTotal > 0) ? iTotal : -1;
        }
    }
    // QIODevice::Text：\r\n→\n 与 QFile 同款（就地压缩），处理跨块 \r 悬挂与 EOF 孤立 \r
    if (openMode() & QIODevice::Text) {
        qint64 iOut = 0;
        qint64 i = 0;
        if (m_bCrPending && iTotal > 0) {
            if (a_pData[0] != '\n') {
                a_pData[iOut++] = '\r';   // 上一块尾 \r 未配对：补发
            }
            m_bCrPending = false;
        }
        while (i < iTotal) {
            const char ch = a_pData[i++];
            if (ch == '\r') {
                if (i < iTotal) {
                    if (a_pData[i] == '\n') {
                        ++i;               // \r\n → \n
                    }
                    a_pData[iOut++] = '\r';   // 孤立 \r 保留
                } else {
                    m_bCrPending = true;   // 块尾悬挂：留待下一块配对
                }
            } else {
                a_pData[iOut++] = ch;
            }
        }
        if (iTotal == 0 && m_bCrPending) {
            a_pData[0] = '\r';             // EOF 仍未配对：补发后收尾
            m_bCrPending = false;
            return 1;
        }
        iTotal = iOut;
    }
    return iTotal;
}

qint64 CFrwDevice::writeData(const char* a_pData, qint64 a_iMax) {
    Q_UNUSED(a_pData);
    Q_UNUSED(a_iMax);
    setErrorString(QStringLiteral("CFrwDevice 只读"));
    return -1;
}
