#include "CAgentRunnerBase.h"

// 构造/析构放在 .cpp：锚定基类 vtable（纯虚接口 + QObject 混入，避免每个包含 TU 都内联展开）。
CAgentRunnerBase::CAgentRunnerBase(QObject* a_pParent)
    : QObject(a_pParent) {
}

CAgentRunnerBase::~CAgentRunnerBase() = default;
