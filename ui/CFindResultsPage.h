#pragma once

// ============================================================================
// CFindResultsPage —— 「查找结果」独立无模式窗口（2026-11-07 编辑菜单查找/替换；
//                      同日多轮增强：多选/右键复制/同串出现序定位）
// ----------------------------------------------------------------------------
// 独立顶层窗口（Qt::Dialog 无模式，父链=主窗口）：点行开文件时窗口仍在——列表与
// 文件预览同屏可见。单实例复用（新搜索刷同窗），点 X 只隐藏不销毁。
// 两列列表：位置（行:列）/ 文件名（相对路径）。点击行 → sigOpenHit → 预览 Tab
//   打开定位（携带命中文本+同串出现序——md/html 渲染后按第 N 次出现定位）。
//   多选（Ctrl/Shift 或框选）时不自动定位（用户需求 4）；右键菜单：复制内容
//   （相对路径:行:列）/ 复制路径（选中文件去重绝对路径）（需求 5）。
// 替换模式（查找时带替换串）在列表上方增「替换」「替换全部」两按钮：
//   · 替换：替换当前选中条目（未选中=第一条），替换后该条目出列并选中下一条；
//   · 替换全部：列表全部条目按文件分组、文件内自后向前一次写回。
//   替换用 QSaveFile 原子落盘并保持原编码/原行尾；被预览 Tab 正打开的文件经
//   sigFileRewritten 通知刷新。
// 搜索在后台 std::thread 执行（取消/join/序号防旧轮投递均自管，析构收线程）。
// ============================================================================

#include <QWidget>
#include <atomic>
#include <thread>
#include <memory>

#include "core/CFileSearcher.h"

class QTableWidget;
class QLabel;
class QPushButton;

// 一次查找任务：对话框参数 + 主窗口解析出的根/排除清单。
struct SFindTask {
    QString m_sFind;
    QString m_sReplace;
    bool m_bReplaceMode = false;
    bool m_bCase = false;
    bool m_bWord = false;
    bool m_bRegex = false;
    QStringList m_aRoots;          // 绝对根（[0]=工程根；全部范围含关联根）
    QString m_sProjectDir;         // 相对路径显示基准（工程根）
    QStringList m_aExcludeDirs;    // 排除目录名（config.ini [Find] ExcludeDirs）
    QStringList m_aLogRoots;       // 任务/助手日志根（各根的 .autodev 绝对路径；
                                   //   独立通道不受排除清单约束，与 m_aRoots 空
                                   //   布尔组合表达查找范围：仅日志=m_aRoots 空）
    QStringList m_aLogFileNames;   // 日志文件名白名单（空=引擎规范全量）；
};

class CFindResultsPage : public QWidget {
    Q_OBJECT

public:
    explicit CFindResultsPage(QWidget* a_pParent = nullptr);
    ~CFindResultsPage() override;   // 收后台线程（置取消 + join）

    // 发起搜索（上一轮未完自动取消收线；页面清空进「正在搜索」态）。
    void StartSearch(const SFindTask& a_stTask);

    bool SearchRunning() const { return m_thrWorker.joinable(); }

signals:
    // 点击行（单选且无修饰键）：打开文件并定位（1-based 行/列、命中长度字符数、
    //   命中文本 + 同串出现序——md/html 渲染后按行锚点+第 N 次出现文本定位；
    //   a_sLineText=源命中行文本，a_iLineDup=同文同行重复序）。
    void sigOpenHit(const QString& a_sAbsPath, int a_iLine, int a_iCol, int a_iLen,
                    const QString& a_sMatch, int a_iMatchOrd,
                    const QString& a_sLineText, int a_iLineDup);
    // 替换写盘完成：预览 Tab 若正显示该文件则重载。
    void sigFileRewritten(const QString& a_sAbsPath);

private:
    void BuildUi();
    void ApplyResults(const std::shared_ptr<QList<SFindHit>>& a_pHits,
                      const std::shared_ptr<SFileSearchStats>& a_pStats,
                      bool a_bCancelled);
    void OnRowClicked(int a_iRow);
    void OnTableContextMenu(const QPoint& a_stPos);   // 右键：复制内容/复制路径
    void OnReplaceOne();
    void OnReplaceAll();
    void RefreshButtons();            // 按钮态随：搜索中/模式/条目数/选中
    void FillTable();
    QString DisplayPath(const QString& a_sAbsPath) const;   // 相对路径列文案
    QList<int> SelectedRowsSorted() const;                  // 选中行按行号升序

    QTableWidget* m_pTable        = nullptr;
    QLabel*       m_pLblStat      = nullptr;   // 状态行（wordWrap，防撑宽）
    QPushButton*  m_pBtnReplace   = nullptr;   // 替换（替换模式可见）
    QPushButton*  m_pBtnReplaceAll = nullptr;  // 替换全部（替换模式可见）

    SFindTask      m_stTask;
    QList<SFindHit> m_aHits;
    std::atomic_bool m_bCancel{false};
    std::thread m_thrWorker;
    std::atomic<qint64> m_iTaskSeq{0};   // 任务序号（旧轮投递据此丢弃）
    int m_iReplacedCount = 0;   // 本次任务累计已替换处数（状态行展示）
};
