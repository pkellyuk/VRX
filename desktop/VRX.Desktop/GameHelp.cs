using System.IO;
using VRX.VrexBridge;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

internal sealed record GameHelpProvider(string Id, string Name, string Url, string[] Hosts, bool IsFixture = false)
{
    internal bool LoopbackFixture => IsFixture && Environment.GetCommandLineArgs().Contains("--smoke-test");
    internal bool CanUpload(string url) => Uri.TryCreate(url, UriKind.Absolute, out var uri) &&
        (LoopbackFixture ? uri.Scheme == "http" && uri.IsLoopback && uri.Authority == new Uri(Url).Authority : uri.Scheme == "https" && uri.IsDefaultPort) &&
        Hosts.Contains(uri.IdnHost, StringComparer.OrdinalIgnoreCase) && !uri.AbsolutePath.StartsWith("/sorry", StringComparison.OrdinalIgnoreCase);
}
internal sealed class GameHelpPreferences(string path)
{
    internal static readonly GameHelpProvider[] Providers =
    [
        new("google-lens", "Google Lens", "https://www.google.com/imghp?hl=en", ["www.google.com", "google.com", "lens.google.com"]),
        new("google-ai", "Google AI Mode", "https://www.google.com/ai?hl=en", ["www.google.com", "google.com"]),
        new("copilot", "Microsoft Copilot", "https://copilot.microsoft.com/", ["copilot.microsoft.com", "copilot.com"]),
        new("gemini", "Google Gemini", "https://gemini.google.com/app", ["gemini.google.com"])
    ];
    private sealed class Data
    {
        public Data() { }
        public string Provider { get; set; } = "google-ai";
        public HashSet<string> Allowed { get; set; } = [];
    }
    private Data? data;
    private Data Read()
    {
        if (data != null) return data;
        try { data = JsonSerializer.Deserialize<Data>(File.ReadAllText(path)) ?? new(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { data = new(); }
        if (!Providers.Any(p => p.Id == data.Provider)) data.Provider = "google-ai";
        data.Allowed ??= []; data.Allowed.IntersectWith(Providers.Select(p => p.Id)); return data;
    }
    internal GameHelpProvider Selected => Providers.Single(p => p.Id == Read().Provider);
    internal bool Allowed(GameHelpProvider provider) => Read().Allowed.Contains(provider.Id);
    internal void Select(GameHelpProvider provider) { Read().Provider = provider.Id; Save(); }
    internal void Permit(GameHelpProvider provider, bool allow)
    { if (allow) Read().Allowed.Add(provider.Id); else Read().Allowed.Remove(provider.Id); Save(); }
    private void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!); string temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(Read())); File.Move(temporary, path, true);
    }
    private static GameHelpPreferences? current;
    internal static GameHelpPreferences Current => current ??= new(Path.Combine(
        Environment.GetCommandLineArgs().Contains("--smoke-test") ? Path.Combine(Path.GetTempPath(), "VRX-GameHelpSmoke", Environment.ProcessId.ToString()) :
            VrxDataDirectory.Root, "game-help.json"));
    internal static void SmokeTest(string output)
    {
        string file = Path.Combine(output, "game-help-settings-test.json"); if (File.Exists(file)) File.Delete(file);
        var prefs = new GameHelpPreferences(file); var google = Providers[0]; var copilot = Providers[2];
        if (prefs.Allowed(google)) throw new Exception("Screenshot sharing must default off");
        prefs.Permit(google, true); prefs.Select(copilot); prefs = new(file);
        if (!prefs.Allowed(google) || prefs.Allowed(copilot) || prefs.Selected.Id != copilot.Id) throw new Exception("Screenshot consent must persist per provider");
        if (google.CanUpload("https://www.google.com.attacker.example/") || google.CanUpload("http://www.google.com/") || google.CanUpload("https://accounts.google.com/"))
            throw new Exception("Game help must not give a screenshot to redirects, lookalikes or sign-in pages");
        prefs.Permit(google, false); if (new GameHelpPreferences(file).Allowed(google)) throw new Exception("Screenshot consent must be revocable");
    }
}
internal sealed class GameHelpImage(GameHelpProvider provider, byte[] bytes) : IDisposable
{
    internal GameHelpProvider Provider { get; } = provider;
    internal byte[] Bytes { get; } = bytes;
    internal static GameHelpImage Capture(BitmapSource source, GameHelpProvider provider)
    {
        double scale = Math.Min(1, 1920.0 / Math.Max(source.PixelWidth, source.PixelHeight));
        BitmapSource image = scale < 1 ? new TransformedBitmap(source, new ScaleTransform(scale, scale)) : source;
        var encoder = new JpegBitmapEncoder { QualityLevel = 88 }; encoder.Frames.Add(BitmapFrame.Create(image));
        using var stream = new MemoryStream(); encoder.Save(stream);
        if (stream.Length > 5 * 1024 * 1024) throw new InvalidOperationException(Loc.Get("GameHelpImageTooLarge"));
        return new(provider, stream.ToArray());
    }
    public void Dispose() => Array.Clear(Bytes);
}
