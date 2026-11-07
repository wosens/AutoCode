// CProjectCostDialog.cpp —— 工程总消耗统计窗口（2026-11-05）。
#include "CProjectCostDialog.h"

#include "CProjectManager.h"

#include <QFont>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScreen>
#include <QTableWidget>
#include <QVBoxLayout>

// K/M/B 单位化显示（2026-11-06 用户需求）：
//   <1000 原样（整数列无小数、费用列保 4 位小数）；≥1000 逐级 ÷1000 附 K/M/B，
//   保留 1 位小数（34600 → 34.6K；2300000 → 2.3M）。精确值放 tooltip（悬停可对账/复制）。
static QString kFmtKmbInt(qint64 a_iVal) {
    double dV = static_cast<double>(a_iVal);
    int iSuf = 0;
    while (qAbs(dV) >= 1000.0 && iSuf < 3) {
        dV /= 1000.0;
        ++iSuf;
    }
    if (iSuf == 0) {
        return QString::number(a_iVal);
    }
    static const char* kSzSuf[4] = { "", "K", "M", "B" };
    return QString::number(dV, 'f', 1) + QLatin1String(kSzSuf[iSuf]);
}

// 费用同理：小额保 4 位（$0.9857），≥1000 走 K/M/B（1 位小数）。
static QString kFmtKmbCost(double a_dCost) {
    if (a_dCost < 1000.0) {
        return QString::number(a_dCost, 'f', 4);
    }
    double dV = a_dCost;
    int iSuf = 0;
    while (dV >= 1000.0 && iSuf < 3) {
        dV /= 1000.0;
        ++iSuf;
    }
    static const char* kSzSuf[4] = { "", "K", "M", "B" };
    return QString::number(dV, 'f', 1) + QLatin1String(kSzSuf[iSuf]);
}

CProjectCostDialog::CProjectCostDialog(const SProjectUsage& a_stUsage,
                                       const QString& a_sProjectDir,
                                       CProjectManager* a_pPm,
                                       QWidget* a_pParent)
    : QDialog(a_pParent), m_stUsage(a_stUsage), m_sProjectDir(a_sProjectDir)
    , m_pPm(a_pPm) {
    SetupUi();
    RestoreLayout();
}

void CProjectCostDialog::done(int a_r) {
    // 几何 + 列宽统一落盘（accept / reject / X 都经 done；失败静默，下次用缺省）。
    if (m_pPm != nullptr && m_pTable != nullptr) {
        // 框架原点 x/y + 客户区尺寸（口径同设置对话框 2026.09.26 零漂移修复：
        //   构造期 move() 按含框架语义记账，两侧同坐标系往返零漂移）。
        m_pPm->SaveLayoutValue(QStringLiteral("CostDlgGeo"),
            QStringLiteral("%1,%2,%3,%4")
                .arg(frameGeometry().x()).arg(frameGeometry().y())
                .arg(width()).arg(height()));
        // 前 5 列宽逗号串；末列「Token 总数」stretchLastSection 恒填满不存。
        QString sCols;
        for (int iCol = 0; iCol < m_pTable->columnCount() - 1; ++iCol) {
            if (!sCols.isEmpty()) {
                sCols += QLatin1Char(',');
            }
            sCols += QString::number(m_pTable->columnWidth(iCol));
        }
        m_pPm->SaveLayoutValue(QStringLiteral("CostDlgCols"), sCols);
    }
    QDialog::done(a_r);
}

void CProjectCostDialog::RestoreLayout() {
    if (m_pPm == nullptr || m_pTable == nullptr) {
        return;
    }
    // ① 几何：x,y,w,h；出屏（所有屏工作区都不相交）或尺寸过小/坏档 → 自弃用缺省
    const QString sGeo = m_pPm->LoadLayoutValue(QStringLiteral("CostDlgGeo"));
    const QStringList aGeo = sGeo.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (aGeo.size() == 4) {
        const QRect rc(aGeo.at(0).toInt(), aGeo.at(1).toInt(),
                       aGeo.at(2).toInt(), aGeo.at(3).toInt());
        bool bOnScreen = false;
        const QList<QScreen*> apScreens = QGuiApplication::screens();
        for (QScreen* pSc : apScreens) {
            if (pSc != nullptr && pSc->availableGeometry().intersects(rc)) {
                bOnScreen = true;
                break;
            }
        }
        if (bOnScreen && rc.width() >= 400 && rc.height() >= 220) {
            move(rc.topLeft());
            resize(rc.size());
        }
    }
    // ② 列宽：逐列限幅 24..2000（口径同设置/回退表），末列 stretch 不恢复
    const QString sCols = m_pPm->LoadLayoutValue(QStringLiteral("CostDlgCols"));
    const QStringList aVals = sCols.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (int iCol = 0; iCol < aVals.size() && iCol < m_pTable->columnCount() - 1; ++iCol) {
        m_pTable->setColumnWidth(iCol, qBound(24, aVals.at(iCol).toInt(), 2000));
    }
}

void CProjectCostDialog::SetupUi() {
    setWindowTitle(QStringLiteral("工程总消耗统计"));
    QFont stFont = font();
    stFont.setPointSize(11);   // 界面缺省 11pt（2026 规则）
    setFont(stFont);

    // —— 明细表：3 行（任务/助手/总计）× 6 列 ——
    m_pTable = new QTableWidget(3, 6, this);
    m_pTable->setFont(stFont);
    m_pTable->verticalHeader()->setDefaultSectionSize(28);
    m_pTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("来源") << QStringLiteral("费用($)")
        << QStringLiteral("请求数") << QStringLiteral("输入 Token")
        << QStringLiteral("输出 Token") << QStringLiteral("Token 总数"));
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    const int aiColWidth[6] = { 150, 110, 90, 130, 130, 130 };
    for (int iCol = 0; iCol < 6; ++iCol) {
        m_pTable->setColumnWidth(iCol, aiColWidth[iCol]);
    }

    struct SRow {
        QString sName;
        double dCost;
        int iReqs;
        qint64 iIn, iOut;
    };
    const SRow aRows[3] = {
        { QStringLiteral("任务（%1 个，history.db）").arg(m_stUsage.m_nTasks),
          m_stUsage.m_dTaskCost, m_stUsage.m_iTaskReqs,
          m_stUsage.m_iTaskIn, m_stUsage.m_iTaskOut },
        { QStringLiteral("助手（%1 个，assistant.json 档）").arg(m_stUsage.m_nAssts),
          m_stUsage.m_dAsstCost, m_stUsage.m_iAsstReqs,
          m_stUsage.m_iAsstIn, m_stUsage.m_iAsstOut },
        { QStringLiteral("工程总计"), m_stUsage.TotalCost(), m_stUsage.TotalReqs(),
          m_stUsage.TotalIn(), m_stUsage.TotalOut() },
    };
    for (int iRow = 0; iRow < 3; ++iRow) {
        const SRow& r = aRows[iRow];
        const QString asCells[6] = {
            r.sName, kFmtKmbCost(r.dCost), kFmtKmbInt(r.iReqs),
            kFmtKmbInt(r.iIn), kFmtKmbInt(r.iOut), kFmtKmbInt(r.iIn + r.iOut),
        };
        // 精确值 tooltip（K/M/B 缩写不可直接对账，悬停给裸数值）
        const QString asExact[6] = {
            QString(), QString::number(r.dCost, 'f', 6), QString::number(r.iReqs),
            QString::number(r.iIn), QString::number(r.iOut),
            QString::number(r.iIn + r.iOut),
        };
        for (int iCol = 0; iCol < 6; ++iCol) {
            QTableWidgetItem* pItem = new QTableWidgetItem(asCells[iCol]);
            pItem->setToolTip(asExact[iCol]);
            pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
            if (iRow == 2) {
                QFont fBold = stFont;
                fBold.setWeight(QFont::DemiBold);
                pItem->setFont(fBold);   // 总计行加重视觉锚点
            }
            m_pTable->setItem(iRow, iCol, pItem);
        }
    }

    m_pLblInfo = new QLabel(this);
    m_pLblInfo->setWordWrap(true);
    m_pLblInfo->setText(QStringLiteral(
        "统计口径：任务侧取 <工程>/.autodev/history.db 全部任务累计；助手侧取工程级与"
        "各任务 assistant.json 档 usage 累计（2026-09-05 起助手消耗只归助手自身、"
        "不并入任务统计，两源相加即工程总量、无重复计数）。打开窗口时现算，"
        "不含正在运行中尚未落盘的部分。数值≥1000 按 K/M/B 缩写显示（悬停单元格可看精确值）。"));

    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), this);
    auto* pBtnRow = new QHBoxLayout();
    pBtnRow->addStretch(1);
    pBtnRow->addWidget(pBtnClose);
    connect(pBtnClose, &QPushButton::clicked, this, &QDialog::accept);

    auto* pMain = new QVBoxLayout(this);
    pMain->addWidget(m_pTable);
    pMain->addWidget(m_pLblInfo);
    pMain->addLayout(pBtnRow);

    resize(780, 300);
}
