using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Buffers.Binary;

namespace VRX.Desktop;

// Runs only under --smoke-test. Uses disposable files and this smoke process;
// never closes a real game, starts a renderer, or installs a real game DLL.
internal static class GeometryLaunchTests
{
    public static async Task Run(string output)
    {
        static void Check(bool value,string reason) { if(!value) throw new Exception("Geometry launch: "+reason); }
        string root=Path.Combine(output,"geometry-launch-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        string exe=Path.Combine(root,"game","fake-vrx-probe.exe"); Directory.CreateDirectory(Path.GetDirectoryName(exe)!); File.WriteAllText(exe,"fixture");
        string[] args=["", "space in argument", "literal\"quote", @"C:\Games\trailing\", "semi;colon", "$literal", "--value=a b", "\u00e9"];
        Check(GeometryLaunch.ParseArguments(string.Join(' ',args.Select(GeometryLaunch.QuoteArgument))).SequenceEqual(args),"Windows argument quoting preserves literal values");
        Check(GeometryLaunch.ApiArguments(["-dx12","-windowed","-ResX=2560"],StereoPaths.Vulkan,true).SequenceEqual(new[]{"-windowed","-ResX=2560","-vulkan"}),"native Vulkan replaces only the old API switch");
        Check(GeometryLaunch.ApiArguments(["-vulkan","--save=a b"],StereoPaths.Dxvk,true).SequenceEqual(new[]{"--save=a b","-dx11"}),"DXVK retains unrelated arguments");
        Check(GeometryLaunch.ApiArguments(["--custom-renderer=vk"],StereoPaths.Vulkan,false).SequenceEqual(new[]{"--custom-renderer=vk"}),"unknown engines do not receive invented switches");
        Check(GeometryLaunch.ApiArguments(["-dx12","--custom"],StereoPaths.Vulkan,false).SequenceEqual(new[]{"-dx12","--custom"}),"unknown engines preserve existing API switches for review");
        var legacy=new Profile{GeometryStereo=true}; Check(legacy.EffectiveStereoBackend==StereoPaths.Vulkan,"old geometry profile migrates");
        var p=new Profile{ExecutablePath=exe}; Check(p.EffectiveStereoBackend==StereoPaths.Depth,"old profiles default to depth estimate");
        p.SelectStereoBackend(StereoPaths.Dxvk); p.GeometrySeparation=.8f; p.GeometryLaunchArguments="-dx11 -windowed";
        var store=new ProfileStore(Path.Combine(root,"settings")); store.Save(p);
        Check(store.Load(exe).EffectiveStereoBackend==StereoPaths.Dxvk && store.Load(exe).GeometrySeparation==.8f,"route and calibration round-trip per executable");
        store.SaveBase(p); Check(store.BaseSettings().EffectiveStereoBackend==StereoPaths.Depth && store.BaseSettings().GeometryLaunchArguments==null,"game launch choice never becomes a global default");
        var settings=new Profile{ExecutablePath=Path.Combine(root,"other.exe"),Width=7};
        store.ApplyToAll(settings,out int skipped,out int failed);
        Check(skipped==0 && failed==0 && store.Load(exe).EffectiveStereoBackend==StereoPaths.Dxvk && store.Load(exe).GeometryLaunchArguments==p.GeometryLaunchArguments && store.Load(exe).Width==7,"Apply to all preserves each game's launch choice");
        Check(StereoPaths.Session(exe)==StereoPaths.Session(exe.ToUpperInvariant()) && Profile.ValidGeometrySession(StereoPaths.Session(exe)),"session identity is stable and valid");
        foreach(string backend in new[]{StereoPaths.Depth,StereoPaths.Vulkan,StereoPaths.Dxvk,StereoPaths.Dx12}){
            var choice=new Profile{ExecutablePath=Path.Combine(root,backend+".exe"),StereoBackend=backend,GeometryStereo=backend==StereoPaths.Depth};
            bool geometry=backend!=StereoPaths.Depth;
            Check(choice.GeometryStereo==geometry && StereoPaths.FromGeometryChoice(geometry,backend)==backend,"explicit method determines the geometry checkbox without changing the selected backend");
            store.Save(choice);var loaded=store.Load(choice.ExecutablePath);
            Check(loaded.EffectiveStereoBackend==backend && loaded.GeometryStereo==geometry,"all render methods round-trip despite a contradictory legacy checkbox");
            Check(StereoPaths.FromGeometryChoice(false,backend)==StereoPaths.Depth,"unchecking geometry selects depth estimation");
        }
        Check(StereoPaths.FromGeometryChoice(true,StereoPaths.Depth)==StereoPaths.Vulkan,"legacy checkbox still selects Vulkan");
        byte[] h=new byte[176];
        void U(int at,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(at,4),v);
        void Q(int at,ulong v)=>BinaryPrimitives.WriteUInt64LittleEndian(h.AsSpan(at,8),v);
        U(0,0x47534758);U(4,1);U(8,176);U(12,1);U(16,123);U(20,2560);U(24,1440);U(28,28);Q(40,1);U(48,2);
        Q(56,3);Q(64,9);Q(72,9);Q(80,10000);
        Check(GeometryLaunch.HeaderMatches(h,123,10020),"reuse a fresh same-frame stream for the chosen process");
        Check(!GeometryLaunch.HeaderMatches(h,124,10020) && !GeometryLaunch.HeaderMatches(h,123,12000),"reject wrong owner and stale streams");
        Q(72,10);Check(!GeometryLaunch.HeaderMatches(h,123,10020),"reject alternating eye IDs");Q(72,9);U(12,0);
        Check(!GeometryLaunch.HeaderMatches(h,123,10020),"stopped stream requires another attempt");
        string sources=Path.Combine(root,"source");Directory.CreateDirectory(sources);
        File.WriteAllText(Path.Combine(sources,"d3d11.dll"),"fixture d3d11; never loaded");File.WriteAllText(Path.Combine(sources,"dxgi.dll"),"fixture dxgi; never loaded");
        string runRoot=Path.Combine(root,"run");Directory.CreateDirectory(runRoot);
        var run=new GeometryLaunch.Run(0,0,exe,"test");File.WriteAllText(Path.Combine(runRoot,"run.json"),JsonSerializer.Serialize(run));
        GeometryLaunch.InstallFiles(sources,exe,runRoot);
        Check(File.Exists(Path.Combine(Path.GetDirectoryName(exe)!,"d3d11.dll")),"scoped install creates expected DLLs");
        File.AppendAllText(Path.Combine(Path.GetDirectoryName(exe)!,"dxgi.dll")," modified");
        GeometryLaunch.Cleanup(runRoot);
        Check(!File.Exists(Path.Combine(Path.GetDirectoryName(exe)!,"d3d11.dll")) && File.ReadAllText(Path.Combine(Path.GetDirectoryName(exe)!,"dxgi.dll")).EndsWith(" modified"),"cleanup removes only unchanged owned files");
        bool refused=false;try{GeometryLaunch.InstallFiles(sources,exe,runRoot);}catch(IOException){refused=true;}
        Check(refused && !File.Exists(Path.Combine(Path.GetDirectoryName(exe)!,"d3d11.dll")) && File.ReadAllText(Path.Combine(Path.GetDirectoryName(exe)!,"dxgi.dll")).EndsWith(" modified"),"existing DLL is preserved and a partial install rolls back");
        File.Delete(Path.Combine(Path.GetDirectoryName(exe)!,"dxgi.dll")); // own modified fixture only
        string nextRun=Path.Combine(root,"next-run"); Directory.CreateDirectory(nextRun);
        File.WriteAllText(Path.Combine(nextRun,"run.json"),JsonSerializer.Serialize(run));
        GeometryLaunch.InstallFiles(sources,exe,nextRun);
        GeometryLaunch.Cleanup(runRoot);
        Check(File.Exists(Path.Combine(Path.GetDirectoryName(exe)!,"d3d11.dll")) && File.Exists(Path.Combine(Path.GetDirectoryName(exe)!,"dxgi.dll")),"a delayed old cleanup cannot remove the next run's identical DLLs");
        GeometryLaunch.Cleanup(nextRun);
        string steamapps=Path.Combine(root,"SteamLibrary","steamapps"), steamExe=Path.Combine(steamapps,"common","TestGame","bin","game.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(steamExe)!);File.WriteAllText(Path.Combine(steamapps,"appmanifest_999.acf"),"\"appid\" \"999\"\n\"installdir\" \"TestGame\"");
        Check(GeometryLaunch.FindSteamAppId(steamExe)=="999","resolve a Steam installation by its manifest, not its game title");
        using var current=Process.GetCurrentProcess();string self=Environment.ProcessPath!;
        var app=new RunningApp(current.Id,Path.GetFileName(self),self,[new GameWindow(42,"Smoke fixture")],null);
        var trial=new Profile{ExecutablePath=self};trial.SelectStereoBackend(StereoPaths.Vulkan);
        var plan=await GeometryLaunch.PrepareWithLayer(app,trial,root,_=>{},CancellationToken.None,root);
        Check(plan.Arguments.Contains("--smoke-test") && current.Id==plan.App.Pid && !current.HasExited,"preflight preserves a real command line without closing the process");
        var dialog=new GeometryLaunchWindow(plan,trial) { ShowActivated=false,WindowStartupLocation=System.Windows.WindowStartupLocation.Manual,Left=-20000,Top=-20000 };
        try {
            dialog.Show(); dialog.UpdateLayout();
            var bitmap=new System.Windows.Media.Imaging.RenderTargetBitmap((int)dialog.ActualWidth,(int)dialog.ActualHeight,96,96,System.Windows.Media.PixelFormats.Pbgra32);
            bitmap.Render(dialog);
            var encoder=new System.Windows.Media.Imaging.PngBitmapEncoder(); encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
            using var image=File.Create(Path.Combine(output,"ui-geometry-restart.png")); encoder.Save(image);
        } finally { dialog.Close(); }
        Check(!current.HasExited,"displaying and dismissing restart details is passive");
        await NativeGeometryLaunchTests.Run(root,app);
        File.WriteAllText(Path.Combine(output,"geometry-launch-pass.txt"),"PASS: profile migration and per-game persistence; base/apply-all isolation; literal Windows arguments; API switch handling; scoped DLL install/rollback and hash-preserving cleanup; Steam manifest resolution; fresh same-frame stream owner validation; real command-line preflight without restarting a game.");
    }
}
