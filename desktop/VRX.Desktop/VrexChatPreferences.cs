using System.Diagnostics;
using System.IO;
using System.Text.Json;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed record VrexChatPreferences(bool Helpful = false, bool SpeakReplies = false,
    bool AllowPcScreen = false, bool AllowPanelScreen = false, string[]? PcTools = null, bool AllowUbuntuMarks = false,
    bool AllowBotClipboard = false, bool AllowBotFileCopy = false)
{
    internal static VrexChatPreferences Load(string path)
    {
        try
        {
            if (!File.Exists(path)) return new();
            var saved = JsonSerializer.Deserialize<VrexChatPreferences>(File.ReadAllText(path)) ?? new();
            return saved with { PcTools = KnownTools(saved.PcTools) };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        { Debug.WriteLine("[Vrex chat preferences] " + ex.Message); return new(); }
    }

    internal void Save(string path)
    {
        try { ProfileStore.AtomicWrite(path, JsonSerializer.Serialize(this)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        { Debug.WriteLine("[Vrex chat preferences] " + ex.Message); }
    }

    internal static string[] KnownTools(string[]? values) => VrexPcPermissions.Tools
        .Where(tool => values?.Contains(tool.Id, StringComparer.Ordinal) == true).Select(tool => tool.Id).ToArray();
}
