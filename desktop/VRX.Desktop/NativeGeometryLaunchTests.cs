#if DEBUG
// Development builds only: these start copies of this executable with fixture switches
// (App.RunDevelopmentFixture) or use the repository's bench builds, none of which exist in a
// release build.
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text.Json;

namespace VRX.Desktop;

// Passive smoke checks: disposable files/IPC only, no real game is restarted.
internal static class NativeGeometryLaunchTests
{
    internal static async Task Run(string root,RunningApp self)
    {
        static void Check(bool ok,string reason){if(!ok)throw new Exception("Native geometry launch: "+reason);}
        byte[] packet=new byte[NativeStereoStatus.Size];
        void U(int at,uint value)=>BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(at,4),value);
        void Q(int at,ulong value)=>BinaryPrimitives.WriteUInt64LittleEndian(packet.AsSpan(at,8),value);
        U(0,0x53445856);U(4,1);U(8,504);U(12,(uint)self.Pid);U(24,1);U(32,472);U(36,4);Q(16,10000);Q(136,4);Q(192,3);
        Check(NativeStereoStatus.Parse(packet,self.Pid,10001)?.Ready==true,"bounded native packet proves current replay/publication counts");
        Check(NativeStereoStatus.Parse(packet,self.Pid+1,10001)==null && NativeStereoStatus.Parse(packet,self.Pid,9999)==null &&
            NativeStereoStatus.Parse(packet.AsSpan(1),self.Pid,10001)==null,"foreign PID, future clock and malformed size rejected");
        Check(NativeStereoStatus.Parse(packet,self.Pid,13000)?.Ready==false,"stale counters cannot advertise a connection");
        Q(192,1);Check(NativeStereoStatus.Parse(packet,self.Pid,10001)?.Ready==false,"one transient startup publication is not a ready connection");Q(192,3);
        U(24,0);Check(NativeStereoStatus.Parse(packet,self.Pid,10001)?.Ready==false,"observation mode is never stereo success");U(24,1);
        Q(128,1);Check(NativeStereoStatus.Parse(packet,self.Pid,10001)?.Ready==false,"incomplete hooks cannot advertise stereo");Q(128,0);
        Q(144,1);packet[248]=(byte)'x';packet[249]=0;packet[^1]=(byte)'x';
        Check(NativeStereoStatus.Parse(packet,self.Pid,10001) is {Ready:false,Reason:"x"},"a bounded stop reason survives unused trailing bytes");
        Array.Fill(packet,(byte)'x',248,256);Check(NativeStereoStatus.Parse(packet,self.Pid,10001)==null,"unterminated diagnostics rejected");Array.Clear(packet,248,256);Q(144,0);
        string session="native-smoke-"+Guid.NewGuid().ToString("N");Q(16,(ulong)Environment.TickCount64);
        using(var mutex=new Mutex(false,$@"Local\VRX.dx12.{session}.lock"))
        using(var map=MemoryMappedFile.CreateNew($@"Local\VRX.dx12.{session}.status",packet.Length))
        using(var view=map.CreateViewAccessor()){
            view.WriteArray(0,packet,0,packet.Length);
            Check(NativeStereoStatus.Read(session,self.Pid)?.Ready==true && NativeStereoStatus.Read(session,self.Pid+1)==null,"live IPC retains process ownership");
            using var held=new ManualResetEventSlim();using var release=new ManualResetEventSlim();
            var owner=Task.Run(()=>{mutex.WaitOne();try{held.Set();release.Wait();}finally{mutex.ReleaseMutex();}});
            held.Wait();try{var watch=Stopwatch.StartNew();Check(NativeStereoStatus.Read(session,self.Pid)==null && watch.ElapsedMilliseconds<100,"busy diagnostic metadata does not block the launcher");}finally{release.Set();await owner;}
        }
        Check(GeometryLaunch.ApiArguments(["-vulkan","-dx11","--save=a b"],StereoPaths.Dx12,true).SequenceEqual(new[]{"--save=a b","-dx12"}),"native Unreal launch selects DX12 and preserves literal arguments");
        string runtime=Path.Combine(root,"native-runtime");Directory.CreateDirectory(runtime);byte[] pe=new byte[70];BinaryPrimitives.WriteUInt16LittleEndian(pe,0x5a4d);BinaryPrimitives.WriteUInt32LittleEndian(pe.AsSpan(60),64);BinaryPrimitives.WriteUInt32LittleEndian(pe.AsSpan(64),0x4550);BinaryPrimitives.WriteUInt16LittleEndian(pe.AsSpan(68),0x8664);File.WriteAllBytes(Path.Combine(runtime,"d3d12.dll"),pe);
        var profile=new Profile{ExecutablePath=self.FullPath,GeometryLaunchArguments="--literal=\"space value\"",GeometrySeparation=.125f,GeometryConvergence=.01f};profile.SelectStereoBackend(StereoPaths.Dx12);
        var plan=await GeometryLaunch.PrepareWithLayer(self,profile,root,_=>{},CancellationToken.None,runtime);
        Check(plan.Backend==StereoPaths.Dx12 && plan.DxvkDirectory==null && plan.Arguments.SequenceEqual(new[]{"--literal=space value"}),"native preflight works without Vulkan/DXVK and leaves the current process alive");
        using(var cancelled=new CancellationTokenSource()) {
            cancelled.Cancel();bool stopped=false;try{await GeometryLaunch.Restart(plan,profile,root,_=>{},cancelled.Token);}catch(OperationCanceledException){stopped=true;}
            Check(stopped,"a cancelled attempt is rejected before closing the selected process");
        }
        bool missing=false;try{await GeometryLaunch.Restart(plan with{LayerDirectory=Path.Combine(root,"missing-runtime")},profile,root,_=>{},CancellationToken.None);}catch(InvalidDataException){missing=true;}
        Check(missing,"missing runtime is detected before the selected process is closed");
        var start=new ProcessStartInfo(self.FullPath){UseShellExecute=false};start.Environment["VK_INSTANCE_LAYERS"]="other_layer;VK_LAYER_VRX_xgstereo";start.Environment["VRX_DX12_TEST_TEN"]="1";start.Environment["VRX_DX12_RECORD_ONLY"]="1";start.Environment["VRX_DX12_REPLAY_PROBES"]="1";start.Environment["VRX_DX12_GPU_VALIDATION"]="1";start.Environment["VRX_DX12_DEBUG_LAYER"]="1";start.Environment["VRX_DX12_DEBUG_LOG"]="unwanted-native-debug.log";
        start.Environment["VRX_DX12_CLONE_ONLY"]="1";
        start.Environment["VRX_DX12_SKIP_COMMANDS"]="1";start.Environment["VRX_DX12_SKIP_WORK"]="1";start.Environment["VRX_DX12_STATE_ONLY"]="1";start.Environment["VRX_DX12_RECORD_COMMANDS"]="SetPipelineState";
        var culture=CultureInfo.CurrentCulture;try{CultureInfo.CurrentCulture=CultureInfo.GetCultureInfo("fr-FR");GeometryLaunch.ConfigureEnvironment(start,plan with{SteamAppId="999"},profile,root);}finally{CultureInfo.CurrentCulture=culture;}
        Check(start.Environment["VRX_DX12_INTERCEPT"]=="1" && start.Environment["VRX_DX12_MIRROR"]=="1" && start.Environment["VRX_DX12_SHIFT"]=="0.125" && start.Environment["VRX_DX12_CONVERGENCE"]=="0.01" && start.Environment["SteamAppId"]=="999" && start.Environment["VK_INSTANCE_LAYERS"]=="other_layer" && !start.Environment.ContainsKey("VRX_DX12_TEST_TEN"),"native child environment is scoped, invariant and excludes the experimental Vulkan layer/test options");
        Check(!start.Environment.ContainsKey("VRX_DX12_REPLAY_PROBES") && !start.Environment.ContainsKey("VRX_DX12_GPU_VALIDATION") && !start.Environment.ContainsKey("VRX_DX12_DEBUG_LAYER") && !start.Environment.ContainsKey("VRX_DX12_DEBUG_LOG") && !start.Environment.ContainsKey("VRX_DX12_RECORD_ONLY") && !start.Environment.ContainsKey("VRX_DX12_CLONE_ONLY") && !start.Environment.ContainsKey("VRX_DX12_SKIP_COMMANDS") && !start.Environment.ContainsKey("VRX_DX12_SKIP_WORK") && !start.Environment.ContainsKey("VRX_DX12_STATE_ONLY") && !start.Environment.ContainsKey("VRX_DX12_RECORD_COMMANDS"),"ordinary managed launches cannot inherit command-recording diagnostics");
        string game=Path.Combine(root,"native-owned","game.exe"),runRoot=Path.Combine(root,"native-run");Directory.CreateDirectory(Path.GetDirectoryName(game)!);Directory.CreateDirectory(runRoot);
        File.WriteAllText(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(new GeometryLaunch.Run(0,0,game,"fixture",Backend:StereoPaths.Dx12)));
        GeometryLaunch.InstallFiles(runtime,game,runRoot,StereoPaths.Dx12);Check(File.Exists(Path.Combine(Path.GetDirectoryName(game)!,"d3d12.dll")) && !File.Exists(Path.Combine(Path.GetDirectoryName(game)!,"dxgi.dll")),"native install scopes only its own bridge");
        string refusedRoot=Path.Combine(root,"native-refused");Directory.CreateDirectory(refusedRoot);
        bool refused=false;try{GeometryLaunch.InstallFiles(runtime,game,refusedRoot,StereoPaths.Dx12);}catch(IOException){refused=true;}Check(refused,"an existing native DLL is never overwritten");
        GeometryLaunch.Cleanup(runRoot);Check(!File.Exists(Path.Combine(Path.GetDirectoryName(game)!,"d3d12.dll")),"native cleanup uses backend-specific ownership and exact content hashes");
    }
}
#endif
