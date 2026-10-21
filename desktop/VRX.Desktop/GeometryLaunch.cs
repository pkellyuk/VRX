using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace VRX.Desktop;

// Optional geometry launch coordinator. Selection is passive; only an explicit
// restart action can close a game. Auto-attach never restarts a foreground app.
public static class GeometryLaunch
{
    public sealed record InstalledFile(string Path, string SHA256);
    internal sealed record LiveTuningState(int Version,int Pid,float Separation,float Convergence,ulong Revision,bool Active);
    internal enum TuningUpdate { NextLaunch, RestartRequired, Pending, Applied }
    public sealed record Run(int Pid, long StartTicks, string Executable, string Session, string Stereo = "mirror", string Backend = "", string LaunchExecutable = "");
    public sealed record Plan(RunningApp App, long StartTicks, string Backend, string LayerDirectory,
        string[] Arguments, string WorkingDirectory, string SteamAppId, string Session, string? DxvkDirectory, string[] OwnedRuns, string? Vkd3dDirectory = null);
    private static string RunsRoot(string dataRoot) => Path.Combine(dataRoot,"geometry","runs");
    private static string Hash(string path) { using var f=File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(f)); }
    private sealed class FileGate : IDisposable {
        private readonly Mutex mutex;
        public FileGate(string executable) {
            mutex=new Mutex(false,@"Local\VRX.xgstereo.files."+StereoPaths.Session(executable));
            try {
                bool held; try { held=mutex.WaitOne(5000); } catch(AbandonedMutexException) { held=true; }
                if(!held) throw new IOException(Loc.Get("ErrorGeometryFilesBusy"));
            } catch { mutex.Dispose(); throw; }
        }
        public void Dispose() { mutex.ReleaseMutex(); mutex.Dispose(); }
    }
    private static bool Same(string a,string b) => string.Equals(Path.GetFullPath(a),Path.GetFullPath(b),StringComparison.OrdinalIgnoreCase);
    private static IEnumerable<string> RunFolders(string dataRoot,bool external=true) {
        string own=RunsRoot(dataRoot);
        if (Directory.Exists(own)) foreach(var d in Directory.GetDirectories(own)) yield return d;
#if DEBUG
        // Runs started by the bench scripts of a development checkout.
        if(!external) yield break;
        string repository;
        try { repository=EngineSession.RepositoryRoot(); } catch(DirectoryNotFoundException) { yield break; }
        string legacy=Path.Combine(repository,"bench","native","xgstereo","out");
        if (Directory.Exists(legacy)) foreach(var d in Directory.GetDirectories(legacy,"dx11-*")) yield return d;
        string native=Path.Combine(repository,"bench","native","dx12stereo","out");
        if (Directory.Exists(native)) foreach(var d in Directory.GetDirectories(native).Where(d => Path.GetFileName(d).StartsWith("native-") || Path.GetFileName(d).StartsWith("vkd3d-"))) yield return d;
#endif
    }
    private static Run? ReadRun(string folder) {
        try {
            var run = JsonSerializer.Deserialize<Run>(File.ReadAllText(Path.Combine(folder,"run.json")));
            if (run is { Backend: "" } && Path.GetFileName(folder).StartsWith("vkd3d-",StringComparison.OrdinalIgnoreCase)) run = run with { Backend = StereoPaths.Vkd3d };
            return run;
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }
    public static bool IsRunning(Run run) {
        try { using var p=Process.GetProcessById(run.Pid); return !p.HasExited && p.StartTime.ToUniversalTime().Ticks==run.StartTicks && Same(RunningApps.ProcessPath(run.Pid),string.IsNullOrEmpty(run.LaunchExecutable)?run.Executable:run.LaunchExecutable); }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { return false; }
    }
    internal static TuningUpdate UpdateTuning(Profile profile,string dataRoot,bool apply=true)
    {
        if(!profile.GeometryStereo) return TuningUpdate.NextLaunch;
        foreach(string folder in RunFolders(dataRoot)) {
            var run=ReadRun(folder);
            if(run==null || !Same(run.Executable,profile.ExecutablePath) || run.Session!=profile.GeometrySession || !IsRunning(run)) continue;
            string tuning=Path.Combine(folder,"stereo.txt");
            try {
                var state=JsonSerializer.Deserialize<LiveTuningState>(File.ReadAllText(tuning+".live.json"));
                if(state is not {Version:1,Active:true} || state.Pid!=run.Pid) return TuningUpdate.RestartRequired;
                string value=FormattableString.Invariant($"{profile.GeometrySeparation:R} {profile.GeometryConvergence:R}\n");
                if(apply && (!File.Exists(tuning) || File.ReadAllText(tuning)!=value)) ProfileStore.AtomicWrite(tuning,value);
                return state.Separation==profile.GeometrySeparation && state.Convergence==profile.GeometryConvergence?TuningUpdate.Applied:TuningUpdate.Pending;
            } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException) { Debug.WriteLine(ex.Message); return TuningUpdate.RestartRequired; }
        }
        return TuningUpdate.NextLaunch;
    }
    public static bool HeaderMatches(ReadOnlySpan<byte> bytes, int pid, long now) {
        if (bytes.Length!=176) return false;
        static uint U(ReadOnlySpan<byte> b,int at)=>System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(b.Slice(at,4));
        // Keep this parser independent of native struct packing; gpu_transport.h v1.
        if (U(bytes,0)!=0x47534758 || U(bytes,4)!=1 || U(bytes,8)!=176 || U(bytes,12)!=1 || U(bytes,16)!=(uint)pid || System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(bytes.Slice(40,8))==0 || U(bytes,48)!=2 ||
            U(bytes,20)<2 || U(bytes,20)>4096 || U(bytes,24)<2 || U(bytes,24)>4096 || U(bytes,28)!=28) return false;
        for(int i=0;i<3;++i) {
            var slot=bytes.Slice(56+i*40,40);
            ulong seq=System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(slot);
            ulong left=System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(slot[8..]);
            ulong right=System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(slot[16..]);
            ulong time=System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(slot[24..]);
            if(seq!=0 && left!=0 && left==right && time<=(ulong)now && (ulong)now-time<=1000) return true;
        }
        return false;
    }
    public static bool StreamMatches(string session,int pid) {
        if(!Profile.ValidGeometrySession(session)) return false;
        bool held=false;
        try {
            using var mutex=Mutex.OpenExisting($@"Local\VRX.xgstereo.{session}.gpu.lock");
            try { held=mutex.WaitOne(0); }
            catch(AbandonedMutexException) { mutex.ReleaseMutex(); return false; }
            if(!held) return false;
            try {
                using var map=MemoryMappedFile.OpenExisting($@"Local\VRX.xgstereo.{session}.gpu.frames",MemoryMappedFileRights.Read);
                using var view=map.CreateViewAccessor(0,176,MemoryMappedFileAccess.Read);
                byte[] bytes=new byte[176]; view.ReadArray(0,bytes,0,bytes.Length);
                return HeaderMatches(bytes,pid,Environment.TickCount64);
            } finally { mutex.ReleaseMutex(); }
        } catch(Exception ex) when(ex is IOException or WaitHandleCannotBeOpenedException or UnauthorizedAccessException or ArgumentException) { return false; }
    }
    public static string? ExistingSession(RunningApp app,Profile profile,string dataRoot) {
        var candidates=new List<string>{profile.GeometrySession,StereoPaths.Session(app.FullPath)};
        foreach(var folder in RunFolders(dataRoot)) {
            var run=ReadRun(folder);
            if(run!=null && run.Stereo=="mirror" && run.Pid==app.Pid && Same(run.Executable,app.FullPath) && IsRunning(run)) {
                // A ready DXVK stream must not silently satisfy a native Vulkan choice.
                string backend=string.IsNullOrEmpty(run.Backend) && Path.GetFileName(folder).StartsWith("dx11-",StringComparison.OrdinalIgnoreCase) ? StereoPaths.Dxvk : run.Backend;
                if(StereoPaths.Valid(backend) && backend!=profile.EffectiveStereoBackend) return null;
                candidates.Add(run.Session);
            }
        }
        return candidates.Distinct(StringComparer.Ordinal).FirstOrDefault(s=>StreamMatches(s,app.Pid)
#if DEBUG
            && (profile.EffectiveStereoBackend!=StereoPaths.Dx12 || NativeStereoStatus.Read(s,app.Pid)?.Ready==true)
#endif
            );
    }
    public static string[] ParseArguments(string text) {
        if(string.IsNullOrWhiteSpace(text)) return [];
        nint block=CommandLineToArgvW("vrx.exe "+text,out int count);
        if(block==0) throw new InvalidDataException(Loc.Get("ErrorLaunchArguments"));
        try { var result=new string[count-1]; for(int i=1;i<count;++i) result[i-1]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(block,i*nint.Size))!; return result; }
        finally { LocalFree(block); }
    }
    public static string QuoteArgument(string arg) {
        if(arg.Length>0 && !arg.Any(c=>char.IsWhiteSpace(c)||c=='"')) return arg;
        var result=new StringBuilder("\""); int slashes=0;
        foreach(char c in arg) {
            if(c=='\\') { ++slashes; continue; }
            result.Append('\\',c=='"'?slashes*2+1:slashes); result.Append(c); slashes=0;
        }
        result.Append('\\',slashes*2).Append('"'); return result.ToString();
    }
    public static string[] ApiArguments(IEnumerable<string> args,string backend,bool unreal) {
        if(!unreal) return args.ToArray();
        var result=args.Where(a=>!new[]{"-vulkan","-dx11","-d3d11","-dx12","-d3d12"}.Contains(a,StringComparer.OrdinalIgnoreCase)).ToList();
        // Unreal's documented switches are also the path verified with System Shock.
        // Other engines keep their command line; the review dialog can edit it.
        if(unreal) result.Add(backend switch {StereoPaths.Vulkan=>"-vulkan",StereoPaths.Dx12 or StereoPaths.Vkd3d=>"-dx12",_=>"-dx11"});
        return result.ToArray();
    }
    internal static async Task<string[]> ProcessArguments(int pid,CancellationToken token) {
        string powershell=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell","v1.0","powershell.exe");
        var start=new ProcessStartInfo(powershell){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,StandardOutputEncoding=Encoding.UTF8};
        foreach(string arg in new[]{"-NoProfile","-NonInteractive","-Command",$"[Console]::OutputEncoding=[Text.Encoding]::UTF8; (Get-CimInstance Win32_Process -Filter 'ProcessId={pid.ToString(CultureInfo.InvariantCulture)}').CommandLine"}) start.ArgumentList.Add(arg);
        using var p=Process.Start(start) ?? throw new InvalidOperationException(Loc.Get("ErrorLaunchArguments"));
        var output=p.StandardOutput.ReadToEndAsync(token); var error=p.StandardError.ReadToEndAsync(token);
        try { await p.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(10),token); }
        catch { if(!p.HasExited) p.Kill(); throw; }
        string command=(await output).Trim(); await error;
        if(p.ExitCode!=0 || command.Length==0) throw new InvalidOperationException(Loc.Get("ErrorLaunchArguments"));
        // ParseArguments treats the dummy executable as argv[0]; here discard the
        // actual argv[0] too, retaining every literal game argument.
        return ParseArguments(command).Skip(1).ToArray();
    }
    public static string FindSteamAppId(string executable) {
        for(var dir=new DirectoryInfo(Path.GetDirectoryName(executable)!);dir?.Parent!=null;dir=dir.Parent) {
            if(!dir.Parent.Name.Equals("common",StringComparison.OrdinalIgnoreCase) || dir.Parent.Parent is not {} steamapps) continue;
            foreach(var manifest in steamapps.GetFiles("appmanifest_*.acf")) {
                string text=File.ReadAllText(manifest.FullName);
                var install=Regex.Match(text,"\"installdir\"\\s+\"([^\"]+)\"",RegexOptions.IgnoreCase);
                var id=Regex.Match(text,"\"appid\"\\s+\"([0-9]+)\"",RegexOptions.IgnoreCase);
                if(install.Success && id.Success && install.Groups[1].Value.Equals(dir.Name,StringComparison.OrdinalIgnoreCase)) return id.Groups[1].Value;
            }
        }
        return "";
    }
    public static bool IsX64(string path) {
        using var f=File.OpenRead(path); using var r=new BinaryReader(f);
        if(f.Length<64 || r.ReadUInt16()!=0x5A4D) return false;
        f.Position=60; uint offset=r.ReadUInt32(); if(offset>f.Length-6) return false;
        f.Position=offset; return r.ReadUInt32()==0x00004550 && r.ReadUInt16()==0x8664;
    }
#if DEBUG
    // The native DX12 proxy (d3d12.dll + MinHook) is a development experiment: never in a release.
    private static string NativeDirectory() {
        string root=EngineSession.RepositoryRoot();
        foreach(string folder in new[]{Path.Combine(root,"geometry","native-dx12"),Path.Combine(root,"bench","native","dx12stereo","out","proxy","Release")})
            if(File.Exists(Path.Combine(folder,"d3d12.dll")) && IsX64(Path.Combine(folder,"d3d12.dll"))) return folder;
        throw new FileNotFoundException(Loc.Get("ErrorGeometryNativeMissing"));
    }
#endif
    internal static string Vkd3dFile(string folder,string name) => GeometryRuntime.Vkd3dFile(folder,name);
    // The DLL names a backend copies into the game's folder.
    internal static string[] InstalledNames(string backend) => backend switch {
        StereoPaths.Vkd3d => ["d3d12.dll","d3d12core.dll","d3d11.dll","dxgi.dll"],
#if DEBUG
        StereoPaths.Dx12 => ["d3d12.dll"],
#endif
        "" or StereoPaths.Dxvk => ["d3d11.dll","dxgi.dll"],
        _ => []
    };
    private static InstalledFile[] Files(string folder,Run run) {
        var files=JsonSerializer.Deserialize<InstalledFile[]>(File.ReadAllText(Path.Combine(folder,"installed.json"))) ?? [];
        string parent=Path.GetDirectoryName(Path.GetFullPath(run.Executable))!;
        var names=InstalledNames(run.Backend);
        if(files.Any(f=>!Same(Path.GetDirectoryName(f.Path)!,parent) || !names.Contains(Path.GetFileName(f.Path),StringComparer.OrdinalIgnoreCase))) throw new InvalidDataException(Loc.Get("ErrorGeometryManifest"));
        return files;
    }
    public static void Cleanup(string folder) {
        var run=ReadRun(folder); if(run==null || IsRunning(run)) return;
        try {
            using var gate=new FileGate(run.Executable);
            if(IsRunning(run)) return;
            // A later launch of the same executable can still have these files loaded.
            foreach(var p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(run.Executable))) {
                using(p) { if(Same(RunningApps.ProcessPath(p.Id),run.Executable)) return; }
            }
            foreach(var file in Files(folder,run)) if(File.Exists(file.Path) && Hash(file.Path).Equals(file.SHA256,StringComparison.OrdinalIgnoreCase)) File.Delete(file.Path);
            // Retire ownership permanently. A delayed observer of an older run
            // must not remove identical DLLs installed by the next launch.
            ProfileStore.AtomicWrite(Path.Combine(folder,"installed.json"),"[]");
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Debug.WriteLine("[Geometry cleanup] "+ex.Message); }
    }
    public static void Recover(string dataRoot) {
        foreach(var folder in RunFolders(dataRoot)) Cleanup(folder);
    }
    internal static void RecoverOwned(string dataRoot) {
        foreach(var folder in RunFolders(dataRoot,false)) Cleanup(folder);
    }
    public static Task<Plan> Prepare(RunningApp app,Profile profile,string dataRoot,Action<string> status,CancellationToken token,string[]? launchArguments=null) =>
        PrepareWithLayer(app,profile,dataRoot,status,token,
#if DEBUG
            profile.EffectiveStereoBackend==StereoPaths.Dx12?NativeDirectory():
#endif
            GeometryRuntime.LayerDirectory(),launchArguments);
    // Unit preflight does not load the DLL; supply a fixture layer directory so
    // standard desktop smoke checks remain independent of optional native builds.
    internal static async Task<Plan> PrepareWithLayer(RunningApp app,Profile profile,string dataRoot,Action<string> status,CancellationToken token,string layer,string[]? launchArguments=null) {
        if(!StereoPaths.Geometry(profile.EffectiveStereoBackend)) throw new InvalidOperationException(Loc.Get("ErrorGeometryNotChosen"));
        if(!IsX64(app.FullPath)) throw new InvalidOperationException(Loc.Get("ErrorGeometryX64"));
#if DEBUG
        if(profile.EffectiveStereoBackend==StereoPaths.Dx12) ValidateNativeRuntime(layer);
#endif
        using var original=app.Pid>0?Process.GetProcessById(app.Pid):null;
        long ticks=original?.StartTime.ToUniversalTime().Ticks ?? 0;
        if(original!=null && !Same(RunningApps.ProcessPath(app.Pid),app.FullPath)) throw new InvalidOperationException(Loc.Get("ErrorProcessChanged"));
        if(original==null && launchArguments==null) throw new InvalidOperationException(Loc.Get("ErrorHistoryEntry"));
        var originalArgs=profile.GeometryLaunchArguments!=null ? ParseArguments(profile.GeometryLaunchArguments) : launchArguments ?? await ProcessArguments(app.Pid,token);
        var args=ApiArguments(originalArgs,profile.EffectiveStereoBackend,Path.GetFileName(app.FullPath).EndsWith("-Shipping.exe",StringComparison.OrdinalIgnoreCase));
        string folder=string.IsNullOrEmpty(profile.GeometryWorkingDirectory)?Path.GetDirectoryName(app.FullPath)!:profile.GeometryWorkingDirectory;
        string steam=string.IsNullOrEmpty(profile.GeometrySteamAppId)?FindSteamAppId(app.FullPath):profile.GeometrySteamAppId;
        // Bundled components only, each DLL checked against its pinned hash; nothing is downloaded.
        string? vkd3d=profile.EffectiveStereoBackend==StereoPaths.Vkd3d?GeometryRuntime.Vkd3dDirectory():null;
        string? dxvk=profile.EffectiveStereoBackend is StereoPaths.Dxvk or StereoPaths.Vkd3d?GeometryRuntime.DxvkDirectory():null;
        if(original==null) Recover(dataRoot);
        string[] owned=original==null?[]:RunFolders(dataRoot).Where(d=>ReadRun(d) is {} run && run.Pid==app.Pid && run.StartTicks==ticks && Same(run.Executable,app.FullPath)).ToArray();
        var plan=new Plan(app,ticks,profile.EffectiveStereoBackend,layer,args,folder,steam,StereoPaths.Session(app.FullPath),dxvk,owned,vkd3d);
        ValidateFiles(plan); return plan;
    }
    private static void ValidateFiles(Plan plan) {
        var owned=new List<InstalledFile>();
        foreach(var folder in plan.OwnedRuns) { var run=ReadRun(folder); if(run!=null) owned.AddRange(Files(folder,run)); }
        foreach(string name in new[]{"d3d11.dll","dxgi.dll","d3d12.dll","d3d12core.dll"}) {
            string file=Path.Combine(Path.GetDirectoryName(plan.App.FullPath)!,name);
            if(File.Exists(file) && !owned.Any(o=>Same(o.Path,file) && Hash(file).Equals(o.SHA256,StringComparison.OrdinalIgnoreCase)))
                throw new IOException(Loc.Format("ErrorGeometryExistingDll",file));
        }
    }
    // verifyPins: production launches check every source DLL against GeometryRuntime's pinned
    // hashes right before copying it (the smoke test installs harmless fixture files).
    internal static List<InstalledFile> InstallFiles(string? sourceFolder,string executable,string runRoot,string backend=StereoPaths.Dxvk,string? vkd3dFolder=null,bool verifyPins=false) {
        using var gate=new FileGate(executable);
        var installed=new List<InstalledFile>();
        try {
            if(sourceFolder!=null) foreach(string name in InstalledNames(backend)) {
                bool translator=backend==StereoPaths.Vkd3d && name is "d3d12.dll" or "d3d12core.dll";
                string target=Path.Combine(Path.GetDirectoryName(executable)!,name), source=translator ? Vkd3dFile(vkd3dFolder ?? throw new InvalidDataException(Loc.Format("ErrorVkd3dMissing",Path.Combine("geometry","vkd3d"))),name) : Path.Combine(sourceFolder,name);
                if(verifyPins && backend!=StereoPaths.Dx12) GeometryRuntime.VerifySource(translator?"vkd3d":"dxvk",name,source);
                string temporary=target+".vrx-"+Guid.NewGuid().ToString("N")+".tmp";
                string hash=Hash(source);
                try { File.Copy(source,temporary,false); if(!Hash(temporary).Equals(hash,StringComparison.Ordinal)) throw new IOException(Loc.Get("ErrorGeometrySourceChanged")); File.Move(temporary,target,false); }
                finally { if(File.Exists(temporary)) File.Delete(temporary); }
                installed.Add(new InstalledFile(target,hash));
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"installed.json"),JsonSerializer.Serialize(installed));
            }
            if(installed.Count==0) ProfileStore.AtomicWrite(Path.Combine(runRoot,"installed.json"),"[]");
            return installed;
        } catch {
            foreach(var file in installed) if(File.Exists(file.Path) && Hash(file.Path).Equals(file.SHA256,StringComparison.Ordinal)) File.Delete(file.Path);
            ProfileStore.AtomicWrite(Path.Combine(runRoot,"installed.json"),"[]");
            throw;
        }
    }
    internal static void ConfigureEnvironment(ProcessStartInfo start,Plan plan,Profile profile,string runRoot) {
        if(profile.GeometryProfileId==TestedStereoProfiles.SystemShock) {
            var compatibility=(start.Environment.TryGetValue("__COMPAT_LAYER",out var previous)?previous ?? "":"")
                .Split(' ',StringSplitOptions.RemoveEmptyEntries)
                .Where(flag=>!new[]{"DPIUNAWARE","GDIDPISCALING"}.Contains(flag,StringComparer.OrdinalIgnoreCase));
            start.Environment["__COMPAT_LAYER"]=string.Join(' ',compatibility.Append("HIGHDPIAWARE").Distinct(StringComparer.OrdinalIgnoreCase));
        }
        start.Environment["VRX_XG_SESSION"]=plan.Session;
        start.Environment.Remove("VRX_DX12_GPU_VALIDATION");
        start.Environment.Remove("VRX_DX12_REPLAY_PROBES");
        start.Environment.Remove("VRX_DX12_DEBUG_LAYER");
        start.Environment.Remove("VRX_DX12_DEBUG_LOG");
        start.Environment.Remove("VRX_DX12_RECORD_ONLY");
        start.Environment.Remove("VRX_DX12_CLONE_ONLY");
        start.Environment.Remove("VRX_DX12_SKIP_COMMANDS");
        start.Environment.Remove("VRX_DX12_SKIP_WORK");
        start.Environment.Remove("VRX_DX12_STATE_ONLY");
        start.Environment.Remove("VRX_DX12_RECORD_COMMANDS");
        if(plan.Backend==StereoPaths.Dx12) {
            start.Environment["VRX_DX12_INTERCEPT"]="1";start.Environment["VRX_DX12_MIRROR"]="1";
            start.Environment["VRX_DX12_SHIFT"]=profile.GeometrySeparation.ToString("R",CultureInfo.InvariantCulture);
            start.Environment["VRX_DX12_CONVERGENCE"]=profile.GeometryConvergence.ToString("R",CultureInfo.InvariantCulture);
            foreach(string key in new[]{"VRX_DX12_TEST_BGRA","VRX_DX12_TEST_MODERN","VRX_DX12_TEST_TEN","VRX_XG_EYE","VRX_XG_TRANSPORT","VRX_XG_TUNING","VRX_XG_LOG"}) start.Environment.Remove(key);
            if(start.Environment.TryGetValue("VK_INSTANCE_LAYERS",out string? inherited))
                start.Environment["VK_INSTANCE_LAYERS"]=string.Join(';',(inherited??"").Split(';',StringSplitOptions.RemoveEmptyEntries).Where(s=>s!="VK_LAYER_VRX_xgstereo"));
        } else {
            start.Environment["VRX_DX12_INTERCEPT"]="0";start.Environment["VRX_DX12_MIRROR"]="0";
            start.Environment["VK_ADD_LAYER_PATH"]=plan.LayerDirectory+(start.Environment.TryGetValue("VK_ADD_LAYER_PATH",out var oldPath)&&!string.IsNullOrEmpty(oldPath)?";"+oldPath:"");
            var layers=(start.Environment.TryGetValue("VK_INSTANCE_LAYERS",out var inheritedLayers)?inheritedLayers??"":"").Split(';',StringSplitOptions.RemoveEmptyEntries).ToList();
            if(!layers.Contains("VK_LAYER_VRX_xgstereo"))layers.Add("VK_LAYER_VRX_xgstereo");
            start.Environment["VK_INSTANCE_LAYERS"]=string.Join(';',layers);
            start.Environment["VRX_XG_EYE"]="mirror";start.Environment["VRX_XG_TRANSPORT"]="gpu";
            start.Environment["VRX_XG_SHIFT"]=profile.GeometrySeparation.ToString("R",CultureInfo.InvariantCulture);
            start.Environment["VRX_XG_TUNING"]=Path.Combine(runRoot,"stereo.txt");start.Environment["VRX_XG_LOG"]=Path.Combine(runRoot,"layer.log");
            start.Environment["DXVK_LOG_PATH"]=runRoot;start.Environment["DXVK_CONFIG_FILE"]=Path.Combine(runRoot,"dxvk.conf");start.Environment["DXVK_STATE_CACHE_PATH"]=runRoot;
        }
        if(plan.Backend==StereoPaths.Vkd3d) {
            string config=start.Environment.TryGetValue("VKD3D_CONFIG",out var inheritedConfig) ? inheritedConfig ?? "" : "";
            start.Environment["VKD3D_CONFIG"]=string.Join(',',config.Split(',',StringSplitOptions.RemoveEmptyEntries).Append("single_queue").Distinct());
            string disabled=start.Environment.TryGetValue("VKD3D_DISABLE_EXTENSIONS",out var inheritedDisabled) ? inheritedDisabled ?? "" : "";
            start.Environment["VKD3D_DISABLE_EXTENSIONS"]=string.Join(',',disabled.Split(',',StringSplitOptions.RemoveEmptyEntries).Concat(new[]{"VK_EXT_descriptor_buffer","VK_EXT_descriptor_heap","VK_NV_low_latency2","VK_EXT_shader_module_identifier","VK_KHR_pipeline_binary","VK_EXT_graphics_pipeline_library"}).Distinct());
            start.Environment["VKD3D_LOG_FILE"]=Path.Combine(runRoot,"vkd3d.log");
#if DEBUG
            // Verbose translator logging is a development aid only.
            start.Environment["VKD3D_DEBUG"]="info";
#endif
            start.Environment["VKD3D_SHADER_CACHE_PATH"]=runRoot;
        }
        if(plan.SteamAppId.Length>0)start.Environment["SteamAppId"]=start.Environment["SteamGameId"]=plan.SteamAppId;
    }
#if DEBUG
    private static void ValidateNativeRuntime(string folder) {
        string file=Path.Combine(folder,"d3d12.dll");
        if(!File.Exists(file) || !IsX64(file))throw new InvalidDataException(Loc.Get("ErrorGeometryNativeMissing"));
    }
#endif
    private static void ValidateRuntime(Plan plan) {
#if DEBUG
        if(plan.Backend==StereoPaths.Dx12){ValidateNativeRuntime(plan.LayerDirectory);return;}
#endif
        if(!StereoPaths.Geometry(plan.Backend) || !File.Exists(Path.Combine(plan.LayerDirectory,GeometryRuntime.LayerLibrary)) || !File.Exists(Path.Combine(plan.LayerDirectory,GeometryRuntime.LayerManifest)))
            throw new FileNotFoundException(Loc.Format("ErrorGeometryLayerMissing",Path.Combine("geometry","layer")));
        if((plan.Backend is StereoPaths.Dxvk or StereoPaths.Vkd3d) && (plan.DxvkDirectory==null || GeometryRuntime.DxvkFiles.Any(n=>!File.Exists(Path.Combine(plan.DxvkDirectory,n)) || !IsX64(Path.Combine(plan.DxvkDirectory,n)))))
            throw new FileNotFoundException(Loc.Format("ErrorGeometryLayerMissing",Path.Combine("geometry","dxvk")));
        if(plan.Backend==StereoPaths.Vkd3d && (plan.Vkd3dDirectory==null || GeometryRuntime.Vkd3dFiles.Any(n=>!File.Exists(Vkd3dFile(plan.Vkd3dDirectory,n)) || !IsX64(Vkd3dFile(plan.Vkd3dDirectory,n)))))
            throw new FileNotFoundException(Loc.Format("ErrorVkd3dMissing",Path.Combine("geometry","vkd3d")));
    }
    public static Task<Run> Restart(Plan plan,Profile profile,string dataRoot,Action<string> status,CancellationToken token) =>
        plan.App.Pid>0?Start(plan,profile,dataRoot,status,token):throw new ArgumentException(nameof(plan));
    public static Task<Run> Launch(Plan plan,Profile profile,string dataRoot,Action<string> status,CancellationToken token) =>
        plan.App.Pid==0?Start(plan,profile,dataRoot,status,token):throw new ArgumentException(nameof(plan));
    internal static Task<Run> LaunchFromSteam(Plan plan,Profile profile,string dataRoot,Action<string> status,CancellationToken token,string steamExecutable) =>
        plan.App.Pid==0?Start(plan,profile,dataRoot,status,token,steamExecutable):throw new ArgumentException(nameof(plan));
    private static async Task<Run> Start(Plan plan,Profile profile,string dataRoot,Action<string> status,CancellationToken token,string? steamExecutable=null) {
        // All preflight and the launch-details dialog have completed before this point.
        token.ThrowIfCancellationRequested();
        ValidateRuntime(plan);
        ValidateFiles(plan);
        if(plan.App.Pid>0) {
        var identity=new Run(plan.App.Pid,plan.StartTicks,plan.App.FullPath,plan.Session);
        if(!IsRunning(identity)) throw new InvalidOperationException(Loc.Get("ErrorProcessChanged"));
        using(var game=Process.GetProcessById(plan.App.Pid)) {
            if(!game.CloseMainWindow()) throw new InvalidOperationException(Loc.Get("ErrorGeometryClose"));
            try { await game.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(30),token); }
            catch(TimeoutException) { throw new InvalidOperationException(Loc.Get("ErrorGeometryClose")); }
        }
        } else {
            foreach(var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(plan.App.FullPath))) using(process)
                if(Same(RunningApps.ProcessPath(process.Id),plan.App.FullPath)) throw new InvalidOperationException(Loc.Get("ErrorHistoryAlreadyRunning"));
        }
        foreach(var folder in plan.OwnedRuns) Cleanup(folder);
        string runRoot=Path.Combine(RunsRoot(dataRoot),Guid.NewGuid().ToString("N")); Directory.CreateDirectory(runRoot);
        var installed=new List<InstalledFile>();
        // A zero PID records ownership even when startup fails before a child exists.
        var run=new Run(0,0,plan.App.FullPath,plan.Session,Backend:plan.Backend);
        ProfileStore.AtomicWrite(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
        Process? bootstrap=null;
        try {
            using(new FileGate(plan.App.FullPath)) {
            ValidateFiles(plan);
            installed = InstallFiles(plan.Backend==StereoPaths.Dx12?plan.LayerDirectory:plan.DxvkDirectory,plan.App.FullPath,runRoot,plan.Backend,plan.Vkd3dDirectory,verifyPins:true);
            if(plan.Backend!=StereoPaths.Dx12) {
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"stereo.txt"),FormattableString.Invariant($"{profile.GeometrySeparation:R} {profile.GeometryConvergence:R}\n"));
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"dxvk.conf"),"dxvk.enableGraphicsPipelineLibrary = False\ndxvk.enableDescriptorBuffer = False\ndxvk.disableNvLowLatency2 = True\n");
            }
            var start=new ProcessStartInfo(steamExecutable ?? plan.App.FullPath){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=plan.WorkingDirectory};
            foreach(string arg in plan.Arguments) start.ArgumentList.Add(arg);
            ConfigureEnvironment(start,plan,profile,runRoot);
            token.ThrowIfCancellationRequested();
            var child=Process.Start(start) ?? throw new InvalidOperationException(Loc.Get("ErrorGeometryLaunch"));
            bool launcher=!Same(start.FileName,plan.App.FullPath);
            run=run with {Pid=child.Id,StartTicks=child.StartTime.ToUniversalTime().Ticks,LaunchExecutable=launcher?start.FileName:""};
            ProfileStore.AtomicWrite(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
            if(launcher) bootstrap=child;
            else _=Observe(child,runRoot);
            }
            if(bootstrap!=null) {
                long startupDeadline=Environment.TickCount64+60000;
                Process? game=null;
                while(Environment.TickCount64<startupDeadline && game==null) {
                    token.ThrowIfCancellationRequested();
                    foreach(var candidate in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(plan.App.FullPath))) {
                        if(!candidate.HasExited && Same(RunningApps.ProcessPath(candidate.Id),plan.App.FullPath)) {game=candidate;break;}
                        candidate.Dispose();
                    }
                    if(game==null) await Task.Delay(150,token);
                }
                if(game==null) throw new InvalidOperationException(Loc.Get("SteamWindowTimeout"));
                run=run with {Pid=game.Id,StartTicks=game.StartTime.ToUniversalTime().Ticks,LaunchExecutable=""};
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
                _=Observe(game,runRoot);
                bootstrap.Dispose(); bootstrap=null;
            }
            status(Loc.Get("StatusGeometryWaiting"));
            var deadline=Environment.TickCount64+45000;
            while(Environment.TickCount64<deadline && IsRunning(run)) {
                token.ThrowIfCancellationRequested();
#if DEBUG
                NativeStereoStatus? native=plan.Backend==StereoPaths.Dx12?NativeStereoStatus.Read(run.Session,run.Pid):null;
                if(native!=null && (native.Stops>0 || native.HookFailures>0 || !native.Mirroring)) {
                    ProfileStore.AtomicWrite(Path.Combine(runRoot,"native-status.json"),JsonSerializer.Serialize(native));
                    throw new InvalidOperationException(Loc.Format("ErrorGeometryNativeStopped",native.Reason.Length>0?native.Reason:Loc.Get("ErrorGeometryNativeCoverage"),runRoot));
                }
                if(StreamMatches(run.Session,run.Pid) && (plan.Backend!=StereoPaths.Dx12 || native?.Ready==true)) {
                    if(native!=null)ProfileStore.AtomicWrite(Path.Combine(runRoot,"native-status.json"),JsonSerializer.Serialize(native));
                    return run;
                }
#else
                if(StreamMatches(run.Session,run.Pid)) return run;
#endif
                await Task.Delay(150,token);
            }
            throw new InvalidOperationException(Loc.Format("ErrorGeometryNoStream",runRoot));
        } catch { if(bootstrap!=null) _=Observe(bootstrap,runRoot); if(run.Pid==0) { foreach(var file in installed) if(File.Exists(file.Path)&&Hash(file.Path).Equals(file.SHA256,StringComparison.Ordinal)) File.Delete(file.Path); } throw; }
    }
    private static async Task Observe(Process child,string folder) {
        try { await child.WaitForExitAsync(); Cleanup(folder); }
        catch(Exception ex) { Debug.WriteLine("[Geometry run] "+ex.Message); }
        finally { child.Dispose(); }
    }
    [DllImport("shell32.dll",CharSet=CharSet.Unicode,SetLastError=true)] private static extern nint CommandLineToArgvW(string command,out int count);
    [DllImport("kernel32.dll")] private static extern nint LocalFree(nint memory);
}
