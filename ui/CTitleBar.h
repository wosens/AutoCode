#pragma once

#include <QWidget>

class QLabel;
class QToolButton;
class QMouseEvent;

// FR-3 自绘标题栏：一行高 widget，左侧软件标题 + 右侧最小化/最大化·还原/关闭三按钮。
// 自身不处理拖拽/缩放——交由 CMainWindow::nativeEvent 拦 WM_NCHITTEST 统一判定
// （保留系统边框/阴影的方案 Q4-(b)，体验接近标准窗口）。
class CTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit CTitleBar(QWidget* a_pParent = nullptr);

    // 主窗口最大化/还原时调用，切换最大化按钮二态图标。
    void SetMaximized(bool a_bMaximized);
    // 2026.09.07 标题文本动态化：主窗口按当前工程刷标题（工程名-AutoCode，未开工程=AutoCode）。
    void SetTitleText(const QString& a_sText);
    // 需求6：返回按钮当前图标态——m_bMaximized==true 表示当前显示「还原」图标(□，即窗口已最大化)。
    //   最大化按钮据此判翻转方向（图标是还原图标→还原窗口，反之最大化）。命名避开 IsMaximized
    //   （与 <windowsx.h> 的 IsMaximized(hwnd) 宏撞名致 C4003）。
    bool IsMaxState() const { return m_bMaximized; }

    // FR-3：给定屏幕坐标，判定落在标题栏的哪一区，供 CMainWindow::nativeEvent
    //        的 WM_NCHITTEST 做低成本命中（只做坐标比较，内部直接用成员指针，不遍历 widget 树）。
    enum EHitZone {
        HitNone,     // 非标题栏区
        HitButton,   // 三按钮区（应 return false 让 Qt 派发 click）
        HitIcon,     // 左上角图标区（应返回 HTSYSMENU，系统菜单区，由 nativeEvent 弹菜单）
        HitCaption   // 标题栏空白（应返回 HTCAPTION 可拖拽 + 双击最大化）
    };
    EHitZone HitTest(const QPoint& a_stScreenPos) const;

    // 系统菜单弹出锚点：图标左下角（菜单从图标下方展开，Windows 原生样式）。
    QPoint IconPopupPos() const;

signals:
    void sigMinimize();       // 最小化按钮
    void sigMaximizeToggle(); // 最大化·还原按钮
    void sigClose();          // 关闭按钮

private:
    QLabel*      m_pIcon     = nullptr;   // FR-10 左上角应用图标
    QLabel*      m_pLblTitle  = nullptr;
    QLabel*      m_pLblVersion = nullptr; // 标题右侧灰色小字版本号（v主.次.编译号）
    QToolButton* m_pBtnMin    = nullptr;
    QToolButton* m_pBtnMax    = nullptr;
    QToolButton* m_pBtnClose  = nullptr;
    bool         m_bMaximized = false;
};
