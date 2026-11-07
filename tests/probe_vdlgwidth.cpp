// probe_vdlgwidth —— 后端版本管理窗口宽度稳定性探针（2026-11-03 用户报：
// 点「验证所有后端」窗口变宽）。
// 根因：m_pLblInfo 是单行 QLabel 且无 wordWrap，其 minimumSizeHint=整行文字宽，
//   直接顶高 QDialog 布局最小宽。VerifyAll→UpdateInfoLine 追加「正在验证后端
//   可用性…（每后端最多 35 秒）」后布局最小宽超过当前 760px，Qt 自动把顶层窗口
//   撑宽（且验证结束文字变短也不会缩回）。
// 判定（三条全过 = 通过，退出码 0；任一失败 = 3）：
//   ① 结构锁：状态行 QLabel wordWrap 必须开启（宽度不再由文字长度决定）；
//   ② 真实路径锁：点「验证所有后端」按钮（真实点击，与用户操作同路径），窗口
//      宽度必须保持点击前的值不变；
//   ③ 压力锁：向状态行直接塞 300 字符长文，窗口宽度与布局 minimumSizeHint
//      宽仍不得超出初始宽（查询失败提示/更新中提示同走此 label，一并覆盖）。
// 手动运行：Execute\probe_vdlgwidth.exe（不挂 ctest——构造对话框会拉起真实
//   CLI 探测进程，关闭即由析构收敛）。
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QtTest>

#include <cstdio>

#include "ui/CVersionDialog.h"

int main(int argc, char** argv) {
    QApplication a(argc, argv);
    int iRc = 0;

    CVersionDialog stDlg(CVersionDialog::FBusyCheck(), nullptr);   // 空判据=放行
    stDlg.resize(760, 380);
    stDlg.show();
    for (int i = 0; i < 6; ++i)
        a.processEvents();
    const int iW0 = stDlg.width();
    std::printf("initial width=%d\n", iW0);

    QLabel* pLbl = stDlg.findChild<QLabel*>();
    if (pLbl == nullptr) {
        std::printf("probe_vdlgwidth: info label not found\n");
        return 3;
    }
    // ① 结构锁：wordWrap 开启
    const bool bWrap = pLbl->wordWrap();
    std::printf("[1] label wordWrap=%d -> %s\n", bWrap, bWrap ? "PASS" : "FAIL");
    if (!bWrap)
        iRc |= 3;

    // ② 真实点击「验证所有后端」（会拉起 5 路真实 CLI 探测，本探针随后立即
    //    关闭对话框由析构杀净；宽度变化发生在点击内的同步 UpdateInfoLine）
    QPushButton* pBtnVerify = nullptr;
    const QList<QPushButton*> aBtns = stDlg.findChildren<QPushButton*>();
    for (QPushButton* pBtn : aBtns) {
        if (pBtn->text() == QStringLiteral("验证所有后端")) {
            pBtnVerify = pBtn;
            break;
        }
    }
    if (pBtnVerify == nullptr) {
        std::printf("probe_vdlgwidth: verify button not found\n");
        return 3;
    }
    QTest::mouseClick(pBtnVerify, Qt::LeftButton);
    for (int i = 0; i < 8; ++i)
        a.processEvents();
    const int iW1 = stDlg.width();
    const bool bStable = (iW1 == iW0);
    std::printf("[2] after verify click width=%d (was %d) -> %s\n",
                iW1, iW0, bStable ? "PASS" : "FAIL");
    if (!bStable)
        iRc |= 3;

    // ③ 长文压力（查询失败提示同 label；wordWrap 后不得再顶宽布局）
    QString sLong;
    for (int i = 0; i < 30; ++i)
        sLong += QStringLiteral("状态压力测试段落%1，").arg(i);
    pLbl->setText(sLong);
    for (int i = 0; i < 6; ++i)
        a.processEvents();
    const int iW2 = stDlg.width();
    const int iMinW = stDlg.minimumSizeHint().width();
    const bool bLong = (iW2 == iW0) && iMinW <= iW0;
    std::printf("[3] long text width=%d minHint=%d (want <=%d) -> %s\n",
                iW2, iMinW, iW0, bLong ? "PASS" : "FAIL");
    if (!bLong)
        iRc |= 3;

    stDlg.close();   // 析构路径杀净探测子进程
    std::printf("probe_vdlgwidth done rc=%d\n", iRc);
    return iRc;
}
