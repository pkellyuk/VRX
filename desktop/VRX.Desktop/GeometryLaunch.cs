using System.Diagnostics;
using System.Formats.Tar;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.IO.MemoryMappedFiles;
using System.Net.Http;
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
    public sealed record Run(int Pid, long StartTicks, string Executable, string Session, string Stereo = "mirror", string Backend = "");
    public sealed record Plan(RunningApp App, long StartTicks, string Backend, string LayerDirectory,
        string[] Arguments, string WorkingDirectory, string SteamAppId, string Session, string? DxvkDirectory, string[] OwnedRuns);
    private const string DxvkVersion = "2.7.1";
    private const string ArchiveHash = "D85CE7C79F57ECD765AAA1B9E7007CB875E6FDE9F6D331DF799BCE73D513CE87";
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
        if(!external) yield break;
        string legacy=Path.Combine(EngineSession.RepositoryRoot(),"bench","native","xgstereo","out");
        if (Directory.Exists(legacy)) foreach(var d in Directory.GetDirectories(legacy,"dx11-*")) yield return d;
        string native=Path.Combine(EngineSession.RepositoryRoot(),"bench","native","dx12stereo","out");
        if (Directory.Exists(native)) foreach(var d in Directory.GetDirectories(native,"native-*")) yield return d;
    }
    private static Run? ReadRun(string folder) {
        try { return JsonSerializer.Deserialize<Run>(File.ReadAllText(Path.Combine(folder,"run.json"))); }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }
    public static bool IsRunning(Run run) {
        try { using var p=Process.GetProcessById(run.Pid); return !p.HasExited && p.StartTime.ToUniversalTime().Ticks==run.StartTicks && Same(RunningApps.ProcessPath(run.Pid),run.Executable); }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { return false; }
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
            if(run!=null && run.Pid==app.Pid && Same(run.Executable,app.FullPath) && IsRunning(run)) {
                // A ready DXVK stream must not silently satisfy a native Vulkan choice.
                string backend=string.IsNullOrEmpty(run.Backend) && Path.GetFileName(folder).StartsWith("dx11-",StringComparison.OrdinalIgnoreCase) ? StereoPaths.Dxvk : run.Backend;
                if(StereoPaths.Valid(backend) && backend!=profile.EffectiveStereoBackend) return null;
                candidates.Add(run.Session);
            }
        }
        return candidates.Distinct(StringComparer.Ordinal).FirstOrDefault(s=>StreamMatches(s,app.Pid) &&
            (profile.EffectiveStereoBackend!=StereoPaths.Dx12 || NativeStereoStatus.Read(s,app.Pid)?.Ready==true));
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
        if(unreal) result.Add(backend switch {StereoPaths.Vulkan=>"-vulkan",StereoPaths.Dx12=>"-dx12",_=>"-dx11"});
        return result.ToArray();
    }
    private static async Task<string[]> ProcessArguments(int pid,CancellationToken token) {
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
    private static string LayerDirectory() {
        string root=EngineSession.RepositoryRoot();
        foreach(string folder in new[]{Path.Combine(root,"geometry","layer"),Path.Combine(root,"bench","native","xgstereo","out","Release")})
            if(File.Exists(Path.Combine(folder,"vrx_xgstereo.dll")) && File.Exists(Path.Combine(folder,"VkLayer_vrx_xgstereo.json"))) return folder;
        throw new FileNotFoundException(Loc.Get("ErrorGeometryLayerMissing"));
    }
    private static string NativeDirectory() {
        string root=EngineSession.RepositoryRoot();
        foreach(string folder in new[]{Path.Combine(root,"geometry","native-dx12"),Path.Combine(root,"bench","native","dx12stereo","out","proxy","Release")})
            if(File.Exists(Path.Combine(folder,"d3d12.dll")) && IsX64(Path.Combine(folder,"d3d12.dll"))) return folder;
        throw new FileNotFoundException(Loc.Get("ErrorGeometryNativeMissing"));
    }
    private static async Task<string> DxvkDirectory(string dataRoot,Action<string> status,CancellationToken token) {
        string root=EngineSession.RepositoryRoot();
        string cache=Path.Combine(dataRoot,"geometry","dxvk-"+DxvkVersion,"x64");
        foreach(string folder in new[]{Path.Combine(root,"geometry","dxvk","x64"),Path.Combine(root,"bench","native","xgstereo","out","dxvk-"+DxvkVersion,"x64"),cache})
            if(new[]{"d3d11.dll","dxgi.dll"}.All(n=>File.Exists(Path.Combine(folder,n)))) return folder;
        status(Loc.Get("StatusDxvkDownload"));
        string stage=Path.Combine(dataRoot,"geometry","download-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(stage);
        try {
            string archive=Path.Combine(stage,"dxvk.tar.gz");
            using var http=new HttpClient{Timeout=TimeSpan.FromMinutes(3)};
            using var response=await http.GetAsync($"https://github.com/doitsujin/dxvk/releases/download/v{DxvkVersion}/dxvk-{DxvkVersion}.tar.gz",HttpCompletionOption.ResponseHeadersRead,token);
            response.EnsureSuccessStatusCode();
            await using(var output=new FileStream(archive,FileMode.CreateNew)) {
                await response.Content.CopyToAsync(output,token);
            }
            if(!Hash(archive).Equals(ArchiveHash,StringComparison.Ordinal)) throw new InvalidDataException(Loc.Get("ErrorDxvkHash"));
            string extracted=Path.Combine(stage,"x64"); Directory.CreateDirectory(extracted);
            using(var file=File.OpenRead(archive)) using(var gzip=new GZipStream(file,CompressionMode.Decompress)) using(var tar=new TarReader(gzip)) {
                while(await tar.GetNextEntryAsync(cancellationToken:token) is {} entry) {
                    string? name=entry.Name switch { "dxvk-2.7.1/x64/d3d11.dll"=>"d3d11.dll", "dxvk-2.7.1/x64/dxgi.dll"=>"dxgi.dll", _=>null };
                    if(name==null) continue;
                    if(entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile) || entry.DataStream==null) throw new InvalidDataException(Loc.Get("ErrorDxvkHash"));
                    await using var target=new FileStream(Path.Combine(extracted,name),FileMode.CreateNew); await entry.DataStream.CopyToAsync(target,token);
                }
            }
            if(!new[]{"d3d11.dll","dxgi.dll"}.All(n=>File.Exists(Path.Combine(extracted,n)))) throw new InvalidDataException(Loc.Get("ErrorDxvkHash"));
            Directory.CreateDirectory(Path.GetDirectoryName(cache)!); Directory.Move(extracted,cache); return cache;
        } finally { Directory.Delete(stage,true); }
    }
    private static InstalledFile[] Files(string folder,Run run) {
        var files=JsonSerializer.Deserialize<InstalledFile[]>(File.ReadAllText(Path.Combine(folder,"installed.json"))) ?? [];
        string parent=Path.GetDirectoryName(Path.GetFullPath(run.Executable))!;
        var names=run.Backend==StereoPaths.Dx12?new[]{"d3d12.dll"}:run.Backend is "" or StereoPaths.Dxvk?new[]{"d3d11.dll","dxgi.dll"}:Array.Empty<string>();
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
    public static Task<Plan> Prepare(RunningApp app,Profile profile,string dataRoot,Action<string> status,CancellationToken token) =>
        PrepareWithLayer(app,profile,dataRoot,status,token,profile.EffectiveStereoBackend==StereoPaths.Dx12?NativeDirectory():LayerDirectory());
    // Unit preflight does not load the DLL; supply a fixture layer directory so
    // standard desktop smoke checks remain independent of optional native builds.
    internal static async Task<Plan> PrepareWithLayer(RunningApp app,Profile profile,string dataRoot,Action<string> status,CancellationToken token,string layer) {
        if(!StereoPaths.Geometry(profile.EffectiveStereoBackend) && profile.EffectiveStereoBackend!=StereoPaths.Dx12) throw new InvalidOperationException(Loc.Get("PathDx12Unavailable"));
        if(!IsX64(app.FullPath)) throw new InvalidOperationException(Loc.Get("ErrorGeometryX64"));
        if(profile.EffectiveStereoBackend==StereoPaths.Dx12) ValidateNativeRuntime(layer);
        using var original=Process.GetProcessById(app.Pid);
        long ticks=original.StartTime.ToUniversalTime().Ticks;
        if(!Same(RunningApps.ProcessPath(app.Pid),app.FullPath)) throw new InvalidOperationException(Loc.Get("ErrorProcessChanged"));
        var originalArgs=profile.GeometryLaunchArguments!=null ? ParseArguments(profile.GeometryLaunchArguments) : await ProcessArguments(app.Pid,token);
        var args=ApiArguments(originalArgs,profile.EffectiveStereoBackend,Path.GetFileName(app.FullPath).EndsWith("-Shipping.exe",StringComparison.OrdinalIgnoreCase));
        string folder=string.IsNullOrEmpty(profile.GeometryWorkingDirectory)?Path.GetDirectoryName(app.FullPath)!:profile.GeometryWorkingDirectory;
        string steam=string.IsNullOrEmpty(profile.GeometrySteamAppId)?FindSteamAppId(app.FullPath):profile.GeometrySteamAppId;
        string? dxvk=profile.EffectiveStereoBackend==StereoPaths.Dxvk?await DxvkDirectory(dataRoot,status,token):null;
        string[] owned=RunFolders(dataRoot).Where(d=>ReadRun(d) is {} run && run.Pid==app.Pid && run.StartTicks==ticks && Same(run.Executable,app.FullPath)).ToArray();
        var plan=new Plan(app,ticks,profile.EffectiveStereoBackend,layer,args,folder,steam,StereoPaths.Session(app.FullPath),dxvk,owned);
        ValidateFiles(plan); return plan;
    }
    private static void ValidateFiles(Plan plan) {
        var owned=new List<InstalledFile>();
        foreach(var folder in plan.OwnedRuns) { var run=ReadRun(folder); if(run!=null) owned.AddRange(Files(folder,run)); }
        foreach(string name in new[]{"d3d11.dll","dxgi.dll","d3d12.dll"}) {
            string file=Path.Combine(Path.GetDirectoryName(plan.App.FullPath)!,name);
            if(File.Exists(file) && !owned.Any(o=>Same(o.Path,file) && Hash(file).Equals(o.SHA256,StringComparison.OrdinalIgnoreCase)))
                throw new IOException(Loc.Format("ErrorGeometryExistingDll",file));
        }
    }
    internal static List<InstalledFile> InstallFiles(string? sourceFolder,string executable,string runRoot,string backend=StereoPaths.Dxvk) {
        using var gate=new FileGate(executable);
        var installed=new List<InstalledFile>();
        try {
            if(sourceFolder!=null) foreach(string name in backend==StereoPaths.Dx12?new[]{"d3d12.dll"}:new[]{"d3d11.dll","dxgi.dll"}) {
                string target=Path.Combine(Path.GetDirectoryName(executable)!,name), source=Path.Combine(sourceFolder,name);
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
        start.Environment["VRX_XG_SESSION"]=plan.Session;
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
        if(plan.SteamAppId.Length>0)start.Environment["SteamAppId"]=start.Environment["SteamGameId"]=plan.SteamAppId;
    }
    private static void ValidateNativeRuntime(string folder) {
        string file=Path.Combine(folder,"d3d12.dll");
        if(!File.Exists(file) || !IsX64(file))throw new InvalidDataException(Loc.Get("ErrorGeometryNativeMissing"));
    }
    private static void ValidateRuntime(Plan plan) {
        if(plan.Backend==StereoPaths.Dx12){ValidateNativeRuntime(plan.LayerDirectory);return;}
        if(!StereoPaths.Geometry(plan.Backend) || !File.Exists(Path.Combine(plan.LayerDirectory,"vrx_xgstereo.dll")) || !File.Exists(Path.Combine(plan.LayerDirectory,"VkLayer_vrx_xgstereo.json")))
            throw new FileNotFoundException(Loc.Get("ErrorGeometryLayerMissing"));
        if(plan.Backend==StereoPaths.Dxvk && (plan.DxvkDirectory==null || new[]{"d3d11.dll","dxgi.dll"}.Any(n=>!File.Exists(Path.Combine(plan.DxvkDirectory,n)) || !IsX64(Path.Combine(plan.DxvkDirectory,n)))))
            throw new FileNotFoundException(Loc.Get("ErrorGeometryLayerMissing"));
    }
    public static async Task<Run> Restart(Plan plan,Profile profile,string dataRoot,Action<string> status,CancellationToken token) {
        // All preflight and the launch-details dialog have completed before this point.
        token.ThrowIfCancellationRequested();
        ValidateRuntime(plan);
        ValidateFiles(plan);
        var identity=new Run(plan.App.Pid,plan.StartTicks,plan.App.FullPath,plan.Session);
        if(!IsRunning(identity)) throw new InvalidOperationException(Loc.Get("ErrorProcessChanged"));
        using(var game=Process.GetProcessById(plan.App.Pid)) {
            if(!game.CloseMainWindow()) throw new InvalidOperationException(Loc.Get("ErrorGeometryClose"));
            try { await game.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(30),token); }
            catch(TimeoutException) { throw new InvalidOperationException(Loc.Get("ErrorGeometryClose")); }
        }
        foreach(var folder in plan.OwnedRuns) Cleanup(folder);
        string runRoot=Path.Combine(RunsRoot(dataRoot),Guid.NewGuid().ToString("N")); Directory.CreateDirectory(runRoot);
        var installed=new List<InstalledFile>();
        // A zero PID records ownership even when startup fails before a child exists.
        var run=new Run(0,0,plan.App.FullPath,plan.Session,Backend:plan.Backend);
        ProfileStore.AtomicWrite(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
        try {
            using(new FileGate(plan.App.FullPath)) {
            ValidateFiles(plan);
            installed = InstallFiles(plan.Backend==StereoPaths.Dx12?plan.LayerDirectory:plan.DxvkDirectory,plan.App.FullPath,runRoot,plan.Backend);
            if(plan.Backend!=StereoPaths.Dx12) {
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"stereo.txt"),FormattableString.Invariant($"{profile.GeometrySeparation:R} {profile.GeometryConvergence:R}\n"));
                ProfileStore.AtomicWrite(Path.Combine(runRoot,"dxvk.conf"),"dxvk.enableGraphicsPipelineLibrary = False\ndxvk.enableDescriptorBuffer = False\ndxvk.disableNvLowLatency2 = True\n");
            }
            var start=new ProcessStartInfo(plan.App.FullPath){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=plan.WorkingDirectory};
            foreach(string arg in plan.Arguments) start.ArgumentList.Add(arg);
            ConfigureEnvironment(start,plan,profile,runRoot);
            token.ThrowIfCancellationRequested();
            var child=Process.Start(start) ?? throw new InvalidOperationException(Loc.Get("ErrorGeometryLaunch"));
            run=run with {Pid=child.Id,StartTicks=child.StartTime.ToUniversalTime().Ticks};
            ProfileStore.AtomicWrite(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
            _=Observe(child,runRoot);
            }
            status(Loc.Get("StatusGeometryWaiting"));
            var deadline=Environment.TickCount64+45000;
            while(Environment.TickCount64<deadline && IsRunning(run)) {
                token.ThrowIfCancellationRequested();
                NativeStereoStatus? native=plan.Backend==StereoPaths.Dx12?NativeStereoStatus.Read(run.Session,run.Pid):null;
                if(native!=null && (native.Stops>0 || native.HookFailures>0 || !native.Mirroring)) {
                    ProfileStore.AtomicWrite(Path.Combine(runRoot,"native-status.json"),JsonSerializer.Serialize(native));
                    throw new InvalidOperationException(Loc.Format("ErrorGeometryNativeStopped",native.Reason.Length>0?native.Reason:Loc.Get("ErrorGeometryNativeCoverage"),runRoot));
                }
                if(StreamMatches(run.Session,run.Pid) && (plan.Backend!=StereoPaths.Dx12 || native?.Ready==true)) {
                    if(native!=null)ProfileStore.AtomicWrite(Path.Combine(runRoot,"native-status.json"),JsonSerializer.Serialize(native));
                    return run;
                }
                await Task.Delay(150,token);
            }
            throw new InvalidOperationException(Loc.Format("ErrorGeometryNoStream",runRoot));
        } catch { if(run.Pid==0) { foreach(var file in installed) if(File.Exists(file.Path)&&Hash(file.Path).Equals(file.SHA256,StringComparison.Ordinal)) File.Delete(file.Path); } throw; }
    }
    private static async Task Observe(Process child,string folder) {
        try { await child.WaitForExitAsync(); Cleanup(folder); }
        catch(Exception ex) { Debug.WriteLine("[Geometry run] "+ex.Message); }
        finally { child.Dispose(); }
    }
    [DllImport("shell32.dll",CharSet=CharSet.Unicode,SetLastError=true)] private static extern nint CommandLineToArgvW(string command,out int count);
    [DllImport("kernel32.dll")] private static extern nint LocalFree(nint memory);
}
