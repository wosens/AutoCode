#pragma once

#include <QWidget>
#include <QVector>
#include <QString>
class QEvent;

// =============================================================================
// CStageBar：任务阶段横向图标条（2026.09.04 替代 QTreeWidget 阶段进度表）。
//
// 旧阶段表每阶段一行（纵向占 120px+，还要与左栏任务列表底部对齐做高度同步）；
// 现改为一条固定高（58px）的横向步骤条：每阶段一个节点（圆形状态图标 + 下方
// 短名），节点间连接线着色指示推进进度——充分利用控件横向空间，纵向全部
// 让给日志区。
//
// 节点状态（与旧表五行口径一一对应）：
//   ✔ Done      绿 #16a34a，圆内白勾
//   ▶ Running   蓝 #2563eb，圆内白三角（当前阶段）
//   ✖ Failed    红 #dc2626，圆内白叉
//   ⏸ Incomplete 黄 #d97706，圆内白双竖条（暂停/停止后的当前阶段）
//   ○ Pending   灰 #c4c9d0，圆内序号（灰字）
// 连接线颜色跟随其左侧节点状态（Done=绿 / Running=蓝 / 其余浅灰）。
//
// 交互：hover 节点高亮 + tooltip（阶段全名 + 状态文字）；customContextMenuRequested
//   由外层接（面板「再次执行」菜单），坐标经 StageAt 反查阶段。
//
// 自绘（QPainter）：无图标资源依赖、无子控件；节点等分整条宽度，名字超宽 elide
//   （tooltip 补全名）。
// =============================================================================
class CStageBar : public QWidget {
    Q_OBJECT

public:
    enum class EState { Pending, Running, Done, Failed, Incomplete };

    struct SNode {
        int     iStage = 0;    // EStage 原值（右键「再次执行」路由用）
        QString sName;         // 阶段全名（tooltip）；绘制取前 4 字
        EState  eState = EState::Pending;
    };

    explicit CStageBar(QWidget* a_pParent = nullptr);

    // 整体重设（切任务/刷新状态）。空向量 = 清空（无任务）。
    void SetStages(const QVector<SNode>& a_aNodes);
    // 本地坐标 → 阶段原值；命中间隙/越界返回 -1（右键菜单抑制）。
    int StageAt(const QPoint& a_stPt) const;

    QSize sizeHint() const override { return QSize(420, 58); }

protected:
    void paintEvent(QPaintEvent* a_pEvent) override;
    void mouseMoveEvent(QMouseEvent* a_pEvent) override;
    void leaveEvent(QEvent* a_pEvent) override;   // Qt6 签名参数为 QEvent*

private:
    // 节点 i 的占位区（整条等分；圆心/名字区由此推导）。
    QRect NodeArea(int a_i) const;
    QString StateText(EState a_eState) const;

    QVector<SNode> m_aNodes;
    int            m_iHover = -1;   // hover 节点下标（-1 无）
};
