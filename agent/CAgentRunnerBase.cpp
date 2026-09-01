#include "CAgentRunnerBase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// 构造/析构放在 .cpp：锚定基类 vtable（纯虚接口 + QObject 混入，避免每个包含 TU 都内联展开）。
CAgentRunnerBase::CAgentRunnerBase(QObject* a_pParent)
    : QObject(a_pParent) {
}

CAgentRunnerBase::~CAgentRunnerBase() = default;

// —— 便携资产统一解析（2026.08.24）：实现说明见头文件同名声明注释 ——
QStringList CAgentRunnerBase::SharedSkillDirs() {
    QStringList aDirs;
    const QString sPortable = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/skills");
    const QString sClaude = QDir::homePath() + QStringLiteral("/.claude/skills");
    if (QFileInfo::exists(sPortable)) {
        aDirs << QDir::toNativeSeparators(sPortable);
    }
    if (QFileInfo::exists(sClaude)) {
        aDirs << QDir::toNativeSeparators(sClaude);
    }
    return aDirs;
}

QString CAgentRunnerBase::SharedGlobalContextContent() {
    const QString sPortable = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/CLAUDE.md");
    const QString sClaude = QDir::homePath() + QStringLiteral("/.claude/CLAUDE.md");
    const QStringList aCandidates = { sPortable, sClaude };
    for (const QString& sPath : aCandidates) {
        QFileInfo stInfo(sPath);
        if (!stInfo.exists() || !stInfo.isFile()) {
            continue;
        }
        // 上限保护（64KB）：超大文件视为异常，不注入。
        if (stInfo.size() > 64 * 1024) {
            continue;
        }
        QFile stFile(sPath);
        if (!stFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString sContent = QString::fromUtf8(stFile.readAll()).trimmed();
        if (!sContent.isEmpty()) {
            return sContent;
        }
    }
    return QString();
}
