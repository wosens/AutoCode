import ctypes, sys
from ctypes import wintypes
from PIL import Image

u = ctypes.windll.user32
g32 = ctypes.windll.gdi32

def capture(hwnd, path):
    r = wintypes.RECT()
    if not u.GetWindowRect(hwnd, ctypes.byref(r)):
        print('GetWindowRect failed'); return
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    g32.SelectObject(mem, bmp)
    # PW_RENDERFULLCONTENT = 2 (works for DWM/Qt), PW_CLIENTONLY=1
    res = u.PrintWindow(hwnd, mem, 2)
    if res == 0:
        res = u.PrintWindow(hwnd, mem, 0)
    # extract bits
    class BMI(ctypes.Structure):
        _fields_ = [('biSize', ctypes.c_uint32), ('biWidth', ctypes.c_int32),
                    ('biHeight', ctypes.c_int32), ('biPlanes', ctypes.c_uint16),
                    ('biBitCount', ctypes.c_uint16), ('biCompression', ctypes.c_uint32),
                    ('biSizeImage', ctypes.c_uint32), ('biXPelsPerMeter', ctypes.c_int32),
                    ('biYPelsPerMeter', ctypes.c_int32), ('biClrUsed', ctypes.c_uint32),
                    ('biClrImportant', ctypes.c_uint32)]
    bmi = BMI()
    bmi.biSize = ctypes.sizeof(BMI); bmi.biWidth = w; bmi.biHeight = -h
    bmi.biPlanes = 1; bmi.biBitCount = 32; bmi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    img = Image.frombytes('RGBA', (w, h), buf.raw)
    img.convert('RGB').save(path)
    print('PrintWindow ret=%d size=%dx%d -> %s' % (res, w, h, path))
    u.ReleaseDC(hwnd, hdc)
    g32.DeleteObject(bmp); g32.DeleteDC(mem)

capture(int(sys.argv[1], 0), sys.argv[2])
