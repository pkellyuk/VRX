using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace VRX.Desktop;

// A choice in the "Depth GPU" list. Id is what the profile stores and the engine
// receives as --depth-gpu: "same", "auto", or "name:<GPU name>#<n>" (the n-th card
// with that name). GPUs are identified by name, not adapter index, because Windows'
// adapter order can change between boots and driver updates (see gpu_choice.h).
public sealed record GpuChoice(string Id, string Label);

public static partial class Gpus
{
    public const string Same = "same";
    public const string Auto = "auto";

    [GeneratedRegex(@"^name:(?<name>.+)#(?<nth>\d{1,2})$")]
    private static partial Regex NamedId();

    public static bool ValidId(string? id) => id == Same || id == Auto || (id != null && NamedId().IsMatch(id));

    public static string NameOf(string id)
    {
        var m = NamedId().Match(id ?? "");
        return m.Success ? m.Groups["name"].Value : id ?? "";
    }

    // The fixed choices plus one per hardware GPU from the engine's --list-gpus lines
    // ("GPU|index|nth|MB|software|name"); anything else in the output is ignored.
    public static IReadOnlyList<GpuChoice> Parse(IEnumerable<string> engineLines)
    {
        var found = new List<(string Name, int Nth, long MB)>();
        foreach (string line in engineLines ?? [])
        {
            var parts = line.Split('|', 6);
            if (parts.Length != 6 || parts[0] != "GPU") continue;
            if (!int.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out int nth)) continue;
            if (!long.TryParse(parts[3], NumberStyles.None, CultureInfo.InvariantCulture, out long mb)) continue;
            if (parts[4] != "0") continue;                          // software adapters cannot run depth usefully
            string name = parts[5].Trim();
            if (name.Length == 0) continue;
            found.Add((name, nth, mb));
        }
        var choices = new List<GpuChoice>
        {
            new(Same, "Same GPU as the game (default)"),
            new(Auto, "Any other GPU (automatic)"),
        };
        foreach (var gpu in found)
        {
            bool duplicate = found.Count(g => string.Equals(g.Name, gpu.Name, StringComparison.OrdinalIgnoreCase)) > 1;
            string size = gpu.MB >= 1024 ? (gpu.MB / 1024.0).ToString("0.#", CultureInfo.InvariantCulture) + " GB" : gpu.MB + " MB";
            choices.Add(new($"name:{gpu.Name}#{gpu.Nth}", duplicate ? $"{gpu.Name} ({size}, card {gpu.Nth + 1})" : $"{gpu.Name} ({size})"));
        }
        return choices;
    }

    // Asks the engine for this PC's GPUs. It exits before any VR or model work, so
    // this never starts SteamVR. On any failure only the fixed choices are offered.
    public static IReadOnlyList<GpuChoice> List()
    {
        try
        {
            var start = new ProcessStartInfo(EngineSession.EnginePath(), "--list-gpus")
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = System.Text.Encoding.UTF8,
            };
            using var process = Process.Start(start);
            if (process == null) return Parse([]);
            var output = process.StandardOutput.ReadToEndAsync();
            if (!process.WaitForExit(5000)) { try { process.Kill(); } catch (InvalidOperationException) { } return Parse([]); }
            return Parse(output.Result.Split('\n').Select(l => l.TrimEnd('\r')));
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception or DirectoryNotFoundException)
        {
            return Parse([]);
        }
    }
}
