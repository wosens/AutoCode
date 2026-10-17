#pragma once

// ---------------------------------------------------------------------------
// SBlockData —— 一条对话流中单个消息块的数据快照（2026.09.03 自 CExecutionPanel
// 嵌套结构迁出为全局——任务日志与各助手日志共用 CStreamPresenter，数据结构须独立
// 于面板；2026-10-17 自绘三期自 CStreamPresenter.h 再拆独立头：CChatModel 等自绘
// 链组件需要 SBlockData 而无 presenter 依赖，且 CStreamPresenter.h 持
// QPointer<CChatModel>（完整类型约束）与 CChatModel.h 需 SBlockData 构成环，拆头解环）。
// 由流呈现器持有；面板按 taskId（任务流）/助手 id（助手流）路由落盘
//（chat.json/assistant.json，BlockToJson/BlockFromJson 恒零迁移）。
// ---------------------------------------------------------------------------

#include <QString>
#include <QJsonObject>

#include "CMessageBlock.h"

struct SBlockData {
    CMessageBlock::EKind eKind = CMessageBlock::Assistant;
    QString sText;        // Assistant/Event/System/Error 文本
    QString sTool;        // ToolUse 工具名
    QString sInput;       // ToolUse 入参 JSON
    QString sResult;      // ToolUse 结果（到达后回填）
    QString sEventKind;   // Event 的 kind
    QString sViewId;      // 评审多视角并行的视角标识（v1/v2/...；空=串行路径）
    QString sBackend;     // Assistant 块的产出后端显示名（消息到达时拍摄，随块持久化）
};

// SBlockData ↔ JSON（chat.json/assistant.json 共用；inline 纯函数无 ODR 顾虑）
inline QJsonObject BlockToJson(const SBlockData& a_stData) {
    QJsonObject o;
    if (!a_stData.sViewId.isEmpty()) {
        o[QStringLiteral("view")] = a_stData.sViewId;   // 评审多视角标识（向后兼容：旧记录无此键）
    }
    switch (a_stData.eKind) {
        case CMessageBlock::Assistant:
            o[QStringLiteral("kind")] = QStringLiteral("assistant");
            o[QStringLiteral("text")] = a_stData.sText;
            // 产出后端随块持久化（旧记录无此键，读取容错为空）
            if (!a_stData.sBackend.isEmpty()) {
                o[QStringLiteral("backend")] = a_stData.sBackend;
            }
            break;
        case CMessageBlock::ToolUse:
            o[QStringLiteral("kind")]   = QStringLiteral("tool_use");
            o[QStringLiteral("tool")]   = a_stData.sTool;
            o[QStringLiteral("input")]  = a_stData.sInput;
            o[QStringLiteral("result")] = a_stData.sResult;
            break;
        case CMessageBlock::Event:
            o[QStringLiteral("kind")]      = QStringLiteral("event");
            o[QStringLiteral("eventKind")] = a_stData.sEventKind;
            o[QStringLiteral("text")]      = a_stData.sText;
            break;
        case CMessageBlock::Error:
            o[QStringLiteral("kind")] = QStringLiteral("error");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
        case CMessageBlock::System:
        default:
            o[QStringLiteral("kind")] = QStringLiteral("system");
            o[QStringLiteral("text")] = a_stData.sText;
            break;
    }
    return o;
}

inline SBlockData BlockFromJson(const QJsonObject& a_o) {
    SBlockData st;
    st.sViewId = a_o.value(QStringLiteral("view")).toString();
    const QString sKind = a_o.value(QStringLiteral("kind")).toString();
    if (sKind == QStringLiteral("assistant")) {
        st.eKind = CMessageBlock::Assistant;
        st.sText = a_o.value(QStringLiteral("text")).toString();
        st.sBackend = a_o.value(QStringLiteral("backend")).toString();   // 产出后端（旧记录无→空）
    } else if (sKind == QStringLiteral("tool_use")) {
        st.eKind   = CMessageBlock::ToolUse;
        st.sTool   = a_o.value(QStringLiteral("tool")).toString();
        st.sInput  = a_o.value(QStringLiteral("input")).toString();
        st.sResult = a_o.value(QStringLiteral("result")).toString();
    } else if (sKind == QStringLiteral("event")) {
        st.eKind      = CMessageBlock::Event;
        st.sEventKind = a_o.value(QStringLiteral("eventKind")).toString();
        st.sText      = a_o.value(QStringLiteral("text")).toString();
    } else if (sKind == QStringLiteral("error")) {
        st.eKind = CMessageBlock::Error;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    } else {
        st.eKind = CMessageBlock::System;
        st.sText = a_o.value(QStringLiteral("text")).toString();
    }
    return st;
}
