#pragma once

#include <QString>
#include <QList>
#include <QChar>
#include <QLatin1String>

// 2026-10-08 提示词 token 本地估算（发送闸门用，零依赖零网络）：CJK 字符 ≈1 token、
//   其余 ≈1/3 token（保守偏大取整），仅作阈值判定，与模型实际计量存在小偏差。
//   置于本头供面板（发送闸门）与编排器（提炼轮收尾校验）单源共用。
inline int PromptTokenEstimate(const QString& a_sText) {
    int iCjk = 0;
    int iOther = 0;
    for (const QChar& ch : a_sText) {
        if (ch.isSpace()) {
            continue;
        }
        const char16_t u = ch.unicode();
        if ((u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF)
                || (u >= 0x3000 && u <= 0x303F) || (u >= 0xFF00 && u <= 0xFF60)) {
            ++iCjk;
        } else {
            ++iOther;
        }
    }
    return iCjk + (iOther + 2) / 3;
}

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

// 2026.09.10 公共模型配置（用户需求：切换后端不用重配模型）——四后端（DSH/Pi/
//   Codex/Hermes）共享同一份模型列表与「当前」条目。四张节（[DshModels]/[PiModels]/
//   [CodexModels]/[HermesModels]）继续作为存储正本（任务级覆盖/全局超集等既有
//   机制不变），读侧经本合并函数归一为一份共享表供全部 runner 使用；设置保存时
//   同一份内容写回四张节。
struct SSharedModels {
    QList<SDshModelEntry> aModels;
    int                   iActive = -1;   // 共享「当前」条目下标（-1=无激活）
};

// 合并身份归一：codex 侧的 "chat" 与 "openai-completions" 协议等价（CCodexRunner
//   对非 responses 一律按 chat 处理），归一为后者——防 codex 词汇经共享表泄漏到
//   pi/dsh/hermes 侧（pi 的 models.json 不认识 "chat"）。往返安全：归一后的
// openai-completions 写回 [CodexModels]，codex 读到仍按 chat 处理。
inline QString NormalizeModelApiForMerge(const QString& a_sApi) {
    QString sApi = a_sApi.trimmed().toLower();
    if (sApi == QLatin1String("chat")) {
        sApi = QStringLiteral("openai-completions");
    }
    return sApi;
}

// 条目身份串：URL + 归一 Api + ModelId（忽略大小写）——合并去重与 2026-10-07
// 对话级模型覆盖解析共用同一口径。
inline QString ModelEntryIdentity(const SDshModelEntry& a_stE) {
    return a_stE.sBaseURL.trimmed().toLower() + QChar(1)
         + NormalizeModelApiForMerge(a_stE.sApi) + QChar(1)
         + a_stE.sModelId.trimmed().toLower();
}

// 2026-10-07 按身份在表中找下标（-1 = 不在表内）。对话级模型覆盖不能硬存下标跨次
// 使用（共享表内容可能已变），每次覆盖生效前用条目快照现解析。
inline int SharedModelIndexByIdentity(const QList<SDshModelEntry>& a_aTbl,
                                      const SDshModelEntry& a_stE) {
    const QString sId = ModelEntryIdentity(a_stE);
    for (int i = 0; i < a_aTbl.size(); ++i) {
        if (ModelEntryIdentity(a_aTbl.at(i)) == sId) {
            return i;
        }
    }
    return -1;
}

// 四张同构模型表合并为一份共享表：
//   - 基底 = 条目数最多的表（并列取传入顺序靠前者——调用方按优先级传参），
//     保持基底原序；其余表按传入顺序把「新身份」条目追加到末尾；
//   - 身份 = URL + 归一化 Api + ModelId（忽略大小写；与 SyncModelTableToGlobal 同口径
//     外加 chat 归一）；
//   - 共享当前项优先级：a_iPrefTable（调用方传入的当前后端表，保任务级/后端级
//     已选模型不被大表盖掉）→ 基底表 → 传入顺序首个有效；均无效 = -1。
inline SSharedModels MergeSharedModelTables(const QList<QList<SDshModelEntry>>& a_aTables,
                                            const QList<int>& a_aActives,
                                            int a_iPrefTable = -1) {
    SSharedModels stOut;
    const int iN = static_cast<int>(qMin(a_aTables.size(), a_aActives.size()));
    if (iN <= 0) {
        return stOut;
    }
    int iBase = 0;
    for (int i = 1; i < iN; ++i) {
        if (a_aTables.at(i).size() > a_aTables.at(iBase).size()) {
            iBase = i;
        }
    }
    auto fnIdent = [](const SDshModelEntry& a_stE) -> QString {
        return ModelEntryIdentity(a_stE);
    };
    QList<int> aOrder;
    aOrder.reserve(iN);
    aOrder.append(iBase);
    for (int i = 0; i < iN; ++i) {
        if (i != iBase) {
            aOrder.append(i);
        }
    }
    QList<QString> aSeen;
    for (int t : aOrder) {
        for (const SDshModelEntry& stE : a_aTables.at(t)) {
            const QString sId = fnIdent(stE);
            if (aSeen.contains(sId)) {
                continue;
            }
            aSeen.append(sId);
            SDshModelEntry stM = stE;
            stM.sApi = NormalizeModelApiForMerge(stM.sApi);
            stOut.aModels.append(stM);
        }
    }
    // 共享当前项：表间下标不通用，按身份映射到合并后下标。优先级 = 调用方指定的
    //   当前后端表 → 基底表 → 传入顺序首个有效（aOrder 首元素即基底，其后按序）。
    QList<int> aPrefOrder;
    if (a_iPrefTable >= 0 && a_iPrefTable < iN && a_iPrefTable != iBase) {
        aPrefOrder.append(a_iPrefTable);
    }
    aPrefOrder.append(aOrder);
    for (int t : aPrefOrder) {
        const int iAct = a_aActives.at(t);
        if (iAct < 0 || iAct >= a_aTables.at(t).size()) {
            continue;
        }
        const QString sId = fnIdent(a_aTables.at(t).at(iAct));
        for (int j = 0; j < stOut.aModels.size(); ++j) {
            if (fnIdent(stOut.aModels.at(j)) == sId) {
                stOut.iActive = j;
                break;
            }
        }
        if (stOut.iActive >= 0) {
            break;
        }
    }
    return stOut;
}
