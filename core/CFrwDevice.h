#pragma once

// CFrwDevice —— AutoCode 磁盘文件读取统一入口（2026-09-13 用户指令：所有读取文件的位置
// 用 fileread.dll）。实现 QIODevice 适配器：open→FRW_OpenW / readData→FRW_Read(分块 ≤64MB) /
// close→FRW_Close，QTextStream/QDataStream/readAll/readLine/seek 在各调用点原样工作。
// 三件套（fileread.dll + code.exe + filereadwrite.h，用法 E:\...\RaadFile\doc\usage.md）
// 缺失/失效时兜底 QFile 直读（单次 qWarning），保证未部署环境不瘫。
// 语义注意：①只读——open 携带写标志直接拒绝；②QIODevice::Text 的 \r\n→\n 转换与 QFile
// 同款（含跨块 \r 悬挂与 EOF 孤立 \r 补发）；③size() 为 OPEN 时刻快照（FRW 口径）。

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

    // QFile 兼容面（调用点只用了这两个 + readAll/read/QTextStream）
    QString fileName() const { return m_sPath; }
    void setFileName(const QString& a_sPath) { m_sPath = a_sPath; }
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
    QFile m_fileFallback;
    bool m_bCrPending = false;        // Text 模式 \r\n→\n 跨块悬挂状态
};
