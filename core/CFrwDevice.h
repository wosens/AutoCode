#pragma once

// CFrwDevice —— AutoCode 磁盘文件读取统一入口（2026-09-13 用户指令：所有读取文件的位置
// 用 fileread.dll）。实现 QIODevice 适配器：open→FRW_OpenW / readData→FRW_Read(分块 ≤64MB) /
// close→FRW_Close，QTextStream/QDataStream/readAll/readLine/seek 在各调用点原样工作。
// 三件套（fileread.dll + code.exe + filereadwrite.h，用法 E:\...\ReadFile\doc\usage.md）
// 缺失/失效时兜底 QFile 直读（单次 qWarning），保证未部署环境不瘫。
// 语义注意：①只读——open 携带写标志直接拒绝；②QIODevice::Text 的 \r\n→\n 转换与 QFile
// 同款（含跨块 \r 悬挂与 EOF 孤立 \r 补发）；③size() 为 OPEN 时刻快照（FRW 口径）；
// ④原始偏移 m_ullRawPos 独立于逻辑 pos() 跟踪（Text 收缩后二者不等——原始读基址
// 绝不能取 pos()，否则块尾 \r\n 对被二次消费）；seek(n) 把原始偏移同置 n
//（生产代码仅在二进制模式 seek，Text+seek 语义按原始偏移近似）。

#include <QIODevice>
#include <QFile>
#include <QString>

class CFrwDevice : public QIODevice {
public:
    explicit CFrwDevice(const QString& a_sPath = QString(), QObject* a_pParent = nullptr);
    ~CFrwDevice() override;

    // QIODevice 契约
    bool open(OpenMode a_eMode) override;
    void close() override;
    bool isSequential() const override { return false; }
    qint64 size() const override;
    bool seek(qint64 a_iPos) override;

    // QFile 兼容面（调用点只用了这两个 + readAll/read/QTextStream）
    QString fileName() const { return m_sPath; }
    // 打开状态下不可改路径（句柄/兑底 QFile 仍握旧路径，改了会与 fileName() 不一致）；
    //   对齐 QFile「打开期间勿改文件名」约定：打开中调用直接忽略并告警。
    void setFileName(const QString& a_sPath) {
        if (isOpen()) {
            qWarning("[CFrwDevice] setFileName 被忽略：设备已打开（先 close）");
            return;
        }
        m_sPath = a_sPath;
    }
    bool exists() const;

    // FRW 服务是否可用（三件套已部署且符号齐）——测试与诊断用
    static bool ServiceAvailable();

protected:
    qint64 readData(char* a_pData, qint64 a_iMax) override;
    qint64 writeData(const char* a_pData, qint64 a_iMax) override;

private:
    bool OpenFrw();      // FRW 主路径；失败转 OpenFallback
    bool OpenFallback();

    QString m_sPath;
    void* m_hFrw = nullptr;           // FRWHANDLE（FRW 侧持有，close 释放）
    unsigned long long m_ullSize = 0; // OPEN 时刻快照（FRW 出参）
    bool m_bFallback = false;         // FRW 不可用/打开失败 → QFile 兜底直读
    bool m_bWantText = false;         // 调用方请求的 Text 标志（兜底 QFile 同口径）
    unsigned long long m_ullRawPos = 0; // 原始流偏移（Text 收缩后 ≠ pos()，读基址专用）
    QFile m_fileFallback;
    bool m_bCrPending = false;        // Text 模式 \r\n→\n 跨块悬挂状态
};
