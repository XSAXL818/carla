"""Client-only window capture via PrintWindow(hwnd, hdc, 3).

Flag 3 = PW_CLIENTONLY(1) | PW_RENDERFULLCONTENT(2). Flag 2 alone has been
observed to return an all-black client area for this engine's flip-model
swapchain, so the combined value is used deliberately; the script also prints
mean luminance so an all-black frame is detectable without opening the file.

Usage: python shot.py out.png [--pid 1234] [--title substring] [--exe UnrealEditor]
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

PW_CLIENTONLY = 1
PW_RENDERFULLCONTENT = 2


def find_hwnd(pid: int | None, title: str | None, exe: str | None) -> tuple[int, str]:
    found: list[tuple[int, str]] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lparam):
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        text = buf.value
        wnd_pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wnd_pid))
        if not user32.IsWindowVisible(hwnd):
            return True
        if pid is not None and wnd_pid.value != pid:
            return True
        if title is not None and title.lower() not in text.lower():
            return True
        if exe is not None:
            ctypes.windll.kernel32.OpenProcess  # noqa: B018  (no-op keep-alive)
            found.append((hwnd, text))
        else:
            found.append((hwnd, text))
        return True

    user32.EnumWindows(cb, 0)
    if not found:
        raise SystemExit("no matching visible top-level window")
    # prefer the widest window (the game viewport, not a tooltip/titlebar child)
    best = max(found, key=lambda h: (lambda r: r.right - r.left)(wt.RECT()) or 1
               if False else 0) if False else found[0]
    return best


def find_window(pid: int | None, title: str | None) -> tuple[int, str]:
    matches: list[tuple[int, str, int]] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        text = buf.value
        if title is not None and title.lower() not in text.lower():
            return True
        wnd_pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wnd_pid))
        if pid is not None and wnd_pid.value != pid:
            return True
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        matches.append((hwnd, text, (rect.right - rect.left) * (rect.bottom - rect.top)))
        return True

    user32.EnumWindows(cb, 0)
    if not matches:
        raise SystemExit("no matching visible top-level window")
    hwnd, text, area = max(matches, key=lambda m: m[2])
    return hwnd, text


def capture(hwnd: int, out_path: str, flag: int = PW_CLIENTONLY | PW_RENDERFULLCONTENT):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        raise SystemExit(f"client rect empty: {w}x{h}")
    hdc_screen = user32.GetDC(hwnd)
    hdc = gdi32.CreateCompatibleDC(hdc_screen)
    bmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc, bmp)
    ok = user32.PrintWindow(hwnd, hdc, flag)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                    ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h  # top-down
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(out_path)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(hwnd, hdc_screen)
    gray = img.convert("L")
    hist = gray.histogram()
    total = sum(hist) or 1
    mean = sum(i * c for i, c in enumerate(hist)) / total
    darkest = sum(hist[:8]) / total
    print(f"PrintWindow ok={ok} size={w}x{h} mean_luma={mean:.1f} "
          f"frac_near_black={darkest:.3f} -> {out_path}")
    return ok


if __name__ == "__main__":
    out = sys.argv[1]
    pid = None
    title = None
    flag = PW_CLIENTONLY | PW_RENDERFULLCONTENT
    for i, a in enumerate(sys.argv):
        if a == "--pid":
            pid = int(sys.argv[i + 1])
        if a == "--title":
            title = sys.argv[i + 1]
        if a == "--flag":
            flag = int(sys.argv[i + 1])
    hwnd, text = find_window(pid, title)
    print(f"hwnd=0x{hwnd:X} title={text!r} flag={flag}")
    capture(hwnd, out, flag)
