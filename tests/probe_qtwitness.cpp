#include <QCoreApplication>
#include <QTimer>
#include "core/CFatalWitness.h"

// qFatal 见证链路验证（手动运行，不挂 ctest）：装见证 → 事件循环启动后 qFatal →
// 进程 fastfail 消失；验证 exe 同级 dump\_qfatal_witness.txt 出现 witness-probe 行
// 即证明「qFatal 零痕迹死亡 → 遗言落盘」链路通（readme v5.1.429）。
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    CFatalWitness::Install();
    QTimer::singleShot(200, [] {
        qFatal("witness-probe-test: qFatal chain verification");
    });
    return app.exec();
}
