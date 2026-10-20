using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;

namespace VRX.Desktop;

// Opt-in smoke extension. Starts/restarts only a freshly copied test fixture.
// No installed game, Steam launch option or existing VR session is modified.
internal static class GeometryLaunchIntegrationTests
{
    public static async Task Run(string output,bool translated=false)
    {
        string repository=EngineSession.RepositoryRoot();
        string fixture=translated?"dx12_present_fixture.exe":"d3d11_launch_fixture.exe";
        string source=Path.Combine(repository,"bench","native","dx12stereo","out","Release",fixture);
        if(!File.Exists(source)) throw new FileNotFoundException("Build the ordinary D3D11 launch fixture first",source);
        string root=Path.Combine(output,(translated?"managed-vkd3d-":"managed-dxvk-")+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        string executable=Path.Combine(root,fixture); File.Copy(source,executable,false);
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(100)); var token=timeout.Token;
        GeometryLaunch.Run? running=null;
        var log=new List<string>();
        void Check(bool ok,string reason) { if(!ok) throw new Exception("Managed geometry integration: "+reason); }
        async Task<string> Tool(string file,IEnumerable<string> arguments) {
            var start=new ProcessStartInfo(file){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
            foreach(string arg in arguments) start.ArgumentList.Add(arg);
            using var p=Process.Start(start) ?? throw new Exception("Test tool failed to start");
            var stdout=p.StandardOutput.ReadToEndAsync(token); var stderr=p.StandardError.ReadToEndAsync(token);
            try { await p.WaitForExitAsync(token).WaitAsync(TimeSpan.FromSeconds(20),token); }
            catch { if(!p.HasExited) { p.Kill(); await p.WaitForExitAsync(); } throw; }
            string text=await stdout+await stderr; log.Add(text); Check(p.ExitCode==0,"tool failed: "+text); return text;
        }
        async Task<RunningApp> Window(GeometryLaunch.Run run) {
            var until=Environment.TickCount64+10000;
            while(Environment.TickCount64<until && GeometryLaunch.IsRunning(run)) {
                var app=RunningApps.List(false).FirstOrDefault(a=>a.Pid==run.Pid && a.CanAttach && string.Equals(a.FullPath,executable,StringComparison.OrdinalIgnoreCase));
                if(app!=null) return app;
                await Task.Delay(50,token);
            }
            throw new Exception("Test fixture window did not appear");
        }
        try {
            var start=new ProcessStartInfo(executable){UseShellExecute=false,WorkingDirectory=root};
            if(translated) start.ArgumentList.Add("--serve-gpu="+StereoPaths.Session(executable));
            const string literal="--fixture-token=space and \"quote\""; start.ArgumentList.Add(literal);
            using(var original=Process.Start(start) ?? throw new Exception("Fixture failed to start"))
                running=new GeometryLaunch.Run(original.Id,original.StartTime.ToUniversalTime().Ticks,executable,StereoPaths.Session(executable));
            var profile=new Profile{ExecutablePath=executable,GeometrySeparation=.15f}; profile.SelectStereoBackend(translated?StereoPaths.Vkd3d:StereoPaths.Dxvk);
            // Two consecutive restarts exercise old-run observer cleanup racing
            // the next installation, not merely one successful first launch.
            for(int attempt=0;attempt<2;++attempt) {
                var app=await Window(running);
                var plan=await GeometryLaunch.Prepare(app,profile,root,log.Add,token);
                Check(plan.Arguments.Contains(literal),"literal launch argument survives capture");
                int old=running.Pid;
                running=await GeometryLaunch.Restart(plan,profile,root,log.Add,token);
                Check(running.Pid!=old && GeometryLaunch.IsRunning(running),"normal close and restart produces a new process");
                var readyApp=await Window(running);
                var reuseUntil=Environment.TickCount64+5000;
                string? reused;
                do { reused=GeometryLaunch.ExistingSession(readyApp,profile,root); if(reused!=null) break; await Task.Delay(50,token); } while(Environment.TickCount64<reuseUntil);
                Check(reused==running.Session,"ready stream is reusable after initial swapchain resizing, without another restart");
                string snapshot=Path.Combine(repository,"bench","native","dx12stereo","out","Release","geometry_gpu_snapshot.exe");
                string pixels=await Tool(snapshot,[running.Session,Path.Combine(root,$"eyes-{attempt}.ppm"),running.Pid.ToString(System.Globalization.CultureInfo.InvariantCulture)]);
                var pair=Regex.Match(pixels,@"frame (\d+)/(\d+).*mean eye difference ([0-9.]+)");
                Check(pair.Success && pair.Groups[1].Value==pair.Groups[2].Value && double.Parse(pair.Groups[3].Value,System.Globalization.CultureInfo.InvariantCulture)>.1,
                    "the renderer produces distinct same-frame eyes for an ordinary D3D11 app");
                string engine=Environment.GetEnvironmentVariable("VRX_GEOMETRY_TEST_ENGINE") ?? Path.Combine(repository,"bench","native","openxr","out","xrplayer.exe");
                string result=await Tool(engine,["--geometry-session="+running.Session,"--geometry-pid="+running.Pid,"--check-geometry","--debug","--normal-gpu-priority"]);
                Check(result.Contains("Geometry check: PASS") && result.Contains("same-frame eyes") && !result.Contains("InitXrInstance:"),"actual VRX GPU eye handoff passes without starting VR");
            }
            using(var final=Process.GetProcessById(running.Pid)) {
                Check(final.CloseMainWindow(),"close the owned fixture normally"); await final.WaitForExitAsync(token);
            }
            var deadline=Environment.TickCount64+5000;
            while((File.Exists(Path.Combine(root,"d3d11.dll"))||File.Exists(Path.Combine(root,"dxgi.dll"))) && Environment.TickCount64<deadline)
                await Task.Delay(50,token);
            GeometryLaunch.RecoverOwned(root);
            Check(new[]{"d3d11.dll","dxgi.dll","d3d12.dll","d3d12core.dll"}.All(n=>!File.Exists(Path.Combine(root,n))),"all scoped graphics DLLs are removed after fixture exit");
            File.WriteAllText(Path.Combine(output,translated?"vkd3d-geometry-integration-pass.txt":"geometry-integration-pass.txt"),(translated?"PASS: managed vkd3d-proton D3D12 translation and four-component installation; ":"PASS: ")+"two managed restarts; literal arguments; distinct same-frame geometry; stream reuse; actual VRX GPU handoff; normal exit and scoped DLL cleanup. Logs: "+root);
        } finally {
            var owned=new List<GeometryLaunch.Run>(); if(running!=null) owned.Add(running);
            foreach(string file in Directory.GetFiles(root,"run.json",SearchOption.AllDirectories)) {
                var run=System.Text.Json.JsonSerializer.Deserialize<GeometryLaunch.Run>(File.ReadAllText(file));
                if(run!=null && string.Equals(run.Executable,executable,StringComparison.OrdinalIgnoreCase)) owned.Add(run);
            }
            foreach(var run in owned.DistinctBy(r=>r.Pid)) if(GeometryLaunch.IsRunning(run)) {
                using var own=Process.GetProcessById(run.Pid);
                if(!own.CloseMainWindow() || !own.WaitForExit(5000)) { own.Kill(); own.WaitForExit(5000); }
            }
            GeometryLaunch.RecoverOwned(root);
            File.WriteAllLines(Path.Combine(root,"integration.log"),log);
        }
    }
}
