using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace VRX.Desktop;

// Geometry viewers belong to a particular game process. Keep their lifetime tied
// to that process, and recover a recorded orphan without disturbing another UI.
internal static class GeometrySessionLifecycle
{
    internal sealed record Lease(GeometryLaunch.Run Viewer, GeometryLaunch.Run Game,
        GeometryLaunch.Run Ui, string Control);

    private static GeometryLaunch.Run Identity(Process process,string session="") =>
        new(process.Id,process.StartTime.ToUniversalTime().Ticks,RunningApps.ProcessPath(process.Id),session);
    private static bool Same(string a,string b) => string.Equals(Path.GetFullPath(a),Path.GetFullPath(b),StringComparison.OrdinalIgnoreCase);
    internal static bool IsEngine(string path,string directory) => Same(Path.GetDirectoryName(path)!,directory) &&
        (Path.GetFileNameWithoutExtension(path) is "xrplayer" or "xrapp5" ||
         Path.GetFileNameWithoutExtension(path).StartsWith("xrplayer.",StringComparison.OrdinalIgnoreCase) ||
         Path.GetFileNameWithoutExtension(path).StartsWith("xrapp5.",StringComparison.OrdinalIgnoreCase));

    public static void Record(string folder,Process viewer,RunningApp game,string session,string control)
    {
        try {
            using var source=Process.GetProcessById(game.Pid);
            using var ui=Process.GetCurrentProcess();
            var lease=new Lease(Identity(viewer,session),Identity(source,session),Identity(ui),control);
            ProfileStore.AtomicWrite(Path.Combine(folder,"geometry-viewer.json"),JsonSerializer.Serialize(lease));
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception) {
            Debug.WriteLine("[Geometry owner] "+ex.Message);
        }
    }

    // Retain the latest recenter/menu counters and all visual settings. Never
    // rebuild a stop snapshot from a possibly stale desktop profile.
    internal static bool RequestStop(string control)
    {
        var fields=File.ReadAllText(control).Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries);
        if(fields.Length!=31 || fields[0]!="VRX" || fields[1]!="11" ||
            fields.Skip(2).Any(f=>!double.TryParse(f,System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out double v) || !double.IsFinite(v))) return false;
        fields[14]="1";
        ProfileStore.AtomicWrite(control,string.Join(' ',fields)+"\n");
        return true;
    }

    internal static async Task WatchGame(Process viewer,int gamePid,string control,long expectedStartTicks=0)
    {
        try {
            Process? source=null;
            try {
                source=Process.GetProcessById(gamePid); _=source.Handle;
                if(expectedStartTicks!=0 && source.StartTime.ToUniversalTime().Ticks!=expectedStartTicks) { source.Dispose(); source=null; }
            }
            catch(Exception ex) when(ex is ArgumentException or InvalidOperationException) { source?.Dispose(); source=null; }
            using var game=source;
            // Hold the original process handle so a reused PID cannot extend a
            // previous game's headset session.
            while(!viewer.HasExited && game!=null && !game.HasExited) await Task.Delay(200);
            if(viewer.HasExited) return;
            if(!RequestStop(control)) return;
            try { await viewer.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(8)); }
            catch(TimeoutException) { viewer.Kill(); await viewer.WaitForExitAsync(); }
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception) {
            Debug.WriteLine("[Geometry lifetime] "+ex.Message);
        }
    }

    private static IEnumerable<Lease> Recorded(string dataRoot)
    {
        string sessions=Path.Combine(dataRoot,"sessions");
        if(!Directory.Exists(sessions)) yield break;
        foreach(string folder in Directory.GetDirectories(sessions)) {
            Lease? lease=null;
            try {
                string file=Path.Combine(folder,"geometry-viewer.json");
                if(File.Exists(file)) {
                    var saved=JsonSerializer.Deserialize<Lease>(File.ReadAllText(file));
                    if(saved!=null && Same(saved.Control,Path.Combine(folder,"control.txt"))) lease=saved;
                }
            } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine(ex.Message); }
            if(lease!=null) yield return lease;
        }
    }

#if DEBUG
    // The earlier command-line trials predate managed session records. Only the
    // explicit current-headset record, its game run and exact viewer arguments
    // qualify for migration; a process name alone never authorizes stopping it.
    private sealed record Legacy(int Pid,int Producer,string Root);
    private static Lease? Trial(string repository)
    {
        try {
            string output=Path.Combine(repository,"bench","native","dx12stereo","out");
            string file=Path.Combine(output,"current-headset.json");
            if(!File.Exists(file)) return null;
            var legacy=JsonSerializer.Deserialize<Legacy>(File.ReadAllText(file));
            if(legacy==null || !Same(Path.GetDirectoryName(Path.GetFullPath(legacy.Root))!,output)) return null;
            var game=JsonSerializer.Deserialize<GeometryLaunch.Run>(File.ReadAllText(Path.Combine(legacy.Root,"run.json")));
            if(game==null || game.Pid!=legacy.Producer || !Profile.ValidGeometrySession(game.Session)) return null;
            using var viewer=Process.GetProcessById(legacy.Pid);
            return new Lease(Identity(viewer,game.Session),game,new(0,0,"",""),Path.Combine(legacy.Root,"control.txt"));
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { return null; }
    }

#endif

    public static async Task ReleasePrevious(RunningApp app,string dataRoot,CancellationToken token,
        string? engineDirectory=null,bool includeTrial=true)
    {
        engineDirectory ??=Path.GetDirectoryName(EngineSession.EnginePath())!;
        var leases=Recorded(dataRoot).ToList();
#if DEBUG
        // The bench trial record exists only in a development checkout.
        if(includeTrial && Trial(EngineSession.RepositoryRoot()) is {} trial) leases.Add(trial);
#endif
        foreach(var lease in leases.DistinctBy(l=>l.Viewer.Pid)) {
            token.ThrowIfCancellationRequested();
            if(!Same(lease.Game.Executable,app.FullPath) || GeometryLaunch.IsRunning(lease.Ui)) continue;
            try {
                using var viewer=Process.GetProcessById(lease.Viewer.Pid);
                if(viewer.StartTime.ToUniversalTime().Ticks!=lease.Viewer.StartTicks ||
                    !IsEngine(RunningApps.ProcessPath(viewer.Id),engineDirectory)) continue;
                var args=await GeometryLaunch.ProcessArguments(viewer.Id,token);
                string? Value(string key) {
                    var found=args.Where(a=>a.StartsWith(key,StringComparison.Ordinal)).ToArray();
                    return found.Length==1?found[0][key.Length..]:null;
                }
                if(Value("--geometry-session=")!=lease.Game.Session || Value("--geometry-pid=")!=lease.Game.Pid.ToString(System.Globalization.CultureInfo.InvariantCulture) ||
                    Value("--control=") is not {} control || !Same(control,lease.Control)) continue;
                if(!RequestStop(control)) continue;
                // Only a cooperative stop is permitted for an unowned viewer.
                await viewer.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(8),token);
            } catch(TimeoutException) { throw new InvalidOperationException(Loc.Get("ErrorPreviousGeometryStop")); }
            catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception) { Debug.WriteLine("[Geometry recovery] "+ex.Message); }
        }
    }
}
