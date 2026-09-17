// CStageBar：任务阶段横向图标条（自绘步骤条，见头文件说明）。
#include "CStageBar.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetrics>

namespace {
// 状态色（与旧阶段表五行着色口径对应；图标态适配白底）。
const QColor kClrDone(QStringLiteral("#16a34a"));
const QColor kClrRun(QStringLiteral("#2563eb"));
const QColor kClrFail(QStringLiteral("#dc2626"));
const QColor kClrIncomp(QStringLiteral("#d97706"));
const QColor kClrPend(QStringLiteral("#c4c9d0"));
const QColor kClrLine(QStringLiteral("#e2e5ea"));   // 未推进段连接线
const QColor kClrText(QStringLiteral("#4b5563"));   // 待执行名字（灰）
constexpr int k_iCircleR = 10;      // 状态圆半径
constexpr int k_iCircleY = 20;      // 圆心 y（固定高 58 内：圆 20 + 名字 ~16 + 余量）
constexpr int k_iNameY   = 38;      // 名字区顶
constexpr int k_iNamePt  = 9;       // 名字号（小字，横向条专用；控件统一 11pt 不适用此密度）
}   // namespace

CStageBar::CStageBar(QWidget* a_pParent)
    : QWidget(a_pParent) {
    setMouseTracking(true);   // hover 高亮/tooltip 需要 move 追踪
    setMinimumHeight(58);
}

void CStageBar::SetStages(const QVector<SNode>& a_aNodes) {
    m_aNodes = a_aNodes;
    m_iHover = -1;
    update();
}

QRect CStageBar::NodeArea(int a_i) const {
    // 等分整条宽度（无边距；连接线贴圆边）。
    if (m_aNodes.isEmpty()) {
        return QRect();
    }
    const int iW = width() / m_aNodes.size();
    return QRect(a_i * iW, 0, iW, height());
}

int CStageBar::StageAt(const QPoint& a_stPt) const {
    for (int i = 0; i < m_aNodes.size(); ++i) {
        if (NodeArea(i).contains(a_stPt)) {
            return m_aNodes.at(i).iStage;
        }
    }
    return -1;
}

QString CStageBar::StateText(EState a_eState) const {
    switch (a_eState) {
        case EState::Done:      return QStringLiteral("已完成");
        case EState::Running:   return QStringLiteral("进行中");
        case EState::Failed:    return QStringLiteral("失败");
        case EState::Incomplete:return QStringLiteral("未完成");
        case EState::Pending:
        default:                return QStringLiteral("待执行");
    }
}

void CStageBar::paintEvent(QPaintEvent* a_pEvent) {
    Q_UNUSED(a_pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int iN = m_aNodes.size();
    if (iN <= 0) {
        return;
    }

    // hover 节点浅底（圆角矩形占满该节点区，提示可交互）。
    if (m_iHover >= 0 && m_iHover < iN) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 18));
        p.drawRoundedRect(NodeArea(m_iHover).adjusted(2, 2, -2, -2), 6, 6);
    }

    QFont fNm = font();
    fNm.setPointSize(k_iNamePt);
    const QFontMetrics fmNm(fNm);

    for (int i = 0; i < iN; ++i) {
        const SNode& stN = m_aNodes.at(i);
        const QRect rcArea = NodeArea(i);
        const QPoint ptC(rcArea.center().x(), k_iCircleY);

        // —— 连接线（画在左侧节点与本节点圆之间；颜色跟随左侧节点状态）——
        if (i > 0) {
            const EState ePrev = m_aNodes.at(i - 1).eState;
            QColor clrLine = kClrLine;
            if (ePrev == EState::Done) {
                clrLine = kClrDone;
            } else if (ePrev == EState::Running) {
                clrLine = kClrRun;
            }
            const QPoint ptPrevC(NodeArea(i - 1).center().x(), k_iCircleY);
            p.setPen(QPen(clrLine, 2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(ptPrevC + QPoint(k_iCircleR + 2, 0),
                       ptC - QPoint(k_iCircleR + 2, 0));
        }

        // —— 状态圆与符号 ——
        QColor clrCirc = kClrPend;
        switch (stN.eState) {
            case EState::Done:       clrCirc = kClrDone;   break;
            case EState::Running:    clrCirc = kClrRun;    break;
            case EState::Failed:     clrCirc = kClrFail;   break;
            case EState::Incomplete: clrCirc = kClrIncomp; break;
            case EState::Pending:
            default:                 clrCirc = kClrPend;   break;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(clrCirc);
        p.drawEllipse(ptC, k_iCircleR, k_iCircleR);

        const QRectF rcCirc(ptC.x() - k_iCircleR, ptC.y() - k_iCircleR,
                            k_iCircleR * 2, k_iCircleR * 2);
        const QPen penSym(QColor(Qt::white), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        switch (stN.eState) {
            case EState::Done: {   // 白勾（两段线）
                p.setPen(penSym);
                p.drawLine(ptC + QPoint(-4, 0), ptC + QPoint(-1, 4));
                p.drawLine(ptC + QPoint(-1, 4), ptC + QPoint(5, -4));
                break;
            }
            case EState::Running: {   // 白三角 ▶
                QPolygon aTri;
                aTri << ptC + QPoint(-3, -5) << ptC + QPoint(5, 0) << ptC + QPoint(-3, 5);
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawPolygon(aTri);
                break;
            }
            case EState::Failed: {   // 白叉 ✖
                p.setPen(penSym);
                p.drawLine(ptC + QPoint(-4, -4), ptC + QPoint(4, 4));
                p.drawLine(ptC + QPoint(-4, 4), ptC + QPoint(4, -4));
                break;
            }
            case EState::Incomplete: {   // 白双竖条 ⏸
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawRect(ptC.x() - 4, ptC.y() - 4, 3, 8);
                p.drawRect(ptC.x() + 1, ptC.y() - 4, 3, 8);
                break;
            }
            case EState::Pending:
            default: {   // 序号（灰字——圆本身浅灰，深灰字可读）
                p.setPen(QColor(QStringLiteral("#6b7280")));
                QFont fNum = font();
                fNum.setPointSize(k_iNamePt);
                fNum.setBold(true);
                p.setFont(fNum);
                p.drawText(rcCirc, Qt::AlignCenter, QString::number(i + 1));
                break;
            }
        }

        // —— 名字（状态同色文字；超宽 elide 到 4 字，全名在 tooltip）——
        QColor clrName = kClrText;
        switch (stN.eState) {
            case EState::Done:       clrName = kClrDone;   break;
            case EState::Running:    clrName = kClrRun;    break;
            case EState::Failed:     clrName = kClrFail;   break;
            case EState::Incomplete: clrName = kClrIncomp; break;
            case EState::Pending:
            default:                 clrName = kClrText;   break;
        }
        p.setFont(fNm);
        p.setPen(clrName);
        QString sName = stN.sName.left(4);
        if (stN.sName.size() > 4) {
            sName = fmNm.elidedText(sName + QStringLiteral("…"),
                                    Qt::ElideRight, rcArea.width() - 6);
        }
        p.drawText(QRect(rcArea.left(), k_iNameY, rcArea.width(),
                         height() - k_iNameY), Qt::AlignHCenter | Qt::AlignTop, sName);
    }
}

void CStageBar::mouseMoveEvent(QMouseEvent* a_pEvent) {
    // hover：换节点才重绘 + tooltip（全名 + 状态）。
    int iHover = -1;
    for (int i = 0; i < m_aNodes.size(); ++i) {
        if (NodeArea(i).contains(a_pEvent->pos())) {
            iHover = i;
            break;
        }
    }
    if (iHover != m_iHover) {
        m_iHover = iHover;
        update();
    }
    if (iHover >= 0) {
        const SNode& stN = m_aNodes.at(iHover);
        QToolTip::showText(a_pEvent->globalPosition().toPoint(),
                           QStringLiteral("%1（%2）").arg(stN.sName, StateText(stN.eState)),
                           this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(a_pEvent);
}

void CStageBar::leaveEvent(QEvent* a_pEvent) {
    if (m_iHover != -1) {
        m_iHover = -1;
        update();
    }
    QWidget::leaveEvent(a_pEvent);
}
