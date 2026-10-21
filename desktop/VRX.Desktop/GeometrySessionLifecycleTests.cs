#if DEBUG
// Development builds only: these start copies of this executable with fixture switches
// (App.RunDevelopmentFixture) or use the repository's bench builds, none of which exist in a
// release build.
using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace VRX.Desktop;

internal static class GeometrySessionLifecycleTests
{
    // A headless stand-in for a game/viewer. No renderer, DLL or headset is used.
    internal static int Fixture(string control)
    {
        File.WriteAllText(control+".ready","ready");
        var until=Environment.TickCount64+60000;
        while(Environment.TickCount64<until) {
            try {
                var fields=File.ReadAllText(control).Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries);
                if(fields.Length==31 && fields[14]=="1") return 0;
            } catch(IOException) { }
            Thread.Sleep(20);
        }
        return 2;
    }

    public static async Task Run(string output)
    {
        static void Check(bool ok,string reason) { if(!ok) throw new Exception("Geometry lifetime: "+reason); }
        string root=Path.Combine(output,"lifecycle-"+Guid.NewGuid().ToString("N"));
        string bin=Path.Combine(root,"bin"),folder=Path.Combine(root,"sessions","recovered");
        Directory.CreateDirectory(bin); Directory.CreateDirectory(folder);
        foreach(string name in new[]{"VRX.Desktop.dll","VRX.Desktop.deps.json","VRX.Desktop.runtimeconfig.json"})
            File.Copy(Path.Combine(AppContext.BaseDirectory,name),Path.Combine(bin,name));
        string gameExe=Path.Combine(bin,"game.exe"),viewerExe=Path.Combine(bin,"xrplayer.fixture.exe");
        File.Copy(Environment.ProcessPath!,gameExe); File.Copy(Environment.ProcessPath!,viewerExe);
        string gameControl=Path.Combine(root,"game.txt"),control=Path.Combine(folder,"control.txt");
        var settings=new Profile{Width=7,Distance=4,GeometrySeparation=.5f};
        string original=settings.Control(17,23,false);
        File.WriteAllText(control,original);
        Check(GeometrySessionLifecycle.RequestStop(control),"accept current control protocol");
        var expected=original.Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries); expected[14]="1";
        Check(File.ReadAllText(control)==string.Join(' ',expected)+"\n","stop preserves visual settings and recenter/menu counters");
        File.WriteAllText(control,"invalid snapshot");
        Check(!GeometrySessionLifecycle.RequestStop(control) && File.ReadAllText(control)=="invalid snapshot","malformed controls are untouched");
        var children=new List<Process>();
        async Task<Process> Start(string executable,string file,params string[] args) {
            File.WriteAllText(file,original);
            File.Delete(file+".ready");
            var start=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true};
            start.ArgumentList.Add("--session-lifecycle-fixture="+file);
            foreach(var arg in args) start.ArgumentList.Add(arg);
            var child=Process.Start(start)!; children.Add(child);
            var until=Environment.TickCount64+10000;
            while(!File.Exists(file+".ready") && !child.HasExited && Environment.TickCount64<until) await Task.Delay(20);
            Check(File.Exists(file+".ready") && !child.HasExited,"headless fixture is ready");
            return child;
        }
        GeometryLaunch.Run Id(Process p,string session="fixture")=>new(p.Id,p.StartTime.ToUniversalTime().Ticks,RunningApps.ProcessPath(p.Id),session);
        try {
            var game=await Start(gameExe,gameControl);
            var app=new RunningApp(game.Id,"fixture",gameExe,[],null);
            var viewer=await Start(viewerExe,control,"--geometry-session=fixture","--geometry-pid="+game.Id,"--control="+control);
            using var ui=Process.GetCurrentProcess();
            var lease=new GeometrySessionLifecycle.Lease(Id(viewer),Id(game),Id(ui),control);
            void Save(GeometrySessionLifecycle.Lease value)=>File.WriteAllText(Path.Combine(folder,"geometry-viewer.json"),JsonSerializer.Serialize(value));
            Save(lease);
            await GeometrySessionLifecycle.ReleasePrevious(app,root,CancellationToken.None,bin,false);
            Check(!viewer.HasExited,"a different live UI keeps ownership of its viewer");
            lease=lease with {Ui=new(0,0,"",""),Viewer=lease.Viewer with {StartTicks=lease.Viewer.StartTicks-1}};
            Save(lease);
            await GeometrySessionLifecycle.ReleasePrevious(app,root,CancellationToken.None,bin,false);
            Check(!viewer.HasExited,"PID reuse or a stale process identity cannot stop a viewer");
            lease=lease with {Viewer=Id(viewer)}; Save(lease);
            Save(lease with {Game=lease.Game with {Session="different-stream"}});
            await GeometrySessionLifecycle.ReleasePrevious(app,root,CancellationToken.None,bin,false);
            Check(!viewer.HasExited,"the actual command line must match the recorded stream");
            Save(lease);
            await GeometrySessionLifecycle.ReleasePrevious(app with {FullPath=Path.Combine(bin,"another-game.exe")},root,CancellationToken.None,bin,false);
            Check(!viewer.HasExited,"another game's viewer remains untouched");
            await GeometrySessionLifecycle.ReleasePrevious(app,root,CancellationToken.None,bin,false);
            Check(viewer.HasExited && viewer.ExitCode==0 && !game.HasExited,"recovering a verified orphan stops only its viewer, cooperatively");
            var staleSource=await Start(viewerExe,control);
            await GeometrySessionLifecycle.WatchGame(staleSource,game.Id,control,game.StartTime.ToUniversalTime().Ticks-1).WaitAsync(TimeSpan.FromSeconds(10));
            Check(staleSource.HasExited && !game.HasExited,"a replacement process cannot retain the previous game's viewer");
            var replacement=await Start(viewerExe,control);
            var watch=GeometrySessionLifecycle.WatchGame(replacement,game.Id,control);
            Check(GeometrySessionLifecycle.RequestStop(gameControl),"close the simulated game normally");
            await game.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            await watch.WaitAsync(TimeSpan.FromSeconds(10));
            Check(replacement.HasExited && replacement.ExitCode==0,"game exit automatically closes its owned headset viewer");
            var late=await Start(viewerExe,control);
            await GeometrySessionLifecycle.WatchGame(late,game.Id,control).WaitAsync(TimeSpan.FromSeconds(10));
            Check(late.HasExited && late.ExitCode==0,"a game that exits during startup cannot leave a viewer behind");
            File.WriteAllText(Path.Combine(output,"geometry-lifecycle-pass.txt"),"PASS: control settings preserved; malformed snapshot rejected; live UI/other game/stale PID protected; verified orphan cooperatively released; game exit and startup-exit close the viewer. No installed game or headset used.");
        } finally {
            foreach(var child in children) {
                if(!child.HasExited) { child.Kill(); await child.WaitForExitAsync(); }
                child.Dispose();
            }
        }
    }
}
#endif
