#include "CChatModel.h"

CChatModel::CChatModel(QObject* a_pParent)
    : QAbstractListModel(a_pParent)
{
}

SDisplayItem& CChatModel::appendItem(CMessageBlock::EKind a_eKind)
{
    beginInsertRows(QModelIndex(), m_aItems.size(), m_aItems.size());
    SDisplayItem it;
    it.eKind = a_eKind;
    it.iBegin = m_aIdx2Item.size();          // 本块在数据序列中的下标
    m_aItems.append(it);
    endInsertRows();
    return m_aItems.last();
}

void CChatModel::emitChanged(int a_iIdx)
{
    // 内容变化（组内追加条目/结果回填/错误展开）：空 roles 列表 = 全角色失效，
    // 视图重取条目并重算高度（sizeHint 经 delegate 缓存惰性重估）
    const QModelIndex idx = index(a_iIdx, 0);
    emit dataChanged(idx, idx);
}

void CChatModel::ClearDisplay()
{
    beginResetModel();
    m_aItems.clear();
    m_aIdx2Item.clear();
    m_iSys = m_iTool = -1;
    endResetModel();
}

void CChatModel::RebuildFrom(const QList<SBlockData>& a_aBlocks)
{
    beginResetModel();
    m_aItems.clear();
    m_aIdx2Item.clear();
    m_aIdx2Item.reserve(a_aBlocks.size());
    m_iSys = m_iTool = -1;
    endResetModel();
    for (const SBlockData& st : a_aBlocks) {
        AppendBlock(st);   // 复用增量状态机（beginResetModel 已复位开卡态）
    }
}

void CChatModel::AppendBlock(const SBlockData& a_stData)
{
    // 原始块 → 显示条目（合并判定与 Legacy TryMergeWidget 逐条对齐，改口径须双改+双测）
    switch (a_stData.eKind) {
    case CMessageBlock::ToolUse: {
        // 连续工具（中间只夹 Assistant 文本不打断，2026.10.15 DSH 思维链穿插口径）
        // 并入同一张合并卡；viewId 不一致（评审并行视角）另开新卡。
        if (m_iTool >= 0 && m_aItems[m_iTool].sViewId == a_stData.sViewId) {
            SDisplayItem& it = m_aItems[m_iTool];
            SToolEntry e;
            e.sTool = a_stData.sTool;
            e.sInput = a_stData.sInput;
            e.sResult = a_stData.sResult;
            e.bError = !e.sResult.isEmpty() && CMessageBlock::IsErrorResult(e.sResult);
            e.iData = m_aIdx2Item.size();
            it.aTools.append(e);
            if (e.bError) {
                it.bHadError = true;
            }
            m_aIdx2Item.append(m_iTool);
            emitChanged(m_iTool);
        } else {
            SDisplayItem& it = appendItem(CMessageBlock::ToolUse);
            it.sViewId = a_stData.sViewId;
            SToolEntry e;
            e.sTool = a_stData.sTool;
            e.sInput = a_stData.sInput;
            e.sResult = a_stData.sResult;
            e.bError = !e.sResult.isEmpty() && CMessageBlock::IsErrorResult(e.sResult);
            e.iData = it.iBegin;
            it.aTools.append(e);
            if (e.bError) {
                it.bHadError = true;
            }
            m_iTool = m_aItems.size() - 1;
            m_aIdx2Item.append(m_iTool);
        }
        m_iSys = -1;   // 其它开卡态关闭（工具卡保持开：后续工具继续并入）
        break;
    }
    case CMessageBlock::System: {
        // v2.0.113 裸 JSON 清洗：progress 心跳等空串行整体丢弃（不建条目不断系统组
        //   链——Legacy 里 progress 行归并进前组卡，同口径）；但该 System 数据块
        //   仍打断工具合并链（Legacy TryMergeWidget 回溯遇 System 即分卡）。
        const QString sPretty = CMessageBlock::PrettyLegacyJsonLine(a_stData.sText);
        if (sPretty.isEmpty()) {
            m_aIdx2Item.append(-1);   // 丢弃行：映射记 -1（ItemAtDataIdx 兜底回退）
            m_iTool = -1;
            break;
        }
        if (m_iSys >= 0) {
            SDisplayItem& it = m_aItems[m_iSys];
            it.aSysLines.append(sPretty);
            m_aIdx2Item.append(m_iSys);
            emitChanged(m_iSys);
        } else {
            SDisplayItem& it = appendItem(CMessageBlock::System);
            it.aSysLines.append(sPretty);
            m_iSys = m_aItems.size() - 1;
            m_aIdx2Item.append(m_iSys);
        }
        m_iTool = -1;   // 系统行打断工具卡链（与 Legacy“系统行打断”同口径）
        break;
    }
    case CMessageBlock::Assistant: {
        // 1:1；不打断工具合并卡（DSH 思维链穿插口径），但打断系统组。
        SDisplayItem& it = appendItem(CMessageBlock::Assistant);
        it.sText = a_stData.sText;
        it.sBackend = a_stData.sBackend;
        m_aIdx2Item.append(m_aItems.size() - 1);
        m_iSys = -1;
        break;
    }
    case CMessageBlock::Event: {
        SDisplayItem& it = appendItem(CMessageBlock::Event);
        it.sEventKind = a_stData.sEventKind;
        it.sText = a_stData.sText;
        m_aIdx2Item.append(m_aItems.size() - 1);
        m_iTool = m_iSys = -1;
        break;
    }
    case CMessageBlock::Error:
    default: {
        SDisplayItem& it = appendItem(CMessageBlock::Error);
        it.sText = a_stData.sText;
        m_aIdx2Item.append(m_aItems.size() - 1);
        m_iTool = m_iSys = -1;
        break;
    }
    }
}

bool CChatModel::BackfillResult(int a_iDataIdx, const QString& a_sResult)
{
    if (a_iDataIdx < 0 || a_iDataIdx >= m_aIdx2Item.size()) {
        return false;
    }
    const int iItem = m_aIdx2Item.at(a_iDataIdx);
    if (iItem < 0 || iItem >= m_aItems.size()) {
        return false;
    }
    SDisplayItem& it = m_aItems[iItem];
    if (it.eKind != CMessageBlock::ToolUse) {
        return false;
    }
    for (SToolEntry& e : it.aTools) {
        if (e.iData == a_iDataIdx) {
            e.sResult = a_sResult;
            e.bError = CMessageBlock::IsErrorResult(a_sResult);
            if (e.bError) {
                it.bHadError = true;
                // 实时错误单条卡自动展开（对齐 Legacy SetResult：仅 aTools.size()<=1
                //   且当前折叠时展开；×N 合并卡/回放路径只标红不摊开）
                if (it.bCollapsed && it.aTools.size() <= 1) {
                    it.bCollapsed = false;
                }
            }
            emitChanged(iItem);
            return true;
        }
    }
    return false;
}

int CChatModel::ItemAtDataIdx(int a_iDataIdx) const
{
    if (a_iDataIdx < 0 || a_iDataIdx >= m_aIdx2Item.size()) {
        return -1;
    }
    int iItem = m_aIdx2Item.at(a_iDataIdx);
    if (iItem >= 0) {
        return iItem;
    }
    // 被丢弃行（progress 心跳等）：向前找最近的非丢弃映射（该行视觉上紧贴前条目）
    for (int i = a_iDataIdx - 1; i >= 0; --i) {
        iItem = m_aIdx2Item.at(i);
        if (iItem >= 0) {
            return iItem;
        }
    }
    return -1;
}

const SDisplayItem* CChatModel::PeekItem(int a_iIdx) const
{
    return (a_iIdx >= 0 && a_iIdx < m_aItems.size()) ? &m_aItems.at(a_iIdx) : nullptr;
}

SDisplayItem* CChatModel::itemAt(int a_iIdx)
{
    return (a_iIdx >= 0 && a_iIdx < m_aItems.size()) ? &m_aItems[a_iIdx] : nullptr;
}

void CChatModel::TouchSizeHint(int a_iIdx)
{
    if (a_iIdx < 0 || a_iIdx >= m_aItems.size())
        return;
    const QModelIndex idx = index(a_iIdx, 0);
    emit dataChanged(idx, idx, QList<int>{Qt::SizeHintRole});
}

int CChatModel::rowCount(const QModelIndex& a_parent) const
{
    return a_parent.isValid() ? 0 : m_aItems.size();
}

QVariant CChatModel::data(const QModelIndex& a_index, int a_iRole) const
{
    const SDisplayItem* pIt = PeekItem(a_index.row());
    if (pIt == nullptr)
        return QVariant();
    switch (a_iRole) {
    case KindRole:        return static_cast<int>(pIt->eKind);
    case TextRole:        return pIt->sText;
    case SpeakerRole:     return pIt->sBackend;
    case ToolEntriesRole: {
        QVariantList aOut;
        for (const SToolEntry& e : pIt->aTools) {
            QVariantMap m;
            m[QStringLiteral("tool")] = e.sTool;
            m[QStringLiteral("input")] = e.sInput;
            m[QStringLiteral("result")] = e.sResult;
            m[QStringLiteral("error")] = e.bError;
            aOut.append(m);
        }
        return aOut;
    }
    case EventKindRole:   return pIt->sEventKind;
    case CollapsedRole:   return pIt->eKind == CMessageBlock::System ? pIt->bSysCollapsed
                                                                     : pIt->bCollapsed;
    case HighlightRole:   return pIt->bHighlight;
    case BeginRole:       return pIt->iBegin;
    default:              return QVariant();
    }
}

bool CChatModel::setData(const QModelIndex& a_index, const QVariant& a_value, int a_iRole)
{
    SDisplayItem* pIt = itemAt(a_index.row());     // 私有可变重载（免 const_cast 反模式）
    if (pIt == nullptr)
        return false;
    if (a_iRole == CollapsedRole) {
        if (pIt->eKind == CMessageBlock::System)
            pIt->bSysCollapsed = a_value.toBool();
        else
            pIt->bCollapsed = a_value.toBool();
        emit dataChanged(a_index, a_index, QList<int>{CollapsedRole, Qt::SizeHintRole});
        return true;                       // 折叠切换 → 高度失效 + 重绘
    }
    if (a_iRole == HighlightRole) {
        pIt->bHighlight = a_value.toBool();
        emit dataChanged(a_index, a_index, QList<int>{HighlightRole});
        return true;
    }
    return QAbstractListModel::setData(a_index, a_value, a_iRole);
}
