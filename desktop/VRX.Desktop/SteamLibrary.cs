using System.Diagnostics;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace VRX.Desktop;

// Local discovery only: no Steam credentials, Web API, or writes to Steam's files.
public static class SteamLibrary
{
    public sealed record Game(string AppId, string Title, string Directory);
    public sealed record Binding(string Executable, bool WrapperConfigured = false);
    // Installed: Steam itself is installed (the chooser hides its section otherwise).
    public sealed record Discovery(IReadOnlyList<Game> Games, IReadOnlyList<string> Errors, bool Installed = true);
    internal sealed class Vdf
    {
        public Dictionary<string,string> Values { get; } = new(StringComparer.OrdinalIgnoreCase);
        public Dictionary<string,Vdf> Children { get; } = new(StringComparer.OrdinalIgnoreCase);
        public string Value(string key) => Values.GetValueOrDefault(key, "");
    }
    internal static Vdf Parse(string text)
    {
        int at=0;
        string? Next() {
            while(at<text.Length) {
                if(char.IsWhiteSpace(text[at])) { at++; continue; }
                if(text[at]=='/' && at+1<text.Length && text[at+1]=='/') { while(at<text.Length && text[at]!='\n') at++; continue; }
                break;
            }
            if(at==text.Length) return null;
            char first=text[at++]; if(first is '{' or '}') return first.ToString();
            var value=new System.Text.StringBuilder();
            if(first=='"') {
                while(at<text.Length) {
                    char c=text[at++]; if(c=='"') return value.ToString();
                    if(c=='\\' && at<text.Length && text[at] is '\\' or '"') c=text[at++];
                    value.Append(c);
                }
                throw new InvalidDataException("Unterminated Steam library value.");
            }
            value.Append(first);
            while(at<text.Length && !char.IsWhiteSpace(text[at]) && text[at] is not ('{' or '}')) value.Append(text[at++]);
            return value.ToString();
        }
        Vdf Read(int depth,bool nested) {
            if(depth>32) throw new InvalidDataException("Steam library nesting is too deep.");
            var result=new Vdf();
            while(Next() is {} key) {
                if(key=="}") { if(!nested) throw new InvalidDataException("Unexpected Steam library brace."); return result; }
                if(key=="{") throw new InvalidDataException("Missing Steam library key.");
                string value=Next() ?? throw new InvalidDataException("Missing Steam library value.");
                if(value=="{") result.Children[key]=Read(depth+1,true);
                else if(value=="}") throw new InvalidDataException("Missing Steam library value.");
                else result.Values[key]=value;
            }
            if(nested) throw new InvalidDataException("Incomplete Steam library.");
            return result;
        }
        return Read(0,false);
    }
    public static bool ValidId(string id) => id.Length is >0 and <=10 && id.All(char.IsAsciiDigit) && uint.TryParse(id,out uint n) && n>0;
    public static bool Inside(string path,string directory) => Path.GetFullPath(path).StartsWith(Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory))+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase);
    public static string? Root() {
        using var key=Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        return key?.GetValue("SteamPath") as string;
    }
    public static Discovery Discover(string? steamRoot=null)
    {
        var games=new Dictionary<string,Game>(); var errors=new List<string>();
        steamRoot ??= Root();
        if(string.IsNullOrWhiteSpace(steamRoot) || !System.IO.Directory.Exists(steamRoot)) { Debug.WriteLine("[Steam] Discover: Steam is not installed"); return new([],[],false); }
        var libraries=new HashSet<string>(StringComparer.OrdinalIgnoreCase){Path.GetFullPath(steamRoot)};
        try {
            string file=Path.Combine(steamRoot,"steamapps","libraryfolders.vdf");
            if(File.Exists(file)) {
                var root=Parse(File.ReadAllText(file)); var folders=root.Children.GetValueOrDefault("libraryfolders");
                if(folders!=null) {
                    foreach(var item in folders.Children.Values) if(Path.IsPathFullyQualified(item.Value("path"))) libraries.Add(Path.GetFullPath(item.Value("path")));
                    foreach(var item in folders.Values) if(item.Key.All(char.IsAsciiDigit) && Path.IsPathFullyQualified(item.Value)) libraries.Add(Path.GetFullPath(item.Value));
                }
            }
        } catch(Exception ex) when(ex is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException) { errors.Add(ex.Message); }
        foreach(string library in libraries) {
            try {
                string steamapps=Path.Combine(library,"steamapps"); if(!System.IO.Directory.Exists(steamapps)) continue;
                foreach(string file in System.IO.Directory.EnumerateFiles(steamapps,"appmanifest_*.acf")) {
                    try {
                        var state=Parse(File.ReadAllText(file)).Children.GetValueOrDefault("AppState"); if(state==null) continue;
                        string id=state.Value("appid"), title=state.Value("name"), install=state.Value("installdir");
                        if(!ValidId(id) || id is "228980" or "250820" || string.IsNullOrWhiteSpace(title) || string.IsNullOrWhiteSpace(install) || Path.IsPathRooted(install)) continue;
                        if(!Path.GetFileNameWithoutExtension(file).Equals("appmanifest_"+id,StringComparison.OrdinalIgnoreCase)) continue;
                        string common=Path.Combine(steamapps,"common"), directory=Path.GetFullPath(Path.Combine(common,install));
                        if(!Inside(directory,common) || !System.IO.Directory.Exists(directory)) continue;
                        // Installed flag; exclude manifests for pending downloads/uninstalls.
                        if(!uint.TryParse(state.Value("StateFlags"),out uint flags) || (flags&4)==0) continue;
                        games.TryAdd(id,new(id,title,directory));
                    } catch(Exception ex) when(ex is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException) { errors.Add(Path.GetFileName(file)+": "+ex.Message); }
                }
            } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { errors.Add(ex.Message); }
        }
        return new(games.Values.OrderBy(g=>g.Title,StringComparer.CurrentCultureIgnoreCase).ToArray(),errors);
    }
    private static string BindingFile(string root,string id) {
        if(!ValidId(id)) throw new InvalidDataException(Loc.Get("SteamInvalidId"));
        return Path.Combine(root,"steam",id+".json");
    }
    public static Binding? LoadBinding(string root,Game game) {
        try {
            var binding=JsonSerializer.Deserialize<Binding>(File.ReadAllText(BindingFile(root,game.AppId)));
            return binding!=null && File.Exists(binding.Executable) && Inside(binding.Executable,game.Directory)?binding:null;
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { return null; }
    }
    public static void SaveBinding(string root,Game game,Binding binding) {
        ArgumentNullException.ThrowIfNull(game);
        ArgumentNullException.ThrowIfNull(binding);
        if(!File.Exists(binding.Executable) || !Inside(binding.Executable,game.Directory)) throw new InvalidDataException(Loc.Get("SteamExecutableInvalid"));
        // A launcher, installer or crash reporter is never recorded as the game.
        if(IsLauncher(binding.Executable)) throw new InvalidDataException(Loc.Get("SteamExecutableLauncher"));
        ProfileStore.AtomicWrite(BindingFile(root,game.AppId),JsonSerializer.Serialize(binding));
    }
    // Before a game's real executable is known its choices live in memory under this
    // placeholder; nothing is saved for it (see RemovePlaceholderProfiles).
    public static string ProfileKey(string root,Game game) => LoadBinding(root,game)?.Executable ?? PlaceholderKey(root,game.AppId);
    internal static string PlaceholderKey(string root,string appId) => Path.Combine(root,"steam","profiles",appId+".exe");
    public static bool IsPlaceholder(string root,string executable) =>
        !string.IsNullOrEmpty(executable) && Inside(executable,Path.Combine(root,"steam","profiles"));

    // Earlier builds saved profiles for the placeholder paths of unbound Steam games, which
    // then counted as set-up games. Deletes them; returns how many.
    public static int RemovePlaceholderProfiles(ProfileStore store) {
        ArgumentNullException.ThrowIfNull(store);
        int removed=0;
        foreach(string file in store.SavedProfileFiles()) {
            try {
                var profile=JsonSerializer.Deserialize<Profile>(File.ReadAllText(file));
                if(profile==null || !IsPlaceholder(store.Root,profile.ExecutablePath)) continue;
                File.Delete(file); removed++;
            } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine("[Steam] "+ex.Message); }
        }
        if(removed>0) Debug.WriteLine($"[Steam] removed {removed} placeholder profile(s)");
        return removed;
    }

    // Executables that are not the game itself: launchers, installers, crash reporters,
    // redistributables, uninstallers and updaters.
    private static readonly string[] LauncherWords=["launcher","setup","crash","redist","unins","updater","installer","start_protected_game"];
    public static bool IsLauncher(string executable) {
        string name=Path.GetFileNameWithoutExtension(executable ?? "");
        return LauncherWords.Any(w=>name.Contains(w,StringComparison.OrdinalIgnoreCase));
    }
    public static void ImportBindings(ProfileStore store,IReadOnlyList<Game> games,IReadOnlyList<GameHistory.Entry> history) {
        var candidates=history.Select(h=>h.Executable).ToList();
        foreach(string file in store.SavedProfileFiles()) {
            try {
                var profile=JsonSerializer.Deserialize<Profile>(File.ReadAllText(file));
                if(profile!=null && Path.IsPathFullyQualified(profile.ExecutablePath) && file.Equals(store.FileFor(profile.ExecutablePath),StringComparison.OrdinalIgnoreCase)) candidates.Add(profile.ExecutablePath);
            } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine(ex.Message); }
        }
        foreach(var game in games) {
            if(LoadBinding(store.Root,game)!=null) continue;
            string? executable=candidates.FirstOrDefault(p=>File.Exists(p) && Inside(p,game.Directory) && !IsLauncher(p) && !IsPlaceholder(store.Root,p));
            if(executable!=null) SaveBinding(store.Root,game,new(executable));
        }
    }
    // The game's running window: the bound executable if it runs, else the program in the
    // game's folder with the largest visible window that is not a launcher, installer or
    // crash reporter. Null when there is none.
    public static RunningApp? Running(Game game,IReadOnlyList<RunningApp> apps,string? bound=null) {
        ArgumentNullException.ThrowIfNull(game);
        ArgumentNullException.ThrowIfNull(apps);
        var inside=apps.Where(a=>a.CanAttach && Inside(a.FullPath,game.Directory)).ToList();
        if(bound!=null && inside.FirstOrDefault(a=>a.FullPath.Equals(bound,StringComparison.OrdinalIgnoreCase)) is {} boundApp) return boundApp;
        return inside.Where(a=>!IsLauncher(a.FullPath)).OrderByDescending(a=>a.Windows.Max(w=>RunningApps.WindowArea(w.Handle))).FirstOrDefault();
    }
    public static void Launch(string appId) {
        if(!ValidId(appId)) throw new InvalidDataException(Loc.Get("SteamInvalidId"));
        Process.Start(new ProcessStartInfo("steam://rungameid/"+appId){UseShellExecute=true})?.Dispose();
    }
    // A program that closes within a few seconds (a DRM stub, a splash or updater) is not the game.
    internal const int StableMilliseconds=3000;
    public static async Task<RunningApp> WaitForWindow(Game game,string? bound,CancellationToken token) {
        ArgumentNullException.ThrowIfNull(game);
        Debug.WriteLine($"[Steam] WaitForWindow enter: {game.Title}, bound {bound ?? "none"}");
        long deadline=Environment.TickCount64+120000;
        var firstSeen=new Dictionary<int,long>();
        while(Environment.TickCount64<deadline) {
            token.ThrowIfCancellationRequested();
            var apps=RunningApps.List(false);
            foreach(int gone in firstSeen.Keys.Where(pid=>!apps.Any(a=>a.Pid==pid)).ToArray()) firstSeen.Remove(gone);
            if(Running(game,apps,bound) is {} app) {
                long now=Environment.TickCount64;
                if(!firstSeen.TryGetValue(app.Pid,out long seen)) firstSeen[app.Pid]=seen=now;
                if(now-seen>=StableMilliseconds) { Debug.WriteLine($"[Steam] WaitForWindow exit: {app.FullPath} pid {app.Pid}"); return app; }
            }
            await Task.Delay(300,token);
        }
        throw new InvalidOperationException(Loc.Get("SteamWindowTimeout"));
    }
}
