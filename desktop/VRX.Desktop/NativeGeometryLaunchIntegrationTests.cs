using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace VRX.Desktop;

// Opt-in: only this unique copied fixture is restarted or closed. No OpenXR.
internal static class NativeGeometryLaunchIntegrationTests
{
    internal static async Task Run(string output)
    {
        string repository=EngineSession.RepositoryRoot(),native=Path.Combine(EngineSession.RepositoryRoot(),"bench","native","dx12stereo","out","Release");
        string source=Path.Combine(native,"dx12_present_fixture.exe"),snapshot=Path.Combine(native,"geometry_gpu_snapshot.exe");
        if(!File.Exists(source)||!File.Exists(snapshot))throw new FileNotFoundException("Build native DX12 presentation and GPU snapshot fixtures first");
        string root=Path.Combine(output,"managed-native-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        string executable=Path.Combine(root,"native [fixture].exe");File.Copy(source,executable,false);
        string session=StereoPaths.Session(executable),literal="--fixture-token=space and \"quote\"";
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(100));var token=timeout.Token;
        var log=new List<string>();GeometryLaunch.Run? running=null;
        void Check(bool ok,string reason){if(!ok)throw new Exception("Managed native DX12 integration: "+reason);}
        async Task<RunningApp> Window(GeometryLaunch.Run run){
            var until=Environment.TickCount64+10000;
            while(Environment.TickCount64<until && GeometryLaunch.IsRunning(run)){
                var app=RunningApps.List(false).FirstOrDefault(a=>a.Pid==run.Pid && a.CanAttach && string.Equals(a.FullPath,executable,StringComparison.OrdinalIgnoreCase));
                if(app!=null)return app;await Task.Delay(50,token);
            }throw new Exception("Native fixture window did not appear");
        }
        async Task<string> Tool(string file,IEnumerable<string> arguments){
            var start=new ProcessStartInfo(file){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};foreach(string arg in arguments)start.ArgumentList.Add(arg);
            using var p=Process.Start(start)??throw new Exception("Native verifier did not start");var stdout=p.StandardOutput.ReadToEndAsync(token);var stderr=p.StandardError.ReadToEndAsync(token);
            try{await p.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(20),token);}catch{if(!p.HasExited){p.Kill();await p.WaitForExitAsync();}throw;}
            string text=await stdout+await stderr;log.Add(text);Check(p.ExitCode==0,"native verifier failed: "+text);return text;
        }
        List<GeometryLaunch.Run> Owned(){
            var own=new List<GeometryLaunch.Run>();if(running!=null)own.Add(running);
            foreach(string file in Directory.GetFiles(root,"run.json",SearchOption.AllDirectories)){
                var run=JsonSerializer.Deserialize<GeometryLaunch.Run>(File.ReadAllText(file));if(run!=null && string.Equals(run.Executable,executable,StringComparison.OrdinalIgnoreCase))own.Add(run);
            }
            // Also recover a fixture that started just before its run record
            // could be updated. Exact full path limits this to our copied EXE.
            foreach(var p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(executable)))using(p){
                if(string.Equals(RunningApps.ProcessPath(p.Id),executable,StringComparison.OrdinalIgnoreCase))own.Add(new(p.Id,p.StartTime.ToUniversalTime().Ticks,executable,session,Backend:StereoPaths.Dx12));
            }return own.DistinctBy(r=>(r.Pid,r.StartTicks)).ToList();
        }
        try{
            var start=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=root};start.ArgumentList.Add("--serve-gpu="+session);start.ArgumentList.Add(literal);
            foreach(string name in new[]{"VRX_DX12_INTERCEPT","VRX_DX12_MIRROR","VRX_DX12_TEST_TEN","VRX_DX12_TEST_BGRA","VRX_DX12_TEST_MODERN"})start.Environment[name]="0";
            using(var original=Process.Start(start)??throw new Exception("Ordinary DX12 fixture did not start"))running=new(original.Id,original.StartTime.ToUniversalTime().Ticks,executable,session);
            await Window(running);Check(NativeStereoStatus.Read(session,running.Pid)==null && !GeometryLaunch.StreamMatches(session,running.Pid),"ordinary initial process has no bridge or stereo stream");
            var profile=new Profile{ExecutablePath=executable,GeometrySeparation=.4f};profile.SelectStereoBackend(StereoPaths.Dx12);
            for(int attempt=0;attempt<2;++attempt){
                var plan=await GeometryLaunch.Prepare(await Window(running),profile,root,log.Add,token);Check(plan.Arguments.Contains(literal),"literal argument survives managed command-line capture");int old=running.Pid;
                running=await GeometryLaunch.Restart(plan,profile,root,log.Add,token);
                Check(running.Pid!=old && GeometryLaunch.IsRunning(running) && NativeStereoStatus.Read(session,running.Pid)?.Ready==true,"native child has fresh replay/publication diagnostics");
                Check(GeometryLaunch.ExistingSession(await Window(running),profile,root)==session,"native stream reuse validates backend and owner");
                var wrong=new Profile{ExecutablePath=executable};wrong.SelectStereoBackend(StereoPaths.Vulkan);Check(GeometryLaunch.ExistingSession(await Window(running),wrong,root)==null,"native output cannot silently satisfy a Vulkan choice");
                string pixels=await Tool(snapshot,[session,Path.Combine(root,$"eyes-{attempt}.ppm"),running.Pid.ToString(CultureInfo.InvariantCulture)]);
                var pair=Regex.Match(pixels,@"frame (\d+)/(\d+).*mean eye difference ([0-9.]+)");Check(pair.Success && pair.Groups[1].Value==pair.Groups[2].Value && double.Parse(pair.Groups[3].Value,CultureInfo.InvariantCulture)>.1,"ordinary one-view app produces distinct native same-frame eye pixels");
                string engine=Environment.GetEnvironmentVariable("VRX_GEOMETRY_TEST_ENGINE")??Path.Combine(repository,"bench","native","openxr","out","xrplayer.exe");
                string handoff=await Tool(engine,["--geometry-session="+session,"--geometry-pid="+running.Pid,"--check-geometry","--debug","--normal-gpu-priority"]);
                Check(handoff.Contains("Geometry check: PASS") && handoff.Contains("same-frame eyes") && !handoff.Contains("InitXrInstance:"),"actual VRX GPU consumer passes without starting a headset session");
                Check(!File.Exists(Path.Combine(root,"d3d11.dll")) && !File.Exists(Path.Combine(root,"dxgi.dll")),"native launch installs no translation wrappers");
            }
            profile.GeometryLaunchArguments=GeometryLaunch.QuoteArgument("--serve-gpu="+session)+" --stop-replay";
            var failurePlan=await GeometryLaunch.Prepare(await Window(running),profile,root,log.Add,token);bool rejected=false;
            try{await GeometryLaunch.Restart(failurePlan,profile,root,log.Add,token);}catch(InvalidOperationException ex){log.Add(ex.Message);rejected=ex.Message.Contains("SOSetTargets");}
            Check(rejected,"native stop reason is surfaced instead of accepting one transient startup frame");
            running=Owned().FirstOrDefault(r=>GeometryLaunch.IsRunning(r))??throw new Exception("Unsupported native app was unexpectedly closed");
            Check(NativeStereoStatus.Read(session,running.Pid) is {Stops:1} && GeometryLaunch.ExistingSession(await Window(running),profile,root)==null,"failed replay leaves native app running and is not reusable as stereo");
            using(var last=Process.GetProcessById(running.Pid)){Check(last.CloseMainWindow(),"owned fixture closes normally");await last.WaitForExitAsync(token);Check(last.ExitCode==0,"native fallback shutdown is clean");}
            GeometryLaunch.RecoverOwned(root);Check(!File.Exists(Path.Combine(root,"d3d12.dll")),"native bridge is removed after normal exit");
            File.WriteAllText(Path.Combine(output,"native-geometry-integration-pass.txt"),"PASS: ordinary native DX12 baseline; two managed restarts; literal arguments; matching diagnostics; backend isolation; distinct same-frame GPU pixels; actual VRX handoff without OpenXR; unsupported-command failure remains native; owned cleanup. Logs: "+root);
        }finally{
            foreach(var run in Owned())if(GeometryLaunch.IsRunning(run)){using var child=Process.GetProcessById(run.Pid);if(!child.CloseMainWindow() || !child.WaitForExit(5000)){child.Kill();child.WaitForExit(5000);}}
            GeometryLaunch.RecoverOwned(root);File.WriteAllLines(Path.Combine(root,"integration.log"),log);
        }
    }
}
