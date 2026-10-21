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
    public sealed record Discovery(IReadOnlyList<Game> Games, IReadOnlyList<string> Errors);
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
        steamRoot ??= Root(); if(string.IsNullOrWhiteSpace(steamRoot)) return new([],[]);
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
        if(!ValidId(id)) throw new InvalidDataException("Invalid Steam App ID.");
        return Path.Combine(root,"steam",id+".json");
    }
    public static Binding? LoadBinding(string root,Game game) {
        try {
            var binding=JsonSerializer.Deserialize<Binding>(File.ReadAllText(BindingFile(root,game.AppId)));
            return binding!=null && File.Exists(binding.Executable) && Inside(binding.Executable,game.Directory)?binding:null;
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { return null; }
    }
    public static void SaveBinding(string root,Game game,Binding binding) {
        if(!File.Exists(binding.Executable) || !Inside(binding.Executable,game.Directory)) throw new InvalidDataException(Loc.Get("SteamExecutableInvalid"));
        ProfileStore.AtomicWrite(BindingFile(root,game.AppId),JsonSerializer.Serialize(binding));
    }
    public static string ProfileKey(string root,Game game) => LoadBinding(root,game)?.Executable ?? Path.Combine(root,"steam","profiles",game.AppId+".exe");
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
            string? executable=candidates.FirstOrDefault(p=>File.Exists(p) && Inside(p,game.Directory));
            if(executable!=null) SaveBinding(store.Root,game,new(executable));
        }
    }
    public static RunningApp? Running(Game game,IReadOnlyList<RunningApp> apps) => apps.FirstOrDefault(a=>a.CanAttach && Inside(a.FullPath,game.Directory));
    public static void Launch(string appId) {
        if(!ValidId(appId)) throw new InvalidDataException("Invalid Steam App ID.");
        Process.Start(new ProcessStartInfo("steam://rungameid/"+appId){UseShellExecute=true})?.Dispose();
    }
    public static async Task<RunningApp> WaitForWindow(Game game,CancellationToken token) {
        long deadline=Environment.TickCount64+120000;
        while(Environment.TickCount64<deadline) {
            token.ThrowIfCancellationRequested();
            if(Running(game,RunningApps.List(false)) is {} app) return app;
            await Task.Delay(300,token);
        }
        throw new InvalidOperationException(Loc.Get("SteamWindowTimeout"));
    }
}
