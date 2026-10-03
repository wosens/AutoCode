#include "CFrwDevice.h"

// FRW 三件套接入（动态加载免 .lib；协议与错误码见 filereadwrite.h / ReadFile doc/usage.md）
#include "filereadwrite.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QtGlobal>
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
    bool bAvailable = false;   // 结论：三件套可用与否
    bool bTried = false;       // 是否已得出结论（无论成败只探测一次，不再重试）
};
const SFrwApi& FrwApi() {
    static SFrwApi s_st;
    static QMutex s_oMtx;
    QMutexLocker oLock(&s_oMtx);
    if (s_st.bTried) {
        return s_st;   // 已得出结论（可用或不可用）：闭锁不再重试（旧实现 dll 缺失时每次
                       //   调用都重新 LoadLibrary，模块引用计数持续增长）
    }
    s_st.bTried = true;
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
            s_st.pOpenW = nullptr;
            s_st.pRead = nullptr;
            s_st.pClose = nullptr;
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
    // 已打开再 open：对齐 QFile 语义直接失败（旧实现会覆盖 m_hFrw 造成句柄泄漏）。
    if (isOpen()) {
        qWarning("[CFrwDevice] open 被拒绝：设备已打开（先 close 再重开）");
        return false;
    }
    const OpenMode eMode = (a_eMode & ~QIODevice::ReadOnly) | QIODevice::ReadOnly;
    m_bWantText = (eMode & QIODevice::Text) != 0;
    const bool bOk = OpenFrw() || OpenFallback();
    if (!bOk) {
        return false;
    }
    m_ullRawPos = 0;   // 新开启：原始偏移与逻辑位归零
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
    // 兜底直读带上调用方请求的 Text 标志（QFile 自做 \r\n→\n，与 FRW 路径口径一致）
    const OpenMode eMode = QIODevice::ReadOnly | (m_bWantText ? QIODevice::Text : OpenMode());
    if (!m_fileFallback.open(eMode)) {
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
    m_ullRawPos = 0;
}

bool CFrwDevice::seek(qint64 a_iPos) {
    // 原始偏移与逻辑位同置（生产代码仅在二进制模式 seek——Claude transcript 尾读；
    // Text+seek 的逻辑→原始映射无闭式解，按原始偏移近似并注明）。
    // 兑底 QFile 路径：QIODevice::seek 只改逻辑位，必须同步兑底句柄，否则后续 read
    //   仍从 QFile 原位置读取返回错位数据。
    if (m_bFallback) {
        if (!m_fileFallback.seek(a_iPos)) {
            return false;
        }
    }
    m_ullRawPos = (a_iPos > 0) ? static_cast<unsigned long long>(a_iPos) : 0;
    m_bCrPending = false;
    return QIODevice::seek(a_iPos);
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
    // 兜底路径：QFile 直读（自身处理 Text 转换；仅 FRW 不可用/打开失败时走到）
    if (m_bFallback) {
        return m_fileFallback.read(a_pData, a_iMax);
    }
    const SFrwApi& stApi = FrwApi();
    if (m_hFrw == nullptr || stApi.pRead == nullptr) {
        setErrorString(QStringLiteral("FRW 句柄缺失"));
        return -1;
    }
    // FRW 按偏移随机读（无 native 指针）：基址 = 原始流偏移 m_ullRawPos（非 pos()！
    //   Text 转换收缩后逻辑位≠原始偏移，取 pos() 会把块尾 \r\n 对二次消费）。
    //   取块 ≤64MB（协议上限 100MB，usage.md §5 LENGTH_EXCEEDED=3）；短读 = 文件尾信号。
    const bool bText = (openMode() & QIODevice::Text) != 0;
    const unsigned long long ullStart = m_ullRawPos;   // 本次取数起点（块偏移唯一口径：
    //   恒用 ullStart+iRaw；旧实现每块同时推进 iRaw 与 ullBase 会双计偏移）
    qint64 iRaw = 0;        // 已取回的原始字节数
    bool bEof = false;
    qint64 iOut = 0;        // Text 转换后的可交付字节数（非 Text 时恒等于 iRaw）
    while (true) {
        // ---- 取块 ----
        if (iRaw < a_iMax && !bEof) {
            const qint64 iWant = qMin<qint64>(a_iMax - iRaw, 64LL * 1024 * 1024);
            unsigned long long ullGot = 0;
            const int iRc = stApi.pRead(m_hFrw, ullStart + static_cast<unsigned long long>(iRaw),
                                        static_cast<unsigned long long>(iWant),
                                        a_pData + iRaw,
                                        static_cast<unsigned long long>(iWant), &ullGot);
            if (iRc == kFrwOk) {
                iRaw += static_cast<qint64>(ullGot);
                if (ullGot < static_cast<unsigned long long>(iWant)) {
                    bEof = true;   // 尾部短读＝文件尾
                }
            } else if (iRc == kFrwEof) {
                bEof = true;       // offset ≥ 当前文件大小（正常到头）
            } else {
                setErrorString(QStringLiteral("FRW_Read rc=%1").arg(iRc));
                if (iRaw == 0) {
                    return -1;
                }
                bEof = true;   // 中途出错但已取到部分数据：停止取块，转换后交付已取部分
            }
        }
        // ---- Text 转换（就地压缩；与 QFile 口径：\r\n→\n，孤立 \r 保留） ----
        if (!bText) {
            if (iRaw > 0 || bEof) {
                m_ullRawPos = ullStart + static_cast<unsigned long long>(iRaw);   // 记账（单口径）
                return iRaw;
            }
            continue;              // 还没取到任何字节，继续取
        }
        iOut = 0;
        qint64 i = 0;
        if (m_bCrPending && iRaw > 0) {
            if (a_pData[0] == '\n') {
                a_pData[iOut++] = '\n';   // 跨块配对成功：补出换行（旧实现仅 ++i 跳过 '\n'，
                ++i;                       //   \r 与 \n 双双被吞，块边界处丢一个换行）
            } else {
                a_pData[iOut++] = '\r';   // 上一块尾 \r 未配对：先补发
            }
            m_bCrPending = false;
        }
        while (i < iRaw) {
            const char ch = a_pData[i++];
            if (ch == '\r') {
                if (i < iRaw) {
                    if (a_pData[i] == '\n') {
                        ++i;               // 吃掉 \r\n 对
                        a_pData[iOut++] = '\n';
                    } else {
                        a_pData[iOut++] = '\r';   // 孤立 \r 保留
                    }
                } else {
                    m_bCrPending = true;   // 块尾悬挂：留待下一块/EOF 配对
                }
            } else {
                a_pData[iOut++] = ch;
            }
        }
        // ---- 交付判定（Text）----
        if (iOut > 0) {
            m_ullRawPos = ullStart + static_cast<unsigned long long>(iRaw);   // 提交所消费的原始字节记账
            return iOut;
        }
        if (bEof) {
            m_ullRawPos = ullStart + static_cast<unsigned long long>(iRaw);   // EOF 收尾也记账（悬挂 \r 已在本块前交付/补发）
            if (m_bCrPending) {
                a_pData[0] = '\r';       // EOF 仍未配对：QFile 口径补发孤立 \r
                m_bCrPending = false;
                return 1;
            }
            return 0;                    // 真正到头
        }
        if (iRaw >= a_iMax) {
            // 请求长度内全是 \r 且未到文件尾（悬挂未决）：交 0 会被 QIODevice 判为
            // EOF 造成假到头——退回原始字节直接交付（丢弃悬挂状态，后续 \n 极罕见地
            // 以裸形态出现；read(1) 命中 '\r' 的概率工程上可忽略，保正确性优先）。
            m_bCrPending = false;
            m_ullRawPos = ullStart + static_cast<unsigned long long>(iRaw);
            return iRaw;
        }
        // iOut==0 且还有取数空间：继续取下一块解析悬挂
    }
}

qint64 CFrwDevice::writeData(const char* a_pData, qint64 a_iMax) {
    Q_UNUSED(a_pData);
    Q_UNUSED(a_iMax);
    setErrorString(QStringLiteral("CFrwDevice 只读"));
    return -1;
}
