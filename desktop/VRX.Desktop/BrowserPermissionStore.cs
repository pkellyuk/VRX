using System.IO;
using System.Text.Json;

namespace VRX.Desktop;

// Grants belong to both the requesting origin and the page embedding it. A grant
// to an embedded service on one site does not grant it access on every other site.
internal sealed class BrowserPermissionStore(string path)
{
    internal sealed record Decision(string Page, string Origin, string Permission, bool Allow);
    private readonly object gate = new();
    private List<Decision>? decisions;
    internal static string? Origin(string value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme is not ("https" or "http") || string.IsNullOrEmpty(uri.Host)) return null;
        return new UriBuilder(uri.Scheme, uri.IdnHost, uri.IsDefaultPort ? -1 : uri.Port).Uri.GetLeftPart(UriPartial.Authority);
    }
    private List<Decision> Read()
    {
        if (decisions != null) return decisions;
        try
        {
            decisions = (JsonSerializer.Deserialize<List<Decision>>(File.ReadAllText(path)) ?? [])
                .Where(d => d != null && Origin(d.Page) == d.Page && Origin(d.Origin) == d.Origin && !string.IsNullOrEmpty(d.Permission)).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { decisions = []; }
        return decisions;
    }
    internal bool? Choice(string page, string origin, string permission)
    { lock (gate) return Read().LastOrDefault(d => d.Page == page && d.Origin == origin && d.Permission == permission)?.Allow; }
    internal Decision[] ForPage(string page) { lock (gate) return Read().Where(d => d.Page == page).ToArray(); }
    internal void Remember(string page, string origin, string[] permissions, bool allow)
    {
        if (Origin(page) != page || Origin(origin) != origin) throw new ArgumentException("Permission choices require attributable web origins");
        lock (gate)
        {
            var data = Read();
            data.RemoveAll(d => d.Page == page && d.Origin == origin && permissions.Contains(d.Permission));
            data.AddRange(permissions.Distinct().Select(p => new Decision(page, origin, p, allow))); Save();
        }
    }
    internal Decision[] Forget(string page)
    {
        lock (gate) { var removed = ForPage(page); Read().RemoveAll(d => d.Page == page); Save(); return removed; }
    }
    private void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(decisions)); File.Move(temporary, path, true);
    }
    internal static void SmokeTest(string output)
    {
        string file = Path.Combine(output, "browser-permissions-test.json");
        if (File.Exists(file)) File.Delete(file);
        var store = new BrowserPermissionStore(file);
        if (Origin("https://EXAMPLE.com:443/path?q=x") != "https://example.com" || Origin("data:text/html,hello") != null)
            throw new Exception("Browser permissions must use canonical, attributable origins");
        store.Remember("https://game.example", "https://voice.example", ["microphone"], true);
        store.Remember("https://game.example", "https://voice.example", ["camera"], false);
        store = new(file);
        if (store.Choice("https://game.example", "https://voice.example", "microphone") != true ||
            store.Choice("https://game.example", "https://voice.example", "camera") != false ||
            store.Choice("https://other.example", "https://voice.example", "microphone") != null ||
            store.Choice("http://game.example", "https://voice.example", "microphone") != null)
            throw new Exception("Permissions must persist Allow and Deny separately by origin, capability and embedding page");
        if (store.Forget("https://game.example").Length != 2 || new BrowserPermissionStore(file).ForPage("https://game.example").Length != 0)
            throw new Exception("Forgetting a site's choices must survive restart");
    }
}
