#pragma once

// ---------------------------------------------------------------------------
// CChatDelegate —— 对话流五块型纯 painter 自绘 delegate（2026-10-17 自绘三期，
// doc/对话流视图自绘改造方案.md §7；参照 HarnessAgent v1.9.44 ChatDelegate 移植
// 并适配本工程块型）。按 CMessageBlock::EKind 分派 paint / sizeHint / editorEvent
//（命中=纯几何，零子 widget）：
//   Assistant 左橙竖条 + speaker 行（⏺ 产出后端）+ QTextDocument 同源 Markdown 直绘
//   （doc 经 CChatDocPool 池化）；ToolUse 折叠合并卡（ElideMiddle 单行截断、
//   “ … ×N” 后缀、错误卡头换肤、展开区入参/结果等宽块 + Edit 红绿 diff）；Event
//   彩色徽章行（user=顶部 3px 分隔线 + 头像 + 蓝“你”徽章）；System 灰斜体折叠组；
//   Error 红字换行。
// 估高公式先占位（doc §7.4），doc 材化后 OnDocReady 逐条校正实高并经
// fnHeightCorrected 通知视图做锚定补偿（防滚动跳变）。
// 视觉常量对齐 Legacy CMessageBlock QSS 实测值（灰度期双引擎 ≤5% 视觉差，方案 G4）；
// 不创建子 widget、不识 CChatListView 具体类型（分层逃生舱）。无 Q_OBJECT。
// ---------------------------------------------------------------------------

#include <QAbstractItemDelegate>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <functional>

#include "CChatModel.h"   // SDisplayItem / SToolEntry / ERole

class CChatDocPool;
class QTextDocument;
class QEvent;
class QPainter;
class QStyleOptionViewItem;
class QModelIndex;
class QAbstractItemModel;

// Edit/MultiEdit 红绿 diff 行（自 CMessageBlock::RebuildToolContent 的
// 「--- 旧 / - 行 / --- 新 / + 行」序列抽出；前景 #b91c1c（旧）/ #15803d（新））
struct SDiffLine {
    QString sText;
    bool bOld = false;    // true=旧（红）/ false=新（绿）；空行两色皆可
};

// 等宽块行（入参 diff/JSON、结果文本；行长上限 12 行之外画「……（共 N 行）」提示）
struct SMonoLine {
    QString sText;
    QColor col;                                // 无效色 = 缺省黑字
};

class CChatDelegate : public QAbstractItemDelegate
{
public:
    // a_oFont = 宿主视图字体（speaker/徽章行度量基准；等宽块固定 Consolas 11pt）
    explicit CChatDelegate(CChatDocPool* a_pPool, const QFont& a_oFont,
                           QObject* a_pParent = nullptr);

    void paint(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
               const QModelIndex& a_oIndex) const override;
    QSize sizeHint(const QStyleOptionViewItem& a_oOption,
                   const QModelIndex& a_oIndex) const override;
    bool editorEvent(QEvent* a_pEvent, QAbstractItemModel* a_pModel,
                     const QStyleOptionViewItem& a_oOption,
                     const QModelIndex& a_oIndex) override;

    void OnDocReady(int a_iItem, QTextDocument* a_pDoc, const SDisplayItem* a_pIt);
    // 池回调：实高=doc 高+speaker 行+边距→更新缓存（指纹取 a_pIt 权威值）→fnHeightCorrected
    void InvalidateWidth(int a_iNewWidth);                 // 高度缓存全失效（惰性重估）+ 转发池 OnWidthChanged
    void SetFont(const QFont& a_oFont);                    // 视图字体变更（11pt 回填）：缓存全失效
    std::function<void(int a_iItem, int a_iDeltaY)> fnHeightCorrected;   // 视图接：锚定补偿 + model->TouchSizeHint

    int EstimateHeight(const SDisplayItem& a_it, int a_iWidth) const;    // 纯算术（测试可直接断言）
    int CachedHeight(int a_iRow) const;                   // 测试断言（resize 失效验证）；0=未缓存
    QString ToolTipAt(const QModelIndex& a_oIndex, const QPoint& a_oPos) const;  // 视图 viewportEvent(ToolTip) 转发

private:
    const SDisplayItem* itemOf(const QModelIndex& a_oIndex) const;
    void paintAssistant(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                        const SDisplayItem& a_it, int a_iRow) const;
    void paintToolUse(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                      const SDisplayItem& a_it, int a_iRow) const;
    void paintEventBadge(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                         const SDisplayItem& a_it) const;   //（命名避开 paintEvent 语义混淆）
    void paintSystem(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                     const SDisplayItem& a_it, int a_iRow) const;
    void paintError(QPainter* a_pPainter, const QStyleOptionViewItem& a_oOption,
                    const SDisplayItem& a_it) const;

    // 等宽块行列表缓存（paint 热路径免逐帧重解析 JSON；键=行号+内容指纹）
    struct SMonoCache {
        quint64 uFp = 0;                            // 内容指纹（与高度缓存同款失效口径）
        int iCount = -1;                            // aTools 条数（结构校验）
        QList<QList<SMonoLine>> aIn, aOut;          // 逐条目的入参/结果行
    };
    SMonoCache monoCacheFor(const SDisplayItem& a_it, int a_iRow) const;

    // 高度缓存值：估高/实高 + 内容指纹（sizeHint 比对指纹，指纹不一致（流式增长/
    // 折叠翻转/错误自动展开/事件 kind 变化）即重估——否则折叠与增长命中旧值，布局
    // 压叠/冻结；指纹不含高亮态，避免跳转高亮抖动已校正实高）
    struct SHeightCache {
        int iH = 0;
        quint64 uFp = 0;
    };

    CChatDocPool* m_pPool;                  // 不持有（视图持有，生命周期一致）
    QFont m_oFont;                          // 常规字体（视图字体）
    QFont m_oFontMono;                      // 等宽字体（Consolas 11pt）
    mutable QHash<int, SHeightCache> m_aHeights;      // 行 → 高度缓存（指纹自校验）
    mutable QHash<int, SMonoCache> m_aMono;           // 行 → 等宽块行缓存（paint 热路径）
    mutable QHash<int, QRect> m_aHeaderRect; // 行 → 折叠头命中区（paint 时记录）
    int m_iWidth = 0;                       // 当前估宽（sizeHint 基准；视图 resize 注入）
};
