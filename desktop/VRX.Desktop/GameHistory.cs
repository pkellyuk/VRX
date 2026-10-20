using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace VRX.Desktop;

public sealed class GameHistory(string root)
{
    public sealed record Entry(string Executable,string Title,string[] Arguments,string WorkingDirectory,string SteamAppId,DateTimeOffset LastPlayedUtc);
    private string FileName=>Path.Combine(root,"recent-games.json");
    private static bool Same(string a,string b)=>string.Equals(Path.GetFullPath(a),Path.GetFullPath(b),StringComparison.OrdinalIgnoreCase);
    private static bool Valid(Entry e)=>!string.IsNullOrWhiteSpace(e.Executable) && Path.IsPathFullyQualified(e.Executable) &&
        e.Executable.EndsWith(".exe",StringComparison.OrdinalIgnoreCase) && !string.IsNullOrWhiteSpace(e.Title) && e.Arguments!=null && e.Arguments.All(a=>a!=null) &&
        Path.IsPathFullyQualified(e.WorkingDirectory) && e.SteamAppId is {} steam && steam.All(char.IsAsciiDigit);
    public IReadOnlyList<Entry> Load()
    {
        try {
            if(!File.Exists(FileName)) return [];
            return (JsonSerializer.Deserialize<Entry[]>(File.ReadAllText(FileName)) ?? []).Where(e=>e!=null && Valid(e))
                .OrderByDescending(e=>e.LastPlayedUtc).DistinctBy(e=>e.Executable,StringComparer.OrdinalIgnoreCase).Take(24).ToArray();
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine(ex.Message); return []; }
    }
    public void Remember(Entry entry)
    {
        if(!Valid(entry)) throw new InvalidDataException(Loc.Get("ErrorHistoryEntry"));
        var entries=new[]{entry}.Concat(Load().Where(e=>!Same(e.Executable,entry.Executable))).Take(24).ToArray();
        ProfileStore.AtomicWrite(FileName,JsonSerializer.Serialize(entries,new JsonSerializerOptions{WriteIndented=true}));
    }
    public void ImportLastGame(ProfileStore store)
    {
        if(File.Exists(FileName)) return;
        try {
            string last=Path.Combine(root,"last-game.txt"); if(!File.Exists(last)) return;
            string exe=File.ReadAllText(last).Trim(); if(!File.Exists(exe) || !exe.EndsWith(".exe",StringComparison.OrdinalIgnoreCase)) return;
            var profile=store.Load(exe);
            Remember(new(exe,MainWindow.GameDisplayName(profile.PreferredWindowTitle,Path.GetFileName(exe)),GeometryLaunch.ParseArguments(profile.GeometryLaunchArguments ?? ""),
                string.IsNullOrEmpty(profile.GeometryWorkingDirectory)?Path.GetDirectoryName(exe)!:profile.GeometryWorkingDirectory,
                string.IsNullOrEmpty(profile.GeometrySteamAppId)?GeometryLaunch.FindSteamAppId(exe):profile.GeometrySteamAppId,File.GetLastWriteTimeUtc(last)));
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine(ex.Message); }
    }
    public async Task Record(RunningApp app,GameWindow window,Profile profile,CancellationToken token)
    {
        if(ScreenSources.IsScreen(app) || window.Monitor>=0) return;
        var previous=Load().FirstOrDefault(e=>Same(e.Executable,app.FullPath));
        string[] args=previous?.Arguments ?? GeometryLaunch.ParseArguments(profile.GeometryLaunchArguments ?? "");
        try { args=await GeometryLaunch.ProcessArguments(app.Pid,token); }
        catch(Exception ex) when(ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception) { Debug.WriteLine(ex.Message); }
        Remember(new(app.FullPath,MainWindow.GameDisplayName(window.Title,app.Name),args,
            string.IsNullOrEmpty(profile.GeometryWorkingDirectory)?Path.GetDirectoryName(app.FullPath)!:profile.GeometryWorkingDirectory,
            string.IsNullOrEmpty(profile.GeometrySteamAppId)?GeometryLaunch.FindSteamAppId(app.FullPath):profile.GeometrySteamAppId,DateTimeOffset.UtcNow));
    }
    public static void LaunchDepth(Entry entry)
    {
        if(!Valid(entry) || !File.Exists(entry.Executable)) throw new FileNotFoundException(Loc.Get("ErrorHistoryMissing"),entry.Executable);
        if(entry.SteamAppId.Length>0) { Process.Start(new ProcessStartInfo("steam://rungameid/"+entry.SteamAppId){UseShellExecute=true}); return; }
        var start=new ProcessStartInfo(entry.Executable){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=entry.WorkingDirectory};
        foreach(string arg in entry.Arguments) start.ArgumentList.Add(arg);
        Process.Start(start)?.Dispose();
    }
    public static async Task<RunningApp> WaitForWindow(Entry entry,int? pid,CancellationToken token)
    {
        var until=Environment.TickCount64+60000;
        while(Environment.TickCount64<until) {
            token.ThrowIfCancellationRequested();
            var app=RunningApps.List(false).FirstOrDefault(a=>a.CanAttach && Same(a.FullPath,entry.Executable) && (!pid.HasValue || a.Pid==pid));
            if(app!=null) return app;
            await Task.Delay(200,token);
        }
        throw new InvalidOperationException(Loc.Get("ErrorHistoryWindow"));
    }
}
