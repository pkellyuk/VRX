#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <cwctype>
#include <iterator>

inline std::wstring LowerWindowText(std::wstring text)
{
    for (auto& ch : text) ch = wchar_t(std::towlower(ch));
    return text;
}

struct CaptureWindow
{
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title, executable, windowClass, fullPath;
};

inline bool MatchesCaptureWindow(const CaptureWindow& window, const std::wstring& title,
    const std::wstring& executable, DWORD ownPid)
{
    if (window.pid == ownPid) return false;
    const auto cls = LowerWindowText(window.windowClass);
    const auto exe = LowerWindowText(window.executable);
    if (cls == L"consolewindowclass" || cls == L"cascadia_hosting_window_class" ||
        exe == L"windowsterminal.exe" || exe == L"conhost.exe" || exe == L"openconsole.exe" ||
        exe == L"cmd.exe" || exe == L"powershell.exe" || exe == L"pwsh.exe") return false;
    if (!executable.empty() && exe != LowerWindowText(executable)) return false;
    if (!title.empty() && LowerWindowText(window.title).find(LowerWindowText(title)) == std::wstring::npos)
        return false;
    return !title.empty() || !executable.empty();
}

struct CaptureWindowSearch
{
    std::wstring title, executable;
    std::wstring fullPath;
    DWORD pid = 0;
    HWND hwnd = nullptr;
    DWORD ownPid = GetCurrentProcessId();
    std::vector<CaptureWindow> matches;
};

inline BOOL CALLBACK EnumerateCaptureWindow(HWND hwnd, LPARAM context)
{
    auto& search = *reinterpret_cast<CaptureWindowSearch*>(context);
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return TRUE;
    CaptureWindow window;
    window.hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &window.pid);
    if ((search.pid && search.pid != window.pid) || (search.hwnd && search.hwnd != hwnd)) return TRUE;
    wchar_t title[2048] = {}, cls[256] = {}, path[32768] = {};
    GetWindowTextW(hwnd, title, int(std::size(title)));
    GetClassNameW(hwnd, cls, int(std::size(cls)));
    window.title = title; window.windowClass = cls;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, window.pid);
    if (process)
    {
        DWORD length = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &length))
        {
            window.executable.assign(path, length);
            window.fullPath = window.executable;
            const auto slash = window.executable.find_last_of(L"\\/");
            if (slash != std::wstring::npos) window.executable.erase(0, slash + 1);
        }
        CloseHandle(process);
    }
    if (!search.fullPath.empty() && CompareStringOrdinal(search.fullPath.c_str(), -1,
        window.fullPath.c_str(), -1, TRUE) != CSTR_EQUAL) return TRUE;
    if (MatchesCaptureWindow(window, search.title, search.executable, search.ownPid))
        search.matches.push_back(std::move(window));
    return TRUE;
}

// Where the window's client area (what the game draws) sits inside a captured
// window frame. Windows.Graphics.Capture frames of a window cover its visible DWM
// frame bounds - title bar and the 1-pixel border included, invisible resize
// borders excluded - so without cropping, a windowed game shows a light line
// around it and its title bar. All inputs are physical pixels (the engine is
// per-monitor DPI aware). Returns false, leaving `crop` unchanged, when the
// geometry is inconsistent (e.g. the frame and bounds sizes disagree during a
// resize or DPI change); callers then use the whole frame.
// `inset` trims that many extra pixels from every side: for a DPI-unaware window
// Windows stretches the picture with filtering, which blends its outermost pixel
// ring with the window border (see ScaledWindowInset).
inline bool ClientCropInFrame(const RECT& frameBounds, POINT clientOrigin, SIZE clientSize,
    int frameW, int frameH, RECT& crop, int inset = 0)
{
    if (frameW <= 0 || frameH <= 0 || clientSize.cx <= 0 || clientSize.cy <= 0) return false;
    const LONG boundsW = frameBounds.right - frameBounds.left, boundsH = frameBounds.bottom - frameBounds.top;
    if (std::abs(boundsW - frameW) > 2 || std::abs(boundsH - frameH) > 2) return false;

    RECT c{ clientOrigin.x - frameBounds.left, clientOrigin.y - frameBounds.top, 0, 0 };
    c.right = c.left + clientSize.cx;
    c.bottom = c.top + clientSize.cy;
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > frameW) c.right = frameW;
    if (c.bottom > frameH) c.bottom = frameH;
    // A client area that is barely visible (minimised, off-frame) is not usable.
    if (c.right - c.left < clientSize.cx / 2 || c.bottom - c.top < clientSize.cy / 2) return false;
    if (inset > 0 && c.right - c.left > 4 * inset && c.bottom - c.top > 4 * inset)
    {
        c.left += inset; c.top += inset; c.right -= inset; c.bottom -= inset;
    }
    crop = c;
    return true;
}

// Pixels to trim when Windows bitmap-stretches a window (DPI-unaware or
// system-aware on a monitor with a different scale): bilinear magnification by
// `scale` blends up to ceil(scale) - 1 pixels at the edge. 0 when not stretched.
inline int ScaledWindowInset(UINT windowDpi, UINT monitorDpi)
{
    if (windowDpi == 0 || monitorDpi <= windowDpi) return 0;
    const double scale = double(monitorDpi) / windowDpi;
    const int inset = int(std::ceil(scale - 1e-6)) - 1;
    return inset < 1 ? 1 : (inset > 3 ? 3 : inset);
}

// Live query for a window; see ClientCropInFrame.
inline bool QueryClientCrop(HWND hwnd, int frameW, int frameH, RECT& crop)
{
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    RECT bounds{}, client{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
    if (!GetClientRect(hwnd, &client)) return false;
    POINT origin{ 0, 0 };
    if (!ClientToScreen(hwnd, &origin)) return false;
    UINT monitorX = 0, monitorY = 0;
    const UINT monitorDpi = SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI,
        &monitorX, &monitorY)) ? monitorX : 0;
    const int inset = ScaledWindowInset(GetDpiForWindow(hwnd), monitorDpi);
    return ClientCropInFrame(bounds, origin, SIZE{ client.right - client.left, client.bottom - client.top }, frameW, frameH, crop, inset);
}
