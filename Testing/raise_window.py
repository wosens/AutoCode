# -*- coding: utf-8 -*-
"""AutoCode 实例窗口「点了任务栏不显示」一键自救。

用法：
    python raise_window.py                # 列出所有 AutoCode 主窗口
    python raise_window.py TuKan          # 把标题含 TuKan 的实例窗口抬到前台并激活

原理：激活僵死的窗口对 SetForegroundWindow 静默失败（前台变 NULL），
但带激活的 SetWindowPos(HWND_TOP) 能强制重建激活链。
"""
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32


def list_windows():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, lp):
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, buf, 256)
        cls = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, cls, 256)
        if cls.value.startswith('Qt') and 'AutoCode' in buf.value and u.IsWindowVisible(h):
            found.append((h, buf.value))
        return True
    u.EnumWindows(cb, 0)
    return found


def raise_one(title_part):
    hits = [(h, t) for h, t in list_windows() if title_part.lower() in t.lower()]
    if not hits:
        print('没有找到标题含 %r 的可见 AutoCode 窗口' % title_part)
        return 1
    for h, t in hits:
        # 带激活抬升（这里故意不用 SWP_NOACTIVATE——激活正是要重建的东西）
        u.SetWindowPos(h, 0, 0, 0, 0, 0, 0x0001 | 0x0002)   # NOSIZE|NOMOVE
        time.sleep(0.3)
        fg = u.GetForegroundWindow()
        ok = (fg == h)
        print('0x%X %-40s 激活=%s 前台=%s' % (h, t, 'OK' if ok else 'FAIL',
              u.GetWindowTextW(fg, ctypes.create_unicode_buffer(256), 256) and '' or ''))
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('当前所有 AutoCode 实例窗口：')
        for h, t in list_windows():
            print('  0x%X  %s' % (h, t))
        print('用法: python raise_window.py <标题关键字>   # 如 TuKan')
    else:
        sys.exit(raise_one(sys.argv[1]))
