using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Buffers.Binary;

namespace VRX.Desktop;

// Runs only under --smoke-test. Uses disposable files and this smoke process;
// never closes a real game, starts a renderer, or installs a real game DLL.
internal static class GeometryLaunchTests
{
    // A minimal x64 PE image with the given import and delay-load import tables (never run).
    internal static void FakePe(string path,string[] imports,string[] delayImports)
    {
        ArgumentNullException.ThrowIfNull(path);
        byte[] pe=new byte[0x400];
        void U16(int at,int v)=>BinaryPrimitives.WriteUInt16LittleEndian(pe.AsSpan(at),(ushort)v);
        void U32(int at,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(pe.AsSpan(at),v);
        U16(0,0x5A4D); U32(0x3C,0x40); U32(0x40,0x4550); U16(0x44,0x8664); U16(0x46,1); U16(0x54,240);
        const int optional=0x58, section=optional+240, raw=0x200; const uint va=0x1000;
        U16(optional,0x20B); U32(optional+108,16);
        int descriptors=raw, delays=descriptors+20*(imports.Length+1), names=delays+32*(delayImports.Length+1);
        uint Rva(int offset)=>(uint)(offset-raw)+va;
        int Write(string text) { int at=names; foreach(char c in text) pe[names++]=(byte)c; pe[names++]=0; return at; }
        for(int i=0;i<imports.Length;i++) U32(descriptors+20*i+12,Rva(Write(imports[i])));
        for(int i=0;i<delayImports.Length;i++) { U32(delays+32*i,1); U32(delays+32*i+4,Rva(Write(delayImports[i]))); }
        if(imports.Length>0) { U32(optional+112+8,Rva(descriptors)); U32(optional+112+12,(uint)(20*(imports.Length+1))); }
        if(delayImports.Length>0) { U32(optional+112+13*8,Rva(delays)); U32(optional+112+13*8+4,(uint)(32*(delayImports.Length+1))); }
        for(int i=0;i<5;i++) pe[section+i]=(byte)".text"[i];
        U32(section+8,0x200); U32(section+12,va); U32(section+16,0x200); U32(section+20,raw);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path,pe);
    }

    // Automatic renderer: the API from the executable's imports (or a known engine DLL), the
    // route that follows, and no guess when it is unknown, unsupported or ambiguous.
    private static void GraphicsApiChecks(string root,ProfileStore store,Action<bool,string> Check)
    {
        System.Diagnostics.Debug.WriteLine("[Smoke] GraphicsApiChecks enter");
        string api=Path.Combine(root,"api");
        string Exe(string name,string[] imports,string[]? delay=null) { string p=Path.Combine(api,name,name+".exe"); FakePe(p,imports,delay ?? []); return p; }
        string dx11=Exe("dx11",["KERNEL32.dll","dxgi.dll","d3d11.dll"]), dx12=Exe("dx12",["d3d12.dll","dxgi.dll"]), vk=Exe("vk",["vulkan-1.dll"]);
        string dx9=Exe("dx9",["d3d9.dll"]), dxgiOnly=Exe("dxgi",["dxgi.dll"]), both=Exe("both",[],["d3d11.dll","d3d12.dll"]);
        string unreal=Path.Combine(api,"ue","Game-Win64-Shipping.exe"); FakePe(unreal,["KERNEL32.dll"],["d3d11.dll","d3d12.dll"]);
        string unity=Exe("unity",["KERNEL32.dll"]); FakePe(Path.Combine(api,"unity","UnityPlayer.dll"),["d3d11.dll","dxgi.dll"],[]);
        Check(GraphicsApi.Imports(dx11).Contains("d3d11.dll") && GraphicsApi.Imports(both).SequenceEqual(new[]{"d3d11.dll","d3d12.dll"}),"PE import and delay-load tables are read");
        Check(GraphicsApi.Detect(dx11).Renderer==StereoPaths.Dxvk && GraphicsApi.Detect(dx12).Renderer==StereoPaths.Vkd3d && GraphicsApi.Detect(vk).Renderer==StereoPaths.Vulkan,"Direct3D 11 uses DXVK, Direct3D 12 vkd3d-proton, Vulkan the plain layer");
        Check(GraphicsApi.Detect(unity).Renderer==StereoPaths.Dxvk && GraphicsApi.Detect(unreal).Renderer==StereoPaths.Dxvk,"a Unity player DLL and an Unreal executable with several APIs are recognised");
        Check(GraphicsApi.Detect(dx9).Renderer=="" && GraphicsApi.Explain(GraphicsApi.Detect(dx9)).Contains(GraphicsApi.D3D9),"Direct3D 9 is reported as unsupported");
        Check(GraphicsApi.Detect(dxgiOnly).Renderer=="" && GraphicsApi.Detect(both).Renderer=="" && GraphicsApi.Detect(Path.Combine(api,"missing.exe")).Renderer=="","an unknown or ambiguous API is not guessed");
        var auto=new Profile{ExecutablePath=dx11}; auto.SelectStereoBackend(StereoPaths.GeometryAuto);
        Check(auto.GeometryStereo && auto.EffectiveStereoBackend==StereoPaths.Dxvk,"automatic geometry on a Direct3D 11 game resolves to DXVK");
        var unknown=new Profile{ExecutablePath=dxgiOnly}; unknown.SelectStereoBackend(StereoPaths.GeometryAuto);
        Check(unknown.GeometryStereo && unknown.EffectiveStereoBackend==StereoPaths.GeometryAuto,"an unknown API stays geometry-chosen but unresolved, never depth or Vulkan");
        bool explained=false;
        try { GeometryLaunch.PrepareWithLayer(new RunningApp(0,"dxgi.exe",dxgiOnly,[],null),unknown,root,_=>{},CancellationToken.None,root,[]).GetAwaiter().GetResult(); }
        catch(InvalidOperationException ex) { explained=ex.Message==GraphicsApi.Explain(GraphicsApi.Detect(dxgiOnly)); }
        Check(explained,"launching with an unknown API says so instead of failing after 45 s");
        System.Diagnostics.Debug.WriteLine("[Smoke] GraphicsApiChecks exit");
    }

    // Attaching to a running game: geometry only from a ready stream of a run VRX started (or the
    // run it just launched); otherwise estimated depth, with the restart offered only when true
    // geometry is the saved method in Expert. The decision never changes the saved method.
    private static void AttachDecisionChecks(Action<bool,string> Check)
    {
        ArgumentNullException.ThrowIfNull(Check);
        Debug.WriteLine("[Smoke] AttachDecisionChecks enter");
        var depth=new Profile(); depth.SelectStereoBackend(StereoPaths.Depth);
        var auto=new Profile(); auto.SelectStereoBackend(StereoPaths.Auto);
        var geometry=new Profile(); geometry.SelectStereoBackend(StereoPaths.GeometryAuto);
        var dxvk=new Profile(); dxvk.SelectStereoBackend(StereoPaths.Dxvk);
        const GeometryLaunch.AttachStereo Depth=GeometryLaunch.AttachStereo.Depth, Hint=GeometryLaunch.AttachStereo.DepthWithGeometryHint,
            Stream=GeometryLaunch.AttachStereo.GeometryStream, Launched=GeometryLaunch.AttachStereo.GeometryLaunched;
        GeometryLaunch.AttachStereo Decide(Profile p,bool expert=true,bool depthOnly=false,bool display=false,bool launched=false,bool stream=false)=>
            GeometryLaunch.DecideAttach(p,expert,depthOnly,display,launched,stream);
        Check(Decide(depth)==Depth && Decide(auto)==Depth && Decide(depth,stream:true)==Depth,"estimated depth and Auto always attach with depth, with no restart offer");
        Check(Decide(geometry)==Hint && Decide(dxvk)==Hint,"true geometry on a game VRX did not start attaches with depth and offers a restart");
        Check(Decide(geometry,stream:true)==Stream && Decide(dxvk,stream:true)==Stream,"a ready stream from a VRX geometry run is attached to, with no restart");
        Check(Decide(geometry,launched:true)==Launched && Decide(geometry,launched:true,stream:true)==Launched,"the run VRX just launched waits for its first frame");
        Check(Decide(geometry,expert:false)==Depth && Decide(geometry,expert:false,stream:true)==Depth,"Easy always attaches with depth, with no restart offer");
        Check(Decide(geometry,depthOnly:true)==Depth && Decide(geometry,depthOnly:true,stream:true)==Depth,"\"Use estimated depth\" attaches with depth, with no restart offer");
        Check(Decide(geometry,display:true)==Depth && Decide(geometry,display:true,stream:true)==Depth,"a whole display attaches with depth, with no restart offer");
        Check(geometry.SelectedStereoBackend==StereoPaths.GeometryAuto && dxvk.SelectedStereoBackend==StereoPaths.Dxvk,"deciding an attach leaves the saved stereo method unchanged");
        bool refused=false;
        try { GeometryLaunch.DecideAttach(null!,true,false,false,false,false); } catch(ArgumentNullException) { refused=true; }
        Check(refused,"a missing profile is refused");
        Debug.WriteLine("[Smoke] AttachDecisionChecks exit");
    }

    // Crash-proof cleanup: the manifest's intent records, the guard (in process and as the real
    // detached --geometry-guard process) and run-folder pruning. Fixture files only.
    private static async Task GuardAndCleanupChecks(string root,string exe,string sources,Action<bool,string> Check)
    {
        System.Diagnostics.Debug.WriteLine("[Smoke] GuardAndCleanupChecks enter");
        string game=Path.GetDirectoryName(exe)!;
        string data=Path.Combine(root,"guard-data"), runs=Path.Combine(data,"geometry","runs");
        string NewRun(GeometryLaunch.Run run) {
            string folder=Path.Combine(runs,Guid.NewGuid().ToString("N")); Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder,"run.json"),JsonSerializer.Serialize(run)); return folder;
        }
        // A crash between recording the intent and finishing the copy: the manifest names the
        // temporary copy and the target, and cleanup removes both (the target only by hash).
        string crashed=NewRun(new GeometryLaunch.Run(0,0,exe,"test"));
        string temporary=Path.Combine(game,"d3d11.dll.vrx-"+Guid.NewGuid().ToString("N")+".tmp");
        File.Copy(Path.Combine(sources,"d3d11.dll"),temporary);
        File.Copy(Path.Combine(sources,"dxgi.dll"),Path.Combine(game,"dxgi.dll"));
        string Sha(string file)=>GeometryRuntime.Hash(file);
        File.WriteAllText(Path.Combine(crashed,"installed.json"),JsonSerializer.Serialize(new[]{
            new GeometryLaunch.InstalledFile(Path.Combine(game,"d3d11.dll"),Sha(Path.Combine(sources,"d3d11.dll")),temporary),
            new GeometryLaunch.InstalledFile(Path.Combine(game,"dxgi.dll"),Sha(Path.Combine(sources,"dxgi.dll")))}));
        GeometryLaunch.Cleanup(crashed);
        Check(!File.Exists(temporary) && !File.Exists(Path.Combine(game,"dxgi.dll")) && GeometryLaunch.Retired(crashed),"files recorded before a crash are all removed, including an unfinished copy");
        string escape=NewRun(new GeometryLaunch.Run(0,0,exe,"test"));
        File.WriteAllText(Path.Combine(escape,"installed.json"),JsonSerializer.Serialize(new[]{new GeometryLaunch.InstalledFile(Path.Combine(game,"d3d11.dll"),"00",Path.Combine(root,"elsewhere.tmp"))}));
        GeometryLaunch.Cleanup(escape);
        Check(!GeometryLaunch.Retired(escape),"a manifest naming a file outside the game's folder is refused");
        // The guard, in this process with no waiting: the game never started, so it cleans.
        string guarded=NewRun(new GeometryLaunch.Run(0,0,exe,"test"));
        GeometryLaunch.InstallFiles(sources,exe,guarded);
        Check(GeometryGuard.ValidRunFolder(guarded) && !GeometryGuard.ValidRunFolder(game) && await GeometryGuard.Run(game)==2,"the guard accepts only geometry run folders");
        Check(await GeometryGuard.Run(guarded,TimeSpan.Zero,TimeSpan.Zero,TimeSpan.FromMilliseconds(20),CancellationToken.None)==0 &&
            !File.Exists(Path.Combine(game,"d3d11.dll")) && GeometryLaunch.Retired(guarded) && !GeometryGuard.Waiting(guarded),"the guard removes a run's files once the game has gone");
        // The real detached guard process: a run whose game has already exited is cleaned
        // after the relaunch grace, with no VRX window and independent of this process.
        string detached=NewRun(new GeometryLaunch.Run(int.MaxValue-7,1,exe,"test"));
        GeometryLaunch.InstallFiles(sources,exe,detached);
        var guardStart=new ProcessStartInfo(Environment.ProcessPath!){UseShellExecute=false,CreateNoWindow=true};
        guardStart.ArgumentList.Add(GeometryGuard.Switch+detached);
        using(var guard=Process.Start(guardStart)!) {
            await guard.WaitForExitAsync().WaitAsync(GeometryGuard.RelaunchGrace+TimeSpan.FromSeconds(30));
            Check(guard.ExitCode==0 && !File.Exists(Path.Combine(game,"d3d11.dll")) && !File.Exists(Path.Combine(game,"dxgi.dll")) && GeometryLaunch.Retired(detached),"the detached --geometry-guard process cleans up after the game exits");
        }
        // Pruning: retired folders beyond the newest few go; a folder still owning files stays.
        for(int i=0;i<GeometryLaunch.KeptRuns+3;++i) {
            string old=NewRun(new GeometryLaunch.Run(0,0,exe,"test")); File.WriteAllText(Path.Combine(old,"installed.json"),"[]");
            Directory.SetLastWriteTimeUtc(old,DateTime.UtcNow.AddMinutes(-60+i));
        }
        string owning=NewRun(new GeometryLaunch.Run(0,0,exe,"test"));
        File.WriteAllText(Path.Combine(owning,"installed.json"),JsonSerializer.Serialize(new[]{new GeometryLaunch.InstalledFile(Path.Combine(game,"d3d11.dll"),"00")}));
        Directory.SetLastWriteTimeUtc(owning,DateTime.UtcNow.AddDays(-30));
        int before=Directory.GetDirectories(runs).Length;
        GeometryLaunch.PruneRuns(data);
        Check(Directory.Exists(owning) && Directory.GetDirectories(runs).Length==GeometryLaunch.KeptRuns+2 && before>GeometryLaunch.KeptRuns+2,"old retired run folders are deleted, keeping the newest few and any folder still owning files");
        System.Diagnostics.Debug.WriteLine("[Smoke] GuardAndCleanupChecks exit");
    }

    public static async Task Run(string output)
    {
        static void Check(bool value,string reason) { if(!value) throw new Exception("Geometry launch: "+reason); }
        string root=Path.Combine(output,"geometry-launch-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        string exe=Path.Combine(root,"game","fake-vrx-probe.exe"); Directory.CreateDirectory(Path.GetDirectoryName(exe)!); File.WriteAllText(exe,"fixture");
        AttachDecisionChecks(Check);
        string[] args=["", "space in argument","literal\"quote", @"C:\Games\trailing\", "semi;colon", "$literal", "--value=a b", "\u00e9"];
        Check(GeometryLaunch.ParseArguments(string.Join(' ',args.Select(GeometryLaunch.QuoteArgument))).SequenceEqual(args),"Windows argument quoting preserves literal values");
        Check(GeometryLaunch.ApiArguments(["-dx12","-windowed","-ResX=2560"],StereoPaths.Vulkan,true).SequenceEqual(new[]{"-windowed","-ResX=2560","-vulkan"}),"native Vulkan replaces only the old API switch");
        Check(GeometryLaunch.ApiArguments(["-vulkan","--save=a b"],StereoPaths.Dxvk,true).SequenceEqual(new[]{"--save=a b","-dx11"}),"DXVK retains unrelated arguments");
        Check(GeometryLaunch.ApiArguments(["--custom-renderer=vk"],StereoPaths.Vulkan,false).SequenceEqual(new[]{"--custom-renderer=vk"}),"unknown engines do not receive invented switches");
        Check(GeometryLaunch.ApiArguments(["-dx12","--custom"],StereoPaths.Vulkan,false).SequenceEqual(new[]{"-dx12","--custom"}),"unknown engines preserve existing API switches for review");
        // The fixed Steam launcher: which Launch Options are old direct ones, which copies may
        // take Steam launches, and reading Launch Options from a Steam-shaped localconfig.vdf.
        Check(SteamLauncher.IsDirectOption(@"""C:\dev\VRX\desktop\VRX.Desktop\bin\Release\net10.0-windows\VRX.Desktop.exe"" --steam-game 482400 -- %command%"),"a Launch Option naming a VRX folder is direct");
        Check(!SteamLauncher.IsDirectOption(@"""C:\Users\u\AppData\Local\VRX\launcher\vrx-steam-launch.exe"" --steam-game 482400 -- %command%"),"the fixed launcher is not direct");
        Check(!SteamLauncher.IsDirectOption("-novid -dx11") && !SteamLauncher.IsDirectOption(null),"other Launch Options are left alone");
        string copy=Path.Combine(root,"copy"); Directory.CreateDirectory(Path.Combine(copy,"geometry","layer"));
        string copyExe=Path.Combine(copy,"VRX.Desktop.exe"); File.WriteAllText(copyExe,"fixture");
        Check(!SteamLauncher.CanTakeLaunches(copyExe,copy),"a copy without the launcher and runtime does not take Steam launches");
        File.WriteAllText(Path.Combine(copy,SteamLauncher.FileName),"fixture");
        File.WriteAllText(Path.Combine(copy,"geometry","layer",GeometryRuntime.LayerLibrary),"fixture");
        File.WriteAllText(Path.Combine(copy,"geometry","layer",GeometryRuntime.LayerManifest),"{}");
        Check(SteamLauncher.CanTakeLaunches(copyExe,copy),"a complete copy takes Steam launches");
        Check(!SteamLauncher.CanTakeLaunches(Path.Combine(copy,"other.exe"),copy),"only VRX.Desktop.exe takes Steam launches");
        string steamRoot=Path.Combine(root,"steam"), config=Path.Combine(steamRoot,"userdata","12345","config");
        Directory.CreateDirectory(config);
        File.WriteAllText(Path.Combine(config,"localconfig.vdf"),"\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"482400\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LaunchOptions\"\t\t\"\\\"C:\\\\VRX\\\\VRX.Desktop.exe\\\" --steam-game 482400 -- %command%\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n");
        Check(SteamLibrary.LaunchOptionsFor("482400",steamRoot)=="\"C:\\VRX\\VRX.Desktop.exe\" --steam-game 482400 -- %command%","Steam's saved Launch Options are read");
        Check(SteamLibrary.LaunchOptionsFor("570",steamRoot)==null && SteamLibrary.LaunchOptionsFor("bad",steamRoot)==null,"absent games and bad ids read as none");
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
#if DEBUG
        string[] backends=[StereoPaths.Depth,StereoPaths.Vulkan,StereoPaths.Dxvk,StereoPaths.Dx12];
#else
        string[] backends=[StereoPaths.Depth,StereoPaths.Vulkan,StereoPaths.Dxvk];
        // The native DX12 proxy is development-only: a saved DX12 choice loads as depth in a release.
        var native=new Profile{ExecutablePath=Path.Combine(root,"dx12.exe"),StereoBackend=StereoPaths.Dx12}; store.Save(native);
        Check(store.Load(native.ExecutablePath).EffectiveStereoBackend==StereoPaths.Depth && !StereoPaths.Geometry(StereoPaths.Dx12),"a release never uses the native DX12 route");
        Check(GeometryLaunch.InstalledNames(StereoPaths.Dx12).Length==0,"a release never installs the native DX12 proxy");
#endif
        foreach(string backend in backends){
            var choice=new Profile{ExecutablePath=Path.Combine(root,backend+".exe"),StereoBackend=backend,GeometryStereo=backend==StereoPaths.Depth};
            bool geometry=backend!=StereoPaths.Depth;
            Check(choice.GeometryStereo==geometry && StereoPaths.FromGeometryChoice(geometry,backend)==backend,"explicit method determines the geometry checkbox without changing the selected backend");
            store.Save(choice);var loaded=store.Load(choice.ExecutablePath);
            Check(loaded.EffectiveStereoBackend==backend && loaded.GeometryStereo==geometry,"all render methods round-trip despite a contradictory legacy checkbox");
            Check(StereoPaths.FromGeometryChoice(false,backend)==StereoPaths.Depth,"unchecking geometry selects depth estimation");
        }
        Check(StereoPaths.FromGeometryChoice(true,StereoPaths.Depth)==StereoPaths.Vulkan,"legacy checkbox still selects Vulkan");
        var separate=new Profile{ExecutablePath=Path.Combine(root,"separate.exe"),GeometrySeparation=.5f};
        separate.SelectStereoBackend(StereoPaths.Vkd3d); separate.SelectStereoBackend(StereoPaths.Depth); store.Save(separate);
        separate=store.Load(separate.ExecutablePath);
        Check(separate.SelectedStereoBackend==StereoPaths.Depth && separate.SelectedGeometryRenderer==StereoPaths.Vkd3d && separate.GeometrySeparation==.5f,"secondary renderer and stereo tuning survive switching to depth and reloading");
        separate.SelectStereoBackend(separate.SelectedGeometryRenderer);
        Check(separate.EffectiveStereoBackend==StereoPaths.Vkd3d,"returning to geometry restores its renderer");
        store.Save(separate); store.SaveBase(separate);
        Check(store.BaseSettings().SelectedGeometryRenderer==StereoPaths.GeometryAuto,"base settings never inherit a game's secondary renderer");
        store.ApplyToAll(settings,out _,out _);
        Check(store.Load(separate.ExecutablePath).SelectedGeometryRenderer==StereoPaths.Vkd3d,"Apply to all preserves secondary renderer selection");
        Check(GeometryLaunch.ApiArguments(["-dx11","-norhithread"],StereoPaths.Vkd3d,true).SequenceEqual(new[]{"-norhithread","-dx12"}),"vkd3d selects D3D12 and retains the tested thread setting");
        string shockApps=Path.Combine(root,"shock","steamapps"), shockExe=Path.Combine(shockApps,"common","System Shock","SystemShock","Binaries","Win64","SystemReShock-Win64-Shipping.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(shockExe)!);
        File.WriteAllText(Path.Combine(shockApps,"appmanifest_482400.acf"),"\"appid\" \"482400\"\n\"installdir\" \"System Shock\"");
        var tested=store.Load(shockExe);
        // Auto is always estimated depth, even for a game with tested geometry settings.
        Check(tested.SelectedStereoBackend==StereoPaths.Auto && tested.EffectiveStereoBackend==StereoPaths.Depth && !tested.GeometryStereo,"a new System Shock profile resolves to depth");
        Check(tested.GeometryProfileId==TestedStereoProfiles.SystemShock && tested.GeometrySeparation==TestedStereoProfiles.SystemShockSeparation &&
            tested.GeometryConvergence==TestedStereoProfiles.SystemShockConvergence && tested.GeometryConvergence>0 && tested.GeometryLaunchArguments!.Contains("-norhithread"),"the tested System Shock calibration is only a suggestion, with the screen plane in front of infinity");
        var chosen=store.Load(shockExe); chosen.SelectStereoBackend(StereoPaths.GeometryAuto);
        Check(chosen.EffectiveStereoBackend==StereoPaths.Vkd3d,"choosing geometry for System Shock uses its tested renderer");
        tested.GeometrySeparation=.35f; tested.GeometryConvergence=.004f; store.Save(tested);
        var remembered=store.Load(shockExe);
        Check(remembered.GeometrySeparation==.35f && remembered.GeometryConvergence==.004f && remembered.SelectedStereoBackend==StereoPaths.Auto,"reloading Auto preserves user calibration");
        Check(!remembered.GeometryLaunchArguments!.Contains("-Res") && !remembered.GeometryLaunchArguments.Contains("-windowed"),"the game controls its saved resolution and display mode");
        remembered.GeometryProfileId="system-shock-vkd3d-v1";
        remembered.GeometryLaunchArguments="-norhithread -windowed -ResX=1280 -ResY=720 -dx12";
        store.Save(remembered); remembered=store.Load(shockExe);
        Check(remembered.GeometryProfileId==TestedStereoProfiles.SystemShock && remembered.GeometrySeparation==.35f && remembered.GeometryLaunchArguments=="-dx12 -norhithread","migrate the old test resolution without changing stereo tuning");
        remembered.GeometryProfileId="system-shock-vkd3d-v2"; remembered.GeometryLaunchArguments="-norhithread -windowed -dx12";
        store.Save(remembered); remembered=store.Load(shockExe);
        Check(remembered.GeometryLaunchArguments=="-dx12 -norhithread" && remembered.GeometryConvergence==.004f,"remove the v2 forced window mode while preserving calibration");
        remembered.GeometryProfileId="system-shock-vkd3d-v3"; remembered.GeometryConvergence=0; store.Save(remembered); remembered=store.Load(shockExe);
        Check(remembered.GeometryProfileId==TestedStereoProfiles.SystemShock && remembered.GeometryConvergence==TestedStereoProfiles.SystemShockConvergence && remembered.GeometrySeparation==.35f,"the v3 seed's convergence 0 (HUD behind the scene) moves to the tested screen plane once");
        remembered.GeometryConvergence=0; store.Save(remembered);
        Check(store.Load(shockExe).GeometryConvergence==0,"a convergence the player sets after the migration is kept");
        var unsafeTuning=store.Load(shockExe); unsafeTuning.GeometrySeparation=40; unsafeTuning.GeometryConvergence=.2f; store.Save(unsafeTuning);
        Check(store.Load(shockExe).GeometrySeparation==Profile.MaxGeometrySeparation && store.Load(shockExe).GeometryConvergence==Profile.MaxGeometryConvergence,"saved tuning outside the safe range is clamped when it loads");
        Check(!Profile.SafeGeometryTuning(5,.001f) && !Profile.SafeGeometryTuning(.5f,.02f) && !Profile.SafeGeometryTuning(0,0) && Profile.SafeGeometryTuning(.5f,0),"typed tuning outside the safe range is refused");
        remembered.GeometryConvergence=.004f; store.Save(remembered);
        remembered.GeometryProfileId="system-shock-vkd3d-v2"; remembered.GeometryLaunchArguments="-dx12 -norhithread -windowed -ResX=2560 -ResY=1440";
        store.Save(remembered); remembered=store.Load(shockExe);
        Check(remembered.GeometryLaunchArguments!.Contains("-ResX=2560"),"explicit custom launch settings survive the default-profile migration");
        remembered.SelectStereoBackend(StereoPaths.Depth); store.Save(remembered);
        Check(store.Load(shockExe).EffectiveStereoBackend==StereoPaths.Depth,"an explicit depth choice overrides the built-in profile");
        var unknown=store.Load(Path.Combine(root,"unknown.exe"));
        Check(unknown.SelectedStereoBackend==StereoPaths.Auto && unknown.EffectiveStereoBackend==StereoPaths.Depth,"unknown games default to external depth capture");
        var translated=new GeometryLaunch.Plan(new RunningApp(1,"fixture",exe,[],null),0,StereoPaths.Vkd3d,root,[],root,"482400","fixture",root,[],root);
        var start=new ProcessStartInfo(exe); start.Environment["VKD3D_CONFIG"]="existing";
        start.Environment["__COMPAT_LAYER"]="RUNASINVOKER DPIUNAWARE";
        string shaderCache=GeometryLaunch.CacheFolder(root,"fixture");
        GeometryLaunch.ConfigureEnvironment(start,translated,tested,root,shaderCache);
        Check(start.Environment["VKD3D_SHADER_CACHE_PATH"]==shaderCache && start.Environment["DXVK_STATE_CACHE_PATH"]==shaderCache && start.Environment["VKD3D_LOG_FILE"]!.StartsWith(root,StringComparison.OrdinalIgnoreCase) &&
            shaderCache.EndsWith(Path.Combine("geometry","cache","fixture"),StringComparison.OrdinalIgnoreCase),"shader caches persist per game while logs stay in the run folder");
#if !DEBUG
        Check(!start.Environment.ContainsKey("VKD3D_DEBUG"),"a release never turns on verbose vkd3d logging");
#endif
        Check(start.Environment["__COMPAT_LAYER"]!.Contains("HIGHDPIAWARE") && start.Environment["__COMPAT_LAYER"]!.Contains("RUNASINVOKER") &&
            !start.Environment["__COMPAT_LAYER"]!.Contains("DPIUNAWARE"),"the tested game uses native pixels on a scaled desktop while preserving other compatibility options");
        Check(start.Environment["VKD3D_CONFIG"]!.Contains("single_queue") && start.Environment["VKD3D_CONFIG"]!.Contains("existing") &&
            start.Environment["VKD3D_DISABLE_EXTENSIONS"]!.Contains("VK_EXT_shader_module_identifier") && start.Environment["VKD3D_DISABLE_EXTENSIONS"]!.Contains("VK_KHR_pipeline_binary") &&
            start.Environment["VRX_XG_EYE"]=="mirror" && start.Environment["VRX_XG_TRANSPORT"]=="gpu" && start.Environment["SteamAppId"]=="482400", "translated launch preserves the tested same-frame GPU and shader-cache environment");
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
        Check(!Directory.GetFiles(Path.GetDirectoryName(exe)!,"*.vrx-*.tmp").Any(),"a rolled-back install leaves no temporary copies");
        await GuardAndCleanupChecks(root,exe,sources,Check);
        GraphicsApiChecks(root,store,Check);
        string translator=Path.Combine(root,"translator"); Directory.CreateDirectory(translator);
        foreach(string name in new[]{"d3d12.dll","d3d12core.dll"}) File.WriteAllText(Path.Combine(translator,name),"translator fixture; never loaded");
        string translatedRun=Path.Combine(root,"translated-run"); Directory.CreateDirectory(translatedRun);
        File.WriteAllText(Path.Combine(translatedRun,"run.json"),JsonSerializer.Serialize(run with {Backend=StereoPaths.Vkd3d}));
        var installed=GeometryLaunch.InstallFiles(sources,exe,translatedRun,StereoPaths.Vkd3d,translator);
        Check(installed.Count==4 && installed.All(f=>File.Exists(f.Path)),"translated D3D12 installs both translator and matching D3D11/DXGI components");
        GeometryLaunch.Cleanup(translatedRun);
        Check(installed.All(f=>!File.Exists(f.Path)),"translated cleanup retires all four owned DLLs");
        Check(GeometryRuntime.PinnedHash("dxvk","d3d11.dll")=="523DA2CD765DD51D99FE1644B24F9EE7EE6247E4C98953BCC449CE84CD303405" && GeometryRuntime.PinnedHash("dxvk","dxgi.dll")?.Length==64,"DXVK 2.7.1's DLLs are pinned");
        bool unpinnedRefused=false;
        try { GeometryRuntime.VerifySource("dxvk","d3d11.dll",Path.Combine(sources,"d3d11.dll")); } catch(InvalidDataException) { unpinnedRefused=true; }
        Check(unpinnedRefused,"a DLL that does not match its pin is refused before it is copied into a game");
        string steamapps=Path.Combine(root,"SteamLibrary","steamapps"), steamExe=Path.Combine(steamapps,"common","TestGame","bin","game.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(steamExe)!);File.WriteAllText(Path.Combine(steamapps,"appmanifest_999.acf"),"\"appid\" \"999\"\n\"installdir\" \"TestGame\"");
        Check(GeometryLaunch.FindSteamAppId(steamExe)=="999","resolve a Steam installation by its manifest, not its game title");
        using var current=Process.GetCurrentProcess();string self=Environment.ProcessPath!;
        string liveRoot=Path.Combine(root,"live-tuning"),liveRun=Path.Combine(liveRoot,"geometry","runs","fixture");Directory.CreateDirectory(liveRun);
        var liveProfile=new Profile {ExecutablePath=self,GeometrySession="live-fixture",GeometrySeparation=.65f,GeometryConvergence=.01f};liveProfile.SelectStereoBackend(StereoPaths.Vkd3d);
        var liveIdentity=new GeometryLaunch.Run(current.Id,current.StartTime.ToUniversalTime().Ticks,self,liveProfile.GeometrySession,Backend:StereoPaths.Vkd3d);
        ProfileStore.AtomicWrite(Path.Combine(liveRun,"run.json"),JsonSerializer.Serialize(liveIdentity));
        string liveFile=Path.Combine(liveRun,"stereo.txt"),liveState=liveFile+".live.json";
        ProfileStore.AtomicWrite(liveFile,"0.5 0\n");
        Check(GeometryLaunch.UpdateTuning(liveProfile,liveRoot)==GeometryLaunch.TuningUpdate.RestartRequired && File.ReadAllText(liveFile)=="0.5 0\n","an older running layer requires one upgrade restart and is not sent unsupported updates");
        var acknowledgement=new GeometryLaunch.LiveTuningState(1,current.Id,.5f,0,0,true);
        ProfileStore.AtomicWrite(liveState,JsonSerializer.Serialize(acknowledgement with {Pid=current.Id+1}));
        Check(GeometryLaunch.UpdateTuning(liveProfile,liveRoot)==GeometryLaunch.TuningUpdate.RestartRequired && File.ReadAllText(liveFile)=="0.5 0\n","another process cannot advertise live capability for this run");
        ProfileStore.AtomicWrite(liveState,JsonSerializer.Serialize(acknowledgement));
        Check(GeometryLaunch.UpdateTuning(liveProfile,liveRoot)==GeometryLaunch.TuningUpdate.Pending && File.ReadAllText(liveFile)=="0.65 0.01\n","desktop sends both values atomically using invariant formatting and waits for the renderer");
        ProfileStore.AtomicWrite(liveState,JsonSerializer.Serialize(acknowledgement with {Separation=.65f,Convergence=.01f,Revision=1}));
        Check(GeometryLaunch.UpdateTuning(liveProfile,liveRoot,false)==GeometryLaunch.TuningUpdate.Applied,"applied feedback requires the renderer's acknowledgement of both values");
        ProfileStore.AtomicWrite(Path.Combine(liveRun,"run.json"),JsonSerializer.Serialize(liveIdentity with {StartTicks=0}));
        Check(GeometryLaunch.UpdateTuning(liveProfile,liveRoot)==GeometryLaunch.TuningUpdate.NextLaunch,"stale or reused process IDs cannot receive live updates");
        var app=new RunningApp(current.Id,Path.GetFileName(self),self,[new GameWindow(42,"Smoke fixture")],null);
        var trial=new Profile{ExecutablePath=self};trial.SelectStereoBackend(StereoPaths.Vulkan);
        Check(GeometryLaunch.DecideAttach(trial,true,false,false,false,GeometryLaunch.ExistingSession(app,trial,root)!=null)==GeometryLaunch.AttachStereo.DepthWithGeometryHint,
            "a running process VRX did not start with geometry has no stream, so attaching uses depth and offers the restart");
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
#if DEBUG
        await NativeGeometryLaunchTests.Run(root,app);
#endif
        File.WriteAllText(Path.Combine(output,"geometry-guard-pass.txt"),"PASS: install records intent before copying; crash-left temporary copies and DLLs removed by hash; detached --geometry-guard process cleans after the game's folder is quiet; invalid guard folders refused; retired run folders pruned to the newest few, never one in use; per-game shader cache.");
        File.WriteAllText(Path.Combine(output,"geometry-launch-pass.txt"),"PASS: profile migration and per-game persistence; base/apply-all isolation; literal Windows arguments; API switch handling; scoped DLL install/rollback and hash-preserving cleanup; Steam manifest resolution; fresh same-frame stream owner validation; real command-line preflight without restarting a game.");
    }
}
