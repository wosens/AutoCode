#pragma once

// 内置测试沙箱（2026-09-26）每窗口录像线程。
// 循环：PrintWindow 离屏截图（窗口已幽灵化仍持续重绘，截图像素完整）→ 帧上叠加
// 时间戳与最近点击红点 → Media Foundation SinkWriter 编码 H264 写 MP4。
// 窗口尺寸变化时封段另起新文件（H264 输入尺寸在 BeginWriting 时固定）。
// 线程模型：独立 QThread 自持消息循环不适用（无 Qt 事件），全部用原子/互斥控制；
// 主线程仅 RequestStop() / SetClickMarker()，退出以 finished 信号收尾。

#include <QThread>
#include <QString>
#include <atomic>
#include <mutex>
#include <windows.h>

class CSandboxRecorder : public QThread {
    Q_OBJECT

public:
    // a_sPath：首段 MP4 输出路径（后续段自动追加 _p2/_p3…）。
    CSandboxRecorder(HWND a_hWnd, int a_iFps, const QString& a_sPath,
                     QObject* a_pParent = nullptr);
    ~CSandboxRecorder() override;

    void RequestStop();                 // 异步停（封盘后线程自然退出）
    void SetClickMarker(int a_iX, int a_iY);   // 主线程点击后叠加红点约 800ms
    QString FirstPath() const { return m_sPath; }

signals:
    // 录像结束：a_sInfo = 「已保存 <路径>（N 秒）」或错误说明。
    void sigFinished(const QString& a_sInfo);

protected:
    void run() override;

private:
    struct SPriv;
    bool BeginSeg(int a_iW, int a_iH);   // 建/重建 H264 段 writer（尺寸变化时）
    SPriv* m_p = nullptr;

    HWND             m_hWnd = nullptr;
    int              m_iFps = 5;
    QString          m_sPath;
    std::atomic_bool m_bStop { false };
    HANDLE           m_hStopEvent = nullptr;   // 立即响应停止（替代纯 Sleep 轮询）
    std::mutex       m_oMtx;                    // 点击标记/尺寸缓存
    int              m_iClickX = -1, m_iClickY = -1;
    qint64           m_iClickUntilMs = 0;
};
