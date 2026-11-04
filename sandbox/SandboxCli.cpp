// 测试沙箱 CLI 参数解析实现（说明见同名 .h；2026-11-01 自 CSandboxCore.cpp 迁入，
// 实现体未改——VkOfName 的 VkKeyScanW 依赖 user32，Qt 链接环境默认可得）。
#include "SandboxCli.h"

#include <QHash>
#include <windows.h>

namespace sbx {

quint32 VkOfName(const QString& a_sName) {
    const QString s = a_sName.trimmed().toLower();
    if (s.isEmpty()) {
        return 0;
    }
    static const QHash<QString, quint32> sMap = {
        { QStringLiteral("ctrl"),   VK_CONTROL }, { QStringLiteral("control"), VK_CONTROL },
        { QStringLiteral("shift"),  VK_SHIFT },   { QStringLiteral("alt"),     VK_MENU },
        { QStringLiteral("win"),    VK_LWIN },    { QStringLiteral("meta"),    VK_LWIN },
        { QStringLiteral("enter"),  VK_RETURN },  { QStringLiteral("return"),  VK_RETURN },
        { QStringLiteral("tab"),    VK_TAB },     { QStringLiteral("esc"),     VK_ESCAPE },
        { QStringLiteral("escape"), VK_ESCAPE },  { QStringLiteral("space"),   VK_SPACE },
        { QStringLiteral("left"),   VK_LEFT },    { QStringLiteral("right"),   VK_RIGHT },
        { QStringLiteral("up"),     VK_UP },      { QStringLiteral("down"),    VK_DOWN },
        { QStringLiteral("del"),    VK_DELETE },  { QStringLiteral("delete"),  VK_DELETE },
        { QStringLiteral("back"),   VK_BACK },    { QStringLiteral("backspace"), VK_BACK },
        { QStringLiteral("home"),   VK_HOME },    { QStringLiteral("end"),     VK_END },
        { QStringLiteral("pgup"),   VK_PRIOR },   { QStringLiteral("pageup"),  VK_PRIOR },
        { QStringLiteral("pgdn"),   VK_NEXT },    { QStringLiteral("pagedown"), VK_NEXT },
        { QStringLiteral("insert"), VK_INSERT },  { QStringLiteral("ins"),     VK_INSERT },
        { QStringLiteral("apps"),   VK_APPS },
        { QStringLiteral("f1"), VK_F1 },  { QStringLiteral("f2"), VK_F2 },
        { QStringLiteral("f3"), VK_F3 },  { QStringLiteral("f4"), VK_F4 },
        { QStringLiteral("f5"), VK_F5 },  { QStringLiteral("f6"), VK_F6 },
        { QStringLiteral("f7"), VK_F7 },  { QStringLiteral("f8"), VK_F8 },
        { QStringLiteral("f9"), VK_F9 },  { QStringLiteral("f10"), VK_F10 },
        { QStringLiteral("f11"), VK_F11 }, { QStringLiteral("f12"), VK_F12 },
        { QStringLiteral("capslock"), VK_CAPITAL }, { QStringLiteral("numlock"), VK_NUMLOCK },
        { QStringLiteral(","), VK_OEM_COMMA }, { QStringLiteral("."), VK_OEM_PERIOD },
    };
    if (sMap.contains(s)) {
        return sMap.value(s);
    }
    if (s.size() == 1) {
        const SHORT sVk = VkKeyScanW(s.at(0).unicode());
        if (sVk != -1) {
            return static_cast<quint32>(LOBYTE(sVk));
        }
    }
    return 0;
}

bool ParseCli(const QStringList& a_aArgs, QString* a_psCmd, QJsonObject* a_pOut) {
    if (a_psCmd == nullptr || a_pOut == nullptr || a_aArgs.size() < 3) {
        return false;
    }
    const QString sCmd = a_aArgs.at(2).toLower();
    const int iN = a_aArgs.size();
    auto fnArg = [&a_aArgs](int a_i, const QString& a_sDef = QString()) -> QString {
        return (a_i < a_aArgs.size()) ? a_aArgs.at(a_i) : a_sDef;
    };
    auto fnH = [&fnArg](int a_i) { return fnArg(a_i); };   // hwnd 文本原样传
    QJsonObject o;
    if (sCmd == QStringLiteral("windows") || sCmd == QStringLiteral("status")
            || sCmd == QStringLiteral("record_on") || sCmd == QStringLiteral("record_off")
            || sCmd == QStringLiteral("restore-all") || sCmd == QStringLiteral("restore_all")) {
        *a_psCmd = (sCmd == QLatin1String("restore-all")) ? QStringLiteral("restore_all") : sCmd;
        *a_pOut = o;
        return true;
    }
    if (sCmd == QStringLiteral("capture")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
        if (iN >= 5) o.insert(QStringLiteral("path"), fnArg(4));
    } else if (sCmd == QStringLiteral("click") || sCmd == QStringLiteral("hclick")) {
        if (iN < 6) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("x"), fnArg(4).toInt());
        o.insert(QStringLiteral("y"), fnArg(5).toInt());
        const QString sMode = (iN >= 7) ? fnArg(6).toLower() : QString();
        o.insert(QStringLiteral("dbl"), sMode == QLatin1String("dbl")
                                     || sMode == QLatin1String("double"));
        o.insert(QStringLiteral("right"), sMode == QLatin1String("right"));
        *a_psCmd = sCmd;   // hclick = 宿主代发硬件点击（只认硬件输入的程序专用）
    } else if (sCmd == QStringLiteral("drag")) {
        if (iN < 8) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("x1"), fnArg(4).toInt());
        o.insert(QStringLiteral("y1"), fnArg(5).toInt());
        o.insert(QStringLiteral("x2"), fnArg(6).toInt());
        o.insert(QStringLiteral("y2"), fnArg(7).toInt());
        if (iN >= 9) o.insert(QStringLiteral("ms"), fnArg(8).toInt());
    } else if (sCmd == QStringLiteral("type")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("text"), QStringList(a_aArgs.mid(4)).join(QLatin1Char(' ')));
    } else if (sCmd == QStringLiteral("launch")) {
        // --sandbox launch <exe> [参数...]：主进程代启动被测程序（2026-10-14
        //   断链漏收治理：Start-Process/cmd start 中间人秒退会切断进程树）。
        if (iN < 4) return false;
        o.insert(QStringLiteral("exe"), fnArg(3));
        if (iN >= 5) {
            // 余参重建成命令行串：含空格/制表的元素补引号（Qt 拆参已剥一层）。
            QStringList aRest;
            for (int k = 4; k < iN; ++k) {
                const QString s = a_aArgs.at(k);
                aRest.append(s.contains(QLatin1Char(' '))
                                 || s.contains(QLatin1Char('\t'))
                                 || s.isEmpty()
                             ? QStringLiteral("\"%1\"").arg(s) : s);
            }
            o.insert(QStringLiteral("args"), aRest.join(QLatin1Char(' ')));
        }
    } else if (sCmd == QStringLiteral("keys")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("keys"), fnArg(4));
    } else if (sCmd == QStringLiteral("command")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("cmd"), fnArg(4).toInt());
    } else if (sCmd == QStringLiteral("button")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
    } else if (sCmd == QStringLiteral("tree") || sCmd == QStringLiteral("add")
            || sCmd == QStringLiteral("restore") || sCmd == QStringLiteral("close")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
    } else {
        return false;   // 未知命令
    }
    *a_psCmd = sCmd;
    *a_pOut = o;
    return true;
}

} // namespace sbx
