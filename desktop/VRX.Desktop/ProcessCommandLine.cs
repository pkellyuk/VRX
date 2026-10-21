using System.Diagnostics;
using System.Runtime.InteropServices;

namespace VRX.Desktop;

// A process's command line, read natively (NtQueryInformationProcess with
// ProcessCommandLineInformation, Windows 8.1 and later) instead of starting a hidden
// PowerShell for every attach. Needs only PROCESS_QUERY_LIMITED_INFORMATION. Also the
// parent process ID, for the anti-cheat check of a game's helper processes.
internal static class ProcessCommandLine
{
    private const uint QueryLimitedInformation = 0x1000;
    private const int ProcessBasicInformationClass = 0, ProcessCommandLineInformationClass = 60;
    private const int StatusInfoLengthMismatch = unchecked((int)0xC0000004), StatusBufferTooSmall = unchecked((int)0xC0000023);

    [StructLayout(LayoutKind.Sequential)]
    private struct UnicodeString
    {
        public ushort Length, MaximumLength;
        public nint Buffer;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BasicInformation
    {
        public nint ExitStatus, PebBaseAddress, AffinityMask, BasePriority, UniqueProcessId, InheritedFromUniqueProcessId;
    }

    [DllImport("ntdll.dll")] private static extern int NtQueryInformationProcess(nint process, int informationClass, nint information, int length, out int returned);
    [DllImport("ntdll.dll")] private static extern int NtQueryInformationProcess(nint process, int informationClass, out BasicInformation information, int length, out int returned);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(nint handle);

    // The full command line (argv[0] included), or null when it cannot be read.
    public static string? Read(int pid)
    {
        if (pid <= 0) return null;
        nint process = OpenProcess(QueryLimitedInformation, false, pid);
        if (process == 0)
        {
            Debug.WriteLine($"[Command line] pid {pid}: OpenProcess failed ({Marshal.GetLastWin32Error()})");
            return null;
        }
        try
        {
            int size = 1024;
            for (int attempt = 0; attempt < 4; attempt++)
            {
                nint buffer = Marshal.AllocHGlobal(size);
                try
                {
                    int status = NtQueryInformationProcess(process, ProcessCommandLineInformationClass, buffer, size, out int returned);
                    if (status is StatusInfoLengthMismatch or StatusBufferTooSmall && returned > size) { size = returned; continue; }
                    if (status != 0)
                    {
                        Debug.WriteLine($"[Command line] pid {pid}: status 0x{status:X8}");
                        return null;
                    }
                    var text = Marshal.PtrToStructure<UnicodeString>(buffer);
                    return text.Buffer == 0 ? "" : Marshal.PtrToStringUni(text.Buffer, text.Length / 2);
                }
                finally { Marshal.FreeHGlobal(buffer); }
            }
            return null;
        }
        finally { CloseHandle(process); }
    }

    // The process that started `pid`, or 0.
    public static int Parent(int pid)
    {
        if (pid <= 0) return 0;
        nint process = OpenProcess(QueryLimitedInformation, false, pid);
        if (process == 0) return 0;
        try
        {
            return NtQueryInformationProcess(process, ProcessBasicInformationClass, out BasicInformation info, Marshal.SizeOf<BasicInformation>(), out _) == 0 ?
                (int)info.InheritedFromUniqueProcessId : 0;
        }
        finally { CloseHandle(process); }
    }

    // Launch arguments without anything that looks like a credential: store launchers pass
    // one-time sign-in codes and tokens on the command line (Epic's -AUTH_PASSWORD=, EA's and
    // Ubisoft's tokens). Only what is needed to start the game again is kept; a flag that
    // carries a secret in the next argument loses that argument too.
    public static string[] WithoutCredentials(IEnumerable<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        var kept = new List<string>();
        bool skipValue = false;
        foreach (string argument in arguments)
        {
            if (skipValue)
            {
                skipValue = false;
                if (!argument.StartsWith('-') && !argument.StartsWith('/')) continue;
            }
            if (!Sensitive(argument)) { kept.Add(argument); continue; }
            Debug.WriteLine($"[Command line] dropped a credential-like argument ({argument.Split('=')[0]})");
            // "-token VALUE": the secret is the next argument.
            skipValue = !argument.Contains('=') && !argument.Contains(':');
        }
        return kept.ToArray();
    }

    private static readonly string[] SensitivePrefixes = ["-auth_password", "-auth_login", "-auth_type", "-epicusername", "-epicuserid", "-epicsandboxid",
        "-epicdeploymentid", "-epicovt", "-eaaccesstoken", "-eaauthcode", "-eagameid", "-uplay_steam_mode", "-upc_", "/upc_", "-ubisoft", "-steamticket"];
    private static readonly string[] SensitiveWords = ["token", "password", "passwd", "auth", "ticket", "session=", "secret", "credential", "apikey", "api_key", "exchangecode", "accesscode"];

    internal static bool Sensitive(string argument)
    {
        ArgumentNullException.ThrowIfNull(argument);
        string lower = argument.ToLowerInvariant();
        return SensitivePrefixes.Any(p => lower.StartsWith(p, StringComparison.Ordinal)) || SensitiveWords.Any(w => lower.Contains(w, StringComparison.Ordinal));
    }
}
