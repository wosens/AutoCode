// probe_sorec —— 复现 2026-10-23 16:31/16:57/16:59 三连崩（0xC00000FD 栈溢出）。
// 机制（Qt 6.8 源码 qabstractscrollarea.cpp viewportEvent：viewport 的 Resize 直接
// return QFrame::event(e) → 子类 resizeEvent）：
//   resizeEvent → InvalidateWidth → 池逐条 setTextWidth+fnDocReady → OnDocReady
//   → onHeightCorrected → visualRect 同步执行布局 → updateGeometries 滚动条 range
//   变化 → 滚动条显隐翻转 → viewport 宽度 ±10px → viewport Resize → viewportEvent
//   → CChatListView::resizeEvent 重入（宽度不同过检查）→ 全量重排 → 同步无限递归。
// 自持震荡条件（滚动条临界）：估高总和 < 视口高（布局时滚动条隐藏）而实高总和
// > 视口高（首条校正后滚动条显示）——重排链中途滚动条显隐翻转，每次翻转宽度
// 变 ±10 → 重入 → 缓存清空回到估高 → 滚动条又隐藏 → ……
// 退出码：0=收敛（防护生效）；0xC00000FD=复现（栈溢出）。
#include <QApplication>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QEvent>
#include <cstdio>

#include "ui/CChatListView.h"
#include "ui/CChatModel.h"
#include "ui/SBlockData.h"

static int g_iViewResize = 0;
static int g_iVpResize = 0;

class ResizeSpy : public QObject {
public:
    using QObject::QObject;
    CChatListView* m_pView = nullptr;
    bool eventFilter(QObject* a_pObj, QEvent* a_pEv) override {
        if (a_pEv->type() == QEvent::Resize) {
            if (a_pObj == m_pView) {
                ++g_iViewResize;
            } else {
                ++g_iVpResize;
            }
        }
        return false;
    }
};

static QList<SBlockData> g_aBlocks;

// 临界构造：估高总和 < 视口高、实高总和 > 视口高（滚动条显隐在重排链中翻转）
static void MakeBlocks(int a_iN, int a_iChars)
{
    g_aBlocks.clear();
    for (int i = 0; i < a_iN; ++i) {
        SBlockData st;
        st.eKind = CMessageBlock::Assistant;
        // 长 CJK 行：估高按折行公式可能算 1 行，doc 实排 2 行——估/实差最大化的
        // 同时总高在视口高两侧徘徊
        st.sText = QString(a_iChars, QChar(0x6D4B))       // “测”×N 长行
                   + QStringLiteral("\n\n")
                   + QString(a_iChars, QChar(0x8BD5))
                   + QStringLiteral("\n\n")
                   + QString(a_iChars, QChar(0x6587));
        st.sBackend = QStringLiteral("DSH");
        g_aBlocks.append(st);
    }
}

int main(int argc, char** argv)
{
    QApplication a(argc, argv);
    // 参数可调（命令行：条数 每行字符数）
    const int iN = (argc > 1) ? std::atoi(argv[1]) : 6;
    const int iChars = (argc > 2) ? std::atoi(argv[2]) : 60;
    MakeBlocks(iN, iChars);

    QTabWidget st;
    QWidget w1, w2;
    auto* pL1 = new QVBoxLayout(&w1); pL1->setContentsMargins(0, 0, 0, 0);
    auto* pL2 = new QVBoxLayout(&w2); pL2->setContentsMargins(0, 0, 0, 0);

    auto* pView1 = new CChatListView(&w1);
    auto* pModel1 = new CChatModel(pView1);
    auto* pView2 = new CChatListView(&w2);
    auto* pModel2 = new CChatModel(pView2);

    pView1->SetModel(pModel1);
    pView2->SetModel(pModel2);
    pL1->addWidget(pView1, 1);
    pL2->addWidget(pView2, 1);
    st.addTab(&w1, QStringLiteral("助手1"));
    st.addTab(&w2, QStringLiteral("助手2"));
    st.resize(900, 700);
    st.setCurrentIndex(1);
    st.show();

    auto* pSpy = new ResizeSpy(&st);
    pSpy->m_pView = pView1;
    pView1->installEventFilter(pSpy);
    pView1->viewport()->installEventFilter(pSpy);

    // 模拟真实切页链：currentChanged 同步槽里 RebuildFrom 全量投影 + ScrollToBottom
    QObject::connect(&st, &QTabWidget::currentChanged, &st, [&](int a_iIdx) {
        if (a_iIdx != 0) {
            return;
        }
        pModel1->RebuildFrom(g_aBlocks);
        pView1->SetAutoScroll(true);
        pView1->ScrollToBottom();
    });

    QTimer::singleShot(300, &st, [&] { st.setCurrentIndex(0); });
    QTimer::singleShot(700, &st, [&] { st.setCurrentIndex(1); });
    QTimer::singleShot(1000, &st, [&] { st.setCurrentIndex(0); });
    // 视图宽度变化（真实场景：面板布局/窗口分栏变化触发首轮 resizeEvent）
    QTimer::singleShot(1200, &st, [&] { st.resize(880, 700); });
    QTimer::singleShot(1600, &a, [&] {
        std::printf("probe: no crash, viewResize=%d vpResize=%d\n",
                    g_iViewResize, g_iVpResize);
        a.exit(0);
    });
    const int rc = a.exec();
    std::printf("probe done rc=%d viewResize=%d vpResize=%d\n",
                rc, g_iViewResize, g_iVpResize);
    return rc;
}
