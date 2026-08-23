#pragma once

#include <QString>

// 2026.08.20 自定义大模型条目（统一数据结构，DSH/Pi 两后端共用）。
//   原定义在 CProjectManager.h（[DshModels] 节）；PiAgent 多条目管理接入后迁到
//   本头（agent 层 runner 也要用，避免 agent → project 反向依赖）：
//   - DSH 后端：存 exe 同级 config.ini [DshModels] 节；
//   - Pi 后端：存 config.ini [PiModels] 节 + 合成写 ~/.pi/agent/models.json。
struct SDshModelEntry {
    QString sDisp;      // 显示名
    QString sBaseURL;   // 接入点 / 请求地址（anthropic-messages 兼容）
    QString sApiKey;    // 密钥明文（tooltip 已告知明文存储）
    QString sApi;       // API 类型 / 格式（默认 anthropic-messages）
    QString sModelId;   // 模型 id / 名称
    qint64  iCtx    = 200000;   // 上下文窗口（token）
    int     iMaxTok = 32768;    // 单次最大输出
};
