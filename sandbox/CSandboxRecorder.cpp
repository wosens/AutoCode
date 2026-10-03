// 内置测试沙箱录像线程实现（说明见同名 .h）。
#include "CSandboxRecorder.h"
#include "CSandboxCore.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>        // IMFMediaSource 等（先于 mfreadwrite，否则其声明不完整）
#include <mfreadwrite.h>
#include <comdef.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// —— MF SinkWriter 句柄集：一段视频一个实例（尺寸固定），尺寸变化重建。 ——
struct CSandboxRecorder::SPriv {
    IMFSinkWriter*   pWriter  = nullptr;
    DWORD            dwStream = 0;
    int              iW = 0, iH = 0;          // 当前段输入尺寸（偶数化后）
    QString          sSegPath;                 // 当前段文件
    int              iSeg = 1;                 // 段号（1 起）
    qint64           iLastPts = -1;            // 上一帧 100ns 时间戳（单调递增保证）

    void CloseSeg() {
        if (pWriter != nullptr) {
            // 2026.09.28 记录 Finalize 结果：失败时 MP4 封装未落盘完成（文件损坏），
            //   调用方据此报「保存失败」而非假成功。
            bSegOk = SUCCEEDED(pWriter->Finalize());
            pWriter->Release();
            pWriter = nullptr;
        }
    }

    bool bSegOk = true;   // 当前段 Finalize 是否成功
};

namespace {

// 段文件名：name.mp4 → name_p2.mp4（扩展名前插段号；首段原样）。
QString SegPath(const QString& a_sBase, int a_iSeg) {
    if (a_iSeg <= 1) {
        return a_sBase;
    }
    const int iDot = a_sBase.lastIndexOf(QLatin1Char('.'));
    if (iDot <= 0) {
        return a_sBase + QStringLiteral("_p%1").arg(a_iSeg);
    }
    return a_sBase.left(iDot) + QStringLiteral("_p%1").arg(a_iSeg)
         + a_sBase.mid(iDot);
}

} // namespace

CSandboxRecorder::CSandboxRecorder(HWND a_hWnd, int a_iFps, const QString& a_sPath,
                                   QObject* a_pParent)
    : QThread(a_pParent), m_hWnd(a_hWnd), m_iFps(qBound(1, a_iFps, 15)), m_sPath(a_sPath) {
    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_p = new SPriv;
}

CSandboxRecorder::~CSandboxRecorder() {
    RequestStop();
    // 2026.09.28 wait 超时校验：线程仍往跑时绝不能 delete m_p / CloseHandle（run()
    //   还在用它们——悬垂指针/句柄竞态，且析构运行中 QThread 会 qFatal）；超时宁可
    //   放弃回收（进程退出路径小泄漏）也不制造 UAF。Finalize 大文件可能超 3s，提到 10s。
    if (!wait(10000)) {
        qWarning("[CSandboxRecorder] 录像线程未在超时内退出，放弃资源回收防悬垂");
        return;
    }
    delete m_p;
    m_p = nullptr;
    if (m_hStopEvent != nullptr) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }
}

void CSandboxRecorder::RequestStop() {
    m_bStop = true;
    if (m_hStopEvent != nullptr) {
        SetEvent(m_hStopEvent);
    }
}

void CSandboxRecorder::SetClickMarker(int a_iX, int a_iY) {
    std::lock_guard<std::mutex> oLock(m_oMtx);
    m_iClickX = a_iX;
    m_iClickY = a_iY;
    m_iClickUntilMs = QDateTime::currentMSecsSinceEpoch() + 800;
}

// —— 段创建：MF H264 writer（输入 ARGB32，stride=w*4）——
bool CSandboxRecorder::BeginSeg(int a_iW, int a_iH) {
    m_p->CloseSeg();
    // 2026.09.26 崩溃修复：不再申请硬件编码（删 MF_READWRITE_ENABLE_HARDWARE_
    //   TRANSFORMS，缺省即软编）。实测本机 Intel 核显驱动 mfx_mft_h264ve_64.dll
    //   在硬编路径以野指针读崩溃（Execute - 副本 4 个 dump 同 RIP +0xB575E，
    //   EXCEPTION_ACCESS_VIOLATION，读取未映射地址），直接杀死 AutoCode 进程
    //   ——用户在资源管理器点 exe → 自动收纳录像即崩。原生崩溃无法拦截，
    //   稳定优先改软编（5fps 小窗 CPU 占用可忽略）。
    IMFSinkWriter* pWriter = nullptr;
    const QString sPath = SegPath(m_p->sSegPath, m_p->iSeg);
    const std::wstring wsPath = QDir::toNativeSeparators(sPath).toStdWString();
    HRESULT hr = MFCreateSinkWriterFromURL(wsPath.c_str(), nullptr, nullptr, &pWriter);
    if (FAILED(hr) || pWriter == nullptr) {
        return false;
    }
    IMFMediaType* pOut = nullptr;
    // 2026.09.28 失败校验：MFCreateMediaType 失败时 pOut 为空，旧实现直接解引用即崩。
    if (FAILED(MFCreateMediaType(&pOut)) || pOut == nullptr) {
        pWriter->Release();
        return false;
    }
    pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOut->SetUINT32(MF_MT_AVG_BITRATE, UINT32(qBound(300000.0, a_iW * a_iH * m_iFps * 0.6, 6000000.0)));
    MFSetAttributeSize(pOut, MF_MT_FRAME_SIZE, a_iW, a_iH);
    MFSetAttributeRatio(pOut, MF_MT_FRAME_RATE, m_iFps, 1);
    MFSetAttributeRatio(pOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = pWriter->AddStream(pOut, &m_p->dwStream);
    pOut->Release();
    if (FAILED(hr)) {
        pWriter->Release();
        return false;
    }
    IMFMediaType* pIn = nullptr;
    // 2026.09.28 失败校验（同 pOut）。
    if (FAILED(MFCreateMediaType(&pIn)) || pIn == nullptr) {
        pWriter->Release();
        return false;
    }
    pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    pIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pIn->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(a_iW * 4));
    MFSetAttributeSize(pIn, MF_MT_FRAME_SIZE, a_iW, a_iH);
    MFSetAttributeRatio(pIn, MF_MT_FRAME_RATE, m_iFps, 1);
    MFSetAttributeRatio(pIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = pWriter->SetInputMediaType(m_p->dwStream, pIn, nullptr);
    pIn->Release();
    if (FAILED(hr)) {
        pWriter->Release();
        return false;
    }
    if (FAILED(pWriter->BeginWriting())) {
        pWriter->Release();
        return false;
    }
    m_p->pWriter = pWriter;
    m_p->iW = a_iW;
    m_p->iH = a_iH;
    return true;
}

void CSandboxRecorder::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool bMf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    QElapsedTimer tClock;
    tClock.start();
    int iFrames = 0;
    QString sErr;

    // 捕获用 DIB（尺寸随窗口重建）。hBmpDef = DC 默认位图（首次选入时记下，
    //   重建/收尾前先换回再删 hBmp——GDI 选入 DC 的位图 DeleteObject 会失败泄漏）。
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = nullptr;
    HGDIOBJ hBmpDef = nullptr;
    void* pBits = nullptr;
    int iCapW = 0, iCapH = 0;
    const int iFrameMs = 1000 / qMax(1, m_iFps);

    while (!m_bStop) {
        if (WaitForSingleObject(m_hStopEvent, iFrameMs) == WAIT_OBJECT_0) {
            break;
        }
        if (!IsWindow(m_hWnd)) {
            break;   // 窗口销毁：封盘退出
        }
        RECT rc = {};
        if (!GetWindowRect(m_hWnd, &rc)) {
            break;
        }
        const int iW = qBound(0, static_cast<int>(rc.right - rc.left), 4096) & ~1;   // 偶数
        const int iH = qBound(0, static_cast<int>(rc.bottom - rc.top), 4096) & ~1;
        if (iW < 8 || iH < 8) {
            continue;
        }
        if (iW != iCapW || iH != iCapH || hBmp == nullptr) {
            if (hBmp != nullptr) {
                if (hBmpDef != nullptr) {
                    SelectObject(hMem, hBmpDef);   // 2026.09.28 先换出再删（防 GDI 位图泄漏）
                }
                DeleteObject(hBmp);
                hBmp = nullptr;
            }
            BITMAPINFO stBi = {};
            stBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            stBi.bmiHeader.biWidth = iW;
            stBi.bmiHeader.biHeight = -iH;
            stBi.bmiHeader.biPlanes = 1;
            stBi.bmiHeader.biBitCount = 32;
            stBi.bmiHeader.biCompression = BI_RGB;
            hBmp = CreateDIBSection(hScreen, &stBi, DIB_RGB_COLORS, &pBits, nullptr, 0);
            if (hBmp == nullptr) {
                continue;
            }
            const HGDIOBJ hOldBmp = SelectObject(hMem, hBmp);
            if (hBmpDef == nullptr) {
                hBmpDef = hOldBmp;   // 记住 DC 默认位图，供后续换出
            }
            iCapW = iW;
            iCapH = iH;
            if (m_p->pWriter != nullptr) {
                ++m_p->iSeg;   // 尺寸变化：另起新段文件
            }
            m_p->sSegPath = m_sPath;
            m_p->iLastPts = -1;
            if (!BeginSeg(iW, iH)) {
                if (m_p->iSeg == 1) {
                    sErr = QStringLiteral("MP4 编码器创建失败");
                }
                break;
            }
        }
        if (m_p->pWriter == nullptr) {
            continue;
        }
        if (!PrintWindow(m_hWnd, hMem, PW_RENDERFULLCONTENT)) {
            HDC hWnd = GetWindowDC(m_hWnd);
            if (hWnd != nullptr) {
                BitBlt(hMem, 0, 0, iCapW, iCapH, hWnd, 0, 0, SRCCOPY | CAPTUREBLT);
                ReleaseDC(m_hWnd, hWnd);
            }
        }
        // —— 帧叠加：时间戳条 + 点击红点 ——
        const qint64 iNowMs = QDateTime::currentMSecsSinceEpoch();
        int iCx = -1, iCy = -1;
        {
            std::lock_guard<std::mutex> oLock(m_oMtx);
            if (iNowMs < m_iClickUntilMs) {
                iCx = m_iClickX;
                iCy = m_iClickY;
            }
        }
        {
            const qint64 iEl = tClock.elapsed() / 1000;
            HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const HGDIOBJ hOldF = SelectObject(hMem, hFont);
            SetBkMode(hMem, TRANSPARENT);
            SetTextColor(hMem, RGB(0, 0, 0));
            const QString sTs = QStringLiteral("%1:%2.%3")
                .arg(iEl / 60, 2, 10, QChar('0')).arg(iEl % 60, 2, 10, QChar('0'))
                .arg((tClock.elapsed() % 1000) / 100);
            RECT rcTs = { 2, 2, 2 + 14 * static_cast<int>(sTs.size()) + 8, 22 };
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 160));
            FillRect(hMem, &rcTs, hBrush);
            DeleteObject(hBrush);
            DrawTextW(hMem, reinterpret_cast<const WCHAR*>(sTs.utf16()), sTs.size(),
                      &rcTs, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hMem, hOldF);
        }
        if (iCx >= 0 && iCy >= 0) {
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));
            HPEN hPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
            const HGDIOBJ hOldB = SelectObject(hMem, hBrush);
            const HGDIOBJ hOldP = SelectObject(hMem, hPen);
            Ellipse(hMem, iCx - 7, iCy - 7, iCx + 7, iCy + 7);
            SelectObject(hMem, hOldB);
            SelectObject(hMem, hOldP);
            DeleteObject(hBrush);
            DeleteObject(hPen);
        }
        // —— alpha 通道补满（DIB 的 alpha 位为 0 会让编码器输出全黑）——
        {
            BYTE* pRow = static_cast<BYTE*>(pBits);
            const int iStride = iCapW * 4;
            for (int y = 0; y < iCapH; ++y) {
                BYTE* p = pRow + 3;
                for (int x = 0; x < iCapW; ++x) {
                    *p = 0xFF;
                    p += 4;
                }
                pRow += iStride;
            }
        }
        // —— 写帧（100ns 时间戳严格单调）——
        qint64 iPts = tClock.elapsed() * 10000;
        if (iPts <= m_p->iLastPts) {
            iPts = m_p->iLastPts + 1;
        }
        const qint64 iDur = 10000000 / qMax(1, m_iFps);
        IMFSample* pSample = nullptr;
        IMFMediaBuffer* pBuf = nullptr;
        if (SUCCEEDED(MFCreateMemoryBuffer(iCapW * iCapH * 4, &pBuf)) && pBuf != nullptr) {
            BYTE* pDst = nullptr;
            if (SUCCEEDED(pBuf->Lock(&pDst, nullptr, nullptr)) && pDst != nullptr) {
                memcpy(pDst, pBits, static_cast<size_t>(iCapW) * iCapH * 4);
                pBuf->Unlock();
                pBuf->SetCurrentLength(iCapW * iCapH * 4);
                if (SUCCEEDED(MFCreateSample(&pSample)) && pSample != nullptr) {
                    pSample->AddBuffer(pBuf);
                    pSample->SetSampleTime(iPts);
                    pSample->SetSampleDuration(iDur);
                    if (FAILED(m_p->pWriter->WriteSample(m_p->dwStream, pSample))) {
                        sErr = QStringLiteral("写帧失败");
                        pSample->Release();
                        pBuf->Release();
                        break;
                    }
                    pSample->Release();
                }
            }
            pBuf->Release();
            ++iFrames;
            m_p->iLastPts = iPts;
        }
    }

    if (hBmp != nullptr) {
        if (hBmpDef != nullptr) {
            SelectObject(hMem, hBmpDef);   // 2026.09.28 先换出再删（防 GDI 位图泄漏）
        }
        DeleteObject(hBmp);
    }
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    m_p->CloseSeg();
    const bool bSegOk = m_p->bSegOk;
    if (bMf) {
        MFShutdown();
    }
    CoUninitialize();
    const QString sInfo = (iFrames > 0)
        ? (bSegOk
           ? QStringLiteral("已保存 %1（%2 帧 / %3 秒）").arg(m_sPath).arg(iFrames)
                 .arg(tClock.elapsed() / 1000)
           : QStringLiteral("录像保存失败（MP4 封装未完成，文件可能损坏）：%1").arg(m_sPath))
        : QStringLiteral("未产出录像：%1").arg(sErr.isEmpty()
              ? QStringLiteral("无有效帧") : sErr);
    emit sigFinished(sInfo);
}
