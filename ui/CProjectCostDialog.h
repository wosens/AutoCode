#pragma once

// ---------------------------------------------------------------------------
// CProjectCostDialog —— 工程总消耗统计窗口（2026-11-05 工程菜单
// 「统计工程总的消耗...」）。展示 CProjectUsage::Aggregate 的聚合结果：
// 费用($)、请求数、输入/输出/合计 Token，任务侧与助手侧分列对账 + 总计行。
// 2026-11-06 布局持久化（用户需求）：窗口几何（[Layout] CostDlgGeo，框架原点
//   x/y + 客户区尺寸，口径同设置对话框零漂移修复）+ 表列宽
//   （[Layout] CostDlgCols，末列 stretch 不存）——done() 统一落盘（确认/取消/X
//   都经此），恢复在构造期，出屏/坏档自弃用缺省。
// 数据打开时现算（短连接不锁库，多实例同开工程也安全）。
// ---------------------------------------------------------------------------

#include <QDialog>

#include "db/CProjectUsage.h"

class QTableWidget;
class QLabel;
class CProjectManager;

class CProjectCostDialog : public QDialog {
    Q_OBJECT

public:
    // a_sProjectDir 仅用于标题展示；数据由调用方先 Aggregate 传入（对话框
    // 保持纯展示，聚合逻辑可在离线单测里覆盖）。a_pPm 可空（无持久化，纯展示）。
    explicit CProjectCostDialog(const SProjectUsage& a_stUsage,
                                const QString& a_sProjectDir,
                                CProjectManager* a_pPm = nullptr,
                                QWidget* a_pParent = nullptr);
    ~CProjectCostDialog() override = default;

protected:
    // accept / reject / 标题栏 X 都经 done()：几何与列宽在此统一落盘。
    void done(int a_r) override;

private:
    void SetupUi();
    void RestoreLayout();   // 几何 + 列宽（出屏/坏档自弃，用 SetupUi 缺省）

    SProjectUsage   m_stUsage;
    QString         m_sProjectDir;
    CProjectManager* m_pPm = nullptr;
    QTableWidget*   m_pTable = nullptr;
    QLabel*         m_pLblInfo = nullptr;
};
