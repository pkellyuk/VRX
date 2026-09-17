#pragma once
#include <windows.h>
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
