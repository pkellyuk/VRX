// vrx-steam-launch.exe: the fixed entry point that Steam's per-game Launch Options use for
// VRX's true-geometry launches:  "<this exe>" --steam-game <app id> -- %command%
//
// VRX copies this program to %LOCALAPPDATA%\VRX\launcher\ and records the VRX copy to hand
// over to in launcher\target.txt (the full path of VRX.Desktop.exe; the last complete copy
// started wins). Steam's Launch Options therefore never name a VRX install folder, so moving,
// reinstalling or switching between the Steam and GitHub copies of VRX cannot leave them stale.
// If no usable VRX copy is recorded, the game is started normally (the command after "--"),
// so a Steam launch always starts the game. Statically linked: it runs on its own.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <string>

static std::wstring g_log;

static void Log(const wchar_t* format, ...)
{
    if (g_log.empty()) return;
    wchar_t text[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_log.c_str(), L"a, ccs=UTF-8") != 0 || !file) return;
    fwprintf(file, L"%04u-%02u-%02u %02u:%02u:%02u [%lu] %s\n", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), text);
    fclose(file);
}

static std::wstring LocalAppData()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) || !path) return L"";
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

// The VRX.Desktop.exe recorded in target.txt, or "" when it is missing or not usable.
static std::wstring ReadTarget(const std::wstring& file)
{
    if (file.empty()) return L"";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Log(L"ReadTarget: no %s (error %lu)", file.c_str(), GetLastError());
        return L"";
    }
    char bytes[4096] = {};
    DWORD read = 0;
    BOOL ok = ReadFile(handle, bytes, sizeof(bytes) - 1, &read, nullptr);
    CloseHandle(handle);
    if (!ok || read == 0) return L"";
    int start = (read >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) ? 3 : 0;
    int length = MultiByteToWideChar(CP_UTF8, 0, bytes + start, (int)read - start, nullptr, 0);
    if (length <= 0) return L"";
    std::wstring path(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes + start, (int)read - start, path.data(), length);
    while (!path.empty() && (path.back() == L'\r' || path.back() == L'\n' || path.back() == L' ' || path.back() == L'\t')) path.pop_back();
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (_wcsicmp(name.c_str(), L"VRX.Desktop.exe") != 0)
    {
        Log(L"ReadTarget: %s is not VRX.Desktop.exe", path.c_str());
        return L"";
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        Log(L"ReadTarget: %s no longer exists", path.c_str());
        return L"";
    }
    return path;
}

// Everything after this program's own path on the command line, exactly as Steam passed it.
static const wchar_t* ArgumentsAfterProgram(const wchar_t* line)
{
    if (!line) return L"";
    const wchar_t* p = line;
    if (*p == L'"')
    {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p) ++p;
    }
    else
    {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

// One argument quoted the way CommandLineToArgvW and the C runtime read it back.
static std::wstring Quote(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring result = L"\"";
    for (size_t i = 0;; ++i)
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++backslashes; }
        if (i == arg.size())
        {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        }
        else
        {
            result.append(backslashes, L'\\');
            result.push_back(arg[i]);
        }
    }
    result.push_back(L'"');
    return result;
}

// Starts `application` with the full command line `command` and waits for it.
static int Run(const std::wstring& application, const std::wstring& command)
{
    Log(L"Run enter: %s", command.c_str());
    std::wstring mutableCommand = command;
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        Log(L"Run exit: CreateProcess failed (error %lu)", GetLastError());
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    Log(L"Run exit: code %lu", code);
    return (int)code;
}

// No VRX copy to hand over to: start the game exactly as Steam asked (the part after "--").
static int RunGameDirectly()
{
    Log(L"RunGameDirectly enter");
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    int separator = -1;
    for (int i = 1; i < count; ++i)
    {
        if (wcscmp(args[i], L"--") == 0) { separator = i; break; }
    }
    if (separator < 0 || separator + 1 >= count)
    {
        Log(L"RunGameDirectly exit: no game command after \"--\"");
        LocalFree(args);
        return 1;
    }
    std::wstring game = args[separator + 1], command;
    for (int i = separator + 1; i < count; ++i)
    {
        if (!command.empty()) command.push_back(L' ');
        command += Quote(args[i]);
    }
    LocalFree(args);
    return Run(game, command);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::wstring data = LocalAppData();
    if (!data.empty())
    {
        CreateDirectoryW((data + L"\\VRX").c_str(), nullptr);
        CreateDirectoryW((data + L"\\VRX\\logs").c_str(), nullptr);
        g_log = data + L"\\VRX\\logs\\steam-launch.log";
    }
    Log(L"enter: %s", GetCommandLineW());
    std::wstring target = data.empty() ? L"" : ReadTarget(data + L"\\VRX\\launcher\\target.txt");
    if (target.empty())
    {
        int code = RunGameDirectly();
        Log(L"exit: game started without VRX, code %d", code);
        return code;
    }
    int code = Run(target, Quote(target) + L" " + ArgumentsAfterProgram(GetCommandLineW()));
    Log(L"exit: handed over to %s, code %d", target.c_str(), code);
    return code;
}
