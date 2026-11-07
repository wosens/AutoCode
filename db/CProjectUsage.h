#pragma once

// ---------------------------------------------------------------------------
// CProjectUsage —— 工程总消耗聚合（2026-11-05 菜单「统计工程总的消耗」）。
//
// 口径（与 2026.09.05 统计隔离决策一致，两源相加即总量、无重复计数）：
//   ① 任务用量：每个工程一份 <工程>/.autodev/history.db（CTaskHistoryDB），
//      每行 cost/in_tok/out_tok/req_count 即该任务全程累计（含其各阶段/评审视角）。
//   ② 助手用量：assistant.json 档内 assistants[].usage{cost,in,out,reqs,ms}——
//      工程级档 <工程>/.autodev/assistant.json + 每任务档
//      <工程>/.autodev/tasks/<id>/assistant.json。2026.09.05 起助手消耗只归
//      助手自身（不并入任务统计），故任务+助手两源直接求和。
//   容错：库/档缺失或个别 JSON 坏行按 0 计（统计类功能不因单点损坏瘫痪）。
// ---------------------------------------------------------------------------

#include <QString>

struct SProjectUsage {
    // —— 任务侧（history.db 全表聚合）——
    double m_dTaskCost = 0.0;
    qint64 m_iTaskIn   = 0;
    qint64 m_iTaskOut  = 0;
    int    m_iTaskReqs = 0;
    int    m_nTasks    = 0;      // 参与聚合的任务行数

    // —— 助手侧（全部 assistant.json 档聚合）——
    double m_dAsstCost = 0.0;
    qint64 m_iAsstIn   = 0;
    qint64 m_iAsstOut  = 0;
    int    m_iAsstReqs = 0;
    int    m_nAssts    = 0;      // 参与聚合的助手个数

    // —— 合计 ——
    double TotalCost() const { return m_dTaskCost + m_dAsstCost; }
    qint64 TotalIn()   const { return m_iTaskIn + m_iAsstIn; }
    qint64 TotalOut()  const { return m_iTaskOut + m_iAsstOut; }
    int    TotalReqs() const { return m_iTaskReqs + m_iAsstReqs; }
    qint64 TotalTok()  const { return TotalIn() + TotalOut(); }
};

class CProjectUsage {
public:
    // 聚合指定工程的全部消耗（工程根目录；空/不存在目录返回全 0）。
    // 纯读、短连接（不锁库文件），随时可调（菜单打开对话框时现算即可）。
    static SProjectUsage Aggregate(const QString& a_sProjectDir);

private:
    // 累加单个 assistant.json 档（文件不存在/坏 JSON 静默按 0）。
    static void SumAssistantArchive(const QString& a_sJsonPath, SProjectUsage* a_pOut);
};
