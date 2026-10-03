#pragma once

#include <QObject>

#include "CAgentRunnerBase.h"
#include "CClaudeRunner.h"
#include "CCodexRunner.h"
#include "CDeepSeekRunner.h"
#include "CPiAgentRunner.h"
#include "CHermesRunner.h"

// Agent 后端工厂（header-only）。
//
// 2026.08.18 多后端支持：后端为**全局单选**（config.ini [Agent] Backend，启动时由
// CMainWindow 回填 SetBackend，设置变更时经漂移重建生效）。所有持有点（任务/评审
// fan-out/ad-hoc/guide×2）统一经 Create() 拿 runner，不再裸 new 具体类。
//
// 2026.08.19 第三后端 PIAgent（pi coding-agent CLI，Backend=3）：`--mode json`
// NDJSON 流式接入（进程模型同 claude：一阶段一进程）。
//
// 后端漂移重建约定：持有点发现既有 runner 的 BackendName() 与工厂当前后端不一致且
// 不在 Running/Grace/Retry 占用时，deleteLater 旧 runner 再 Create 新的。
class CAgentRunnerFactory {
public:
    enum EBackend {
        BackendClaude,   // Claude Code CLI（原路径）
        BackendDsh,      // DeepSeekHarness（JSON-RPC 完整接入）
        BackendPi,         // PIAgent（pi coding-agent，--mode json 流式接入）
        BackendCodex,     // CodexHarness（OpenAI Codex CLI 0.87.0，exec --json 接入）
        BackendHermes    // Hermes（第五后端，任务 3213cdbf；CLI 子进程+网关基线）
    };

    static void SetBackend(EBackend a_eBackend) { s_eBackend = a_eBackend; }
    static EBackend Backend() { return s_eBackend; }
    static bool IsDsh() { return s_eBackend == BackendDsh; }
    static bool IsPi() { return s_eBackend == BackendPi; }
    static bool IsCodex() { return s_eBackend == BackendCodex; }
    static bool IsHermes() { return s_eBackend == BackendHermes; }
    // 当前后端名（与各 runner BackendName() 同词汇表，漂移检测直接字符串比较）。
    static QString BackendName() {
        return NameOf(s_eBackend);
    }
    // 2026.09.20 助手粘性后端：任意后端枚举→名（与 BackendName 同源防漂移）。
    static QString NameOf(EBackend e) {
        switch (e) {
            case BackendDsh:   return QStringLiteral("dsh");
            case BackendPi:    return QStringLiteral("pi");
            case BackendCodex: return QStringLiteral("codex");
            case BackendHermes: return QStringLiteral("hermes");
            default:           return QStringLiteral("claude");
        }
    }
    // 2026.09.20 任意后端名→显示名（粘性提示/Tab tooltip 用）。
    static QString DisplayNameOf(const QString& a_sName) {
        if (a_sName == QStringLiteral("dsh"))    return QStringLiteral("DSH");
        if (a_sName == QStringLiteral("pi"))     return QStringLiteral("PIAgent");
        if (a_sName == QStringLiteral("codex"))  return QStringLiteral("CodexHarness");
        if (a_sName == QStringLiteral("hermes")) return QStringLiteral("Hermes");
        return QStringLiteral("Claude");
    }
    // 后端显示名（UI 展示用：助手对话块说话人标签等，随当前选择的后端走）。
    static QString BackendDisplayName() {
        switch (s_eBackend) {
            case BackendDsh:   return QStringLiteral("DSH");
            case BackendPi:    return QStringLiteral("PIAgent");
            case BackendCodex: return QStringLiteral("CodexHarness");
            case BackendHermes: return QStringLiteral("Hermes");
            default:           return QStringLiteral("Claude");
        }
    }

    static CAgentRunnerBase* Create(QObject* a_pParent = nullptr) {
        // 按全局单选后端分派。DSH/Pi 路径未配置时不在此拦截——StartStage 的前置校验
        // 会带具体缺失路径报失败（工厂层拦会把配置错误变成静默回退，难排查）。
        switch (s_eBackend) {
            case BackendDsh:   return new CDeepSeekRunner(a_pParent);
            case BackendPi:    return new CPiAgentRunner(a_pParent);
            case BackendCodex: return new CCodexRunner(a_pParent);
            case BackendHermes: return new CHermesRunner(a_pParent);
            default:           return new CClaudeRunner(a_pParent);
        }
    }
    // 2026.09.20 助手粘性后端（v2.4.193）：按后端名分派（忽略全局单选——会话绑定
    //   的后端与全局不同时，助手继续用自己的 Harness）。未知名回退当前全局。
    static CAgentRunnerBase* CreateFor(const QString& a_sName, QObject* a_pParent = nullptr) {
        if (a_sName == QStringLiteral("dsh"))    return new CDeepSeekRunner(a_pParent);
        if (a_sName == QStringLiteral("pi"))     return new CPiAgentRunner(a_pParent);
        if (a_sName == QStringLiteral("codex"))  return new CCodexRunner(a_pParent);
        if (a_sName == QStringLiteral("hermes")) return new CHermesRunner(a_pParent);
        if (a_sName == QStringLiteral("claude")) return new CClaudeRunner(a_pParent);
        return Create(a_pParent);
    }

private:
    inline static EBackend s_eBackend = BackendClaude;   // 全局单选后端（默认 claude）
};
