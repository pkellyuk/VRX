using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace VRX.Desktop;

internal static class SteamLibraryTests
{
    public static async Task Run(string output,Window owner)
    {
        static void Check(bool ok,string reason) {if(!ok) throw new Exception("Steam library: "+reason);}
        string root=Path.Combine(output,"steam-fixture-"+Guid.NewGuid().ToString("N"));
        string steam=Path.Combine(root,"Steam"), extra=Path.Combine(root,"Other library");
        Directory.CreateDirectory(Path.Combine(steam,"steamapps")); Directory.CreateDirectory(Path.Combine(extra,"steamapps","common","Test game"));
        string Escape(string value)=>value.Replace("\\","\\\\").Replace("\"","\\\"");
        File.WriteAllText(Path.Combine(steam,"steamapps","libraryfolders.vdf"),"// fixture\n\"libraryfolders\" { \"0\" { \"path\" \""+Escape(steam)+"\" } \"1\" { \"path\" \""+Escape(extra)+"\" } \"2\" { \"path\" \""+Escape(extra.ToUpperInvariant())+"\" } }");
        string Manifest(string id,string name,string dir,string flags="4")=>"\"AppState\" { \"appid\" \""+id+"\" \"name\" \""+name+"\" \"installdir\" \""+dir+"\" \"StateFlags\" \""+flags+"\" }";
        void Write(string id,string text)=>File.WriteAllText(Path.Combine(extra,"steamapps","appmanifest_"+id+".acf"),text);
        Write("123",Manifest("123","Test & game","Test game"));
        Write("124",Manifest("124","Downloading","Test game","2"));
        Write("125",Manifest("125","Traversal","..\\..\\.."));
        Write("126","\"AppState\" { \"appid\" \"126\"");
        Write("127",Manifest("128","Wrong identity","Test game"));
        Write("228980",Manifest("228980","Redistributables","Test game"));
        Check(!SteamLibrary.Discover(Path.Combine(root,"no-steam")).Installed && SteamLibrary.Discover(steam).Installed,"a PC without Steam is told apart from an empty library");
        var discovery=SteamLibrary.Discover(steam);
        Check(discovery.Games.Count==1 && discovery.Games[0].AppId=="123" && discovery.Games[0].Title=="Test & game" && discovery.Errors.Count==1,"multiple library paths, deduplication, malformed file isolation, installed flags and path/identity validation");
        var game=discovery.Games[0];
        string exe=Path.Combine(game.Directory,"game.exe"); File.Copy(Environment.ProcessPath!,exe);
        var store=new ProfileStore(Path.Combine(root,"data"));
        var profile=new Profile {ExecutablePath=exe,GeometrySeparation=.5f,GeometryConvergence=.001f}; profile.SelectStereoBackend(StereoPaths.Vkd3d); store.Save(profile);
        SteamLibrary.ImportBindings(store,[game],[]);
        Check(SteamLibrary.ProfileKey(store.Root,game)==exe && store.Load(exe).GeometrySeparation==.5f,"existing executable profile and calibration reused by the installed card");
        bool refused=false; try {SteamLibrary.SaveBinding(store.Root,game,new(Environment.ProcessPath!));} catch(InvalidDataException) {refused=true;}
        Check(refused && !SteamLibrary.Inside(game.Directory+" sibling/game.exe",game.Directory),"binding cannot escape the game's installation folder or match a prefix sibling");
        Check(!SteamLibrary.ValidId("123/quit") && !SteamLibrary.ValidId("0") && !SteamLibrary.ValidId("4294967296"),"Steam identifiers validated before URI/path construction");
        var unknown=new SteamLibrary.Game("456","Not configured",game.Directory);
        Check(SteamLibrary.ProfileKey(store.Root,unknown)!=exe && SteamLibrary.IsPlaceholder(store.Root,SteamLibrary.ProfileKey(store.Root,unknown)),"unconfigured titles use a placeholder key");
        // An earlier build saved placeholder profiles that then counted as set-up games.
        store.Save(new Profile{ExecutablePath=SteamLibrary.PlaceholderKey(store.Root,"456")});
        Check(SteamLibrary.RemovePlaceholderProfiles(store)==1 && !store.HasProfile(SteamLibrary.PlaceholderKey(store.Root,"456")) && store.HasProfile(exe),"placeholder profiles are removed, real ones kept");
        // Launchers are never bound or attached as the game.
        string launcherExe=Path.Combine(game.Directory,"GameLauncher.exe"); File.Copy(Environment.ProcessPath!,launcherExe);
        bool launcherRefused=false; try {SteamLibrary.SaveBinding(store.Root,game,new(launcherExe));} catch(InvalidDataException) {launcherRefused=true;}
        Check(launcherRefused && SteamLibrary.IsLauncher("UnityCrashHandler64.exe") && SteamLibrary.IsLauncher("unins000.exe") && !SteamLibrary.IsLauncher(exe),"launchers, crash reporters and uninstallers are never recorded as the game");
        var launcherApp=new RunningApp(7,"GameLauncher.exe",launcherExe,[new GameWindow(1,"Launcher")],null);
        var gameApp=new RunningApp(8,"game.exe",exe,[new GameWindow(2,"Game")],null);
        Check(SteamLibrary.Running(game,[launcherApp])==null && SteamLibrary.Running(game,[launcherApp,gameApp])==gameApp && SteamLibrary.Running(game,[launcherApp,gameApp],launcherExe)==launcherApp,
            "a Steam launch attaches to the bound executable, or the game rather than its launcher");
        string[] merged=SteamGeometry.MergeArguments(["--bundle-dir","data","literal space","-dx11"],new Profile {ExecutablePath="game-Shipping.exe",StereoBackend=StereoPaths.Vkd3d,GeometryLaunchArguments="-dx12 -norhithread"});
        Check(merged.Contains("literal space") && merged.Take(2).SequenceEqual(new[]{"--bundle-dir","data"}) && !merged.Contains("-dx11") && merged.Count(a=>a=="-dx12")==1,"Steam arguments retained literally while the selected Unreal renderer is applied");
        var chooser=new ChooserWindow([],[],null,0,null,[],[game],store.Root,geometryMode:true) {
            Owner=owner,ShowActivated=false,WindowStartupLocation=WindowStartupLocation.Manual,Left=-20000,Top=-20000
        };
        static IEnumerable<Button> Buttons(DependencyObject node) {
            if(node is Button button) yield return button;
            for(int i=0;i<VisualTreeHelper.GetChildrenCount(node);i++) foreach(var child in Buttons(VisualTreeHelper.GetChild(node,i))) yield return child;
        }
        _=owner.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle,new Action(()=>{
            chooser.UpdateLayout();
            var image=new RenderTargetBitmap((int)chooser.ActualWidth,(int)chooser.ActualHeight,96,96,PixelFormats.Pbgra32); image.Render(chooser);
            var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(image));
            using(var file=File.Create(Path.Combine(output,"ui-chooser-steam.png"))) encoder.Save(file);
            // No per-tile routes or Steam setup review: the tile launches as the Depth | Geometry
            // switch says (here Geometry, so it says so), and its ⚙ shows the game's settings.
            Check(!Buttons(chooser).Any(b=>b.Tag is string tag && StereoPaths.Valid(tag)) && !Buttons(chooser).Any(b=>Equals(b.Content,Loc.Get("SteamReviewSetup"))),
                "Steam tiles have no stereo method buttons or Steam setup review");
            var tile=Buttons(chooser).First(b=>b.Style==chooser.FindResource("Tile"));
            Check(System.Windows.Automation.AutomationProperties.GetName(tile).StartsWith(Loc.Get("ChooserLaunchGeometryAction"),StringComparison.Ordinal),"in Geometry mode a Steam game reads Launch with true geometry");
            tile.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }));
        Check(chooser.ShowDialog()==true && chooser.SteamSectionShown && chooser.SteamTileCount==1 && chooser.Picked?.Steam==game && !chooser.Picked.ReviewSteamSetup && !chooser.Picked.DepthOnly &&
            chooser.Picked.App.FullPath==exe,"installed card launches without a running window, as the mode says");
        var runningApp=new RunningApp(123,"game.exe",exe,[new GameWindow(42,"Test gameplay")],null);
        var recent=new GameHistory.Entry(exe,game.Title,[],game.Directory,game.AppId,DateTimeOffset.UtcNow);
        var attachChooser=new ChooserWindow([runningApp],[],null,0,null,[recent],[game],store.Root) {
            Owner=owner,ShowActivated=false,WindowStartupLocation=WindowStartupLocation.Manual,Left=-20000,Top=-20000
        };
        _=owner.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle,new Action(()=>{
            attachChooser.UpdateLayout();
            var image=new RenderTargetBitmap((int)attachChooser.ActualWidth,(int)attachChooser.ActualHeight,96,96,PixelFormats.Pbgra32);image.Render(attachChooser);
            var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(image));
            using(var file=File.Create(Path.Combine(output,"ui-chooser-attach.png"))) encoder.Save(file);
            // A running Steam game only attaches: no stereo method buttons, no Steam restart route.
            Check(!Buttons(attachChooser).Any(b=>b.Tag is string tag && StereoPaths.Valid(tag)),"running windows have no stereo method buttons");
            var tile=Buttons(attachChooser).First(b=>b.Style==attachChooser.FindResource("Tile"));
            Check(System.Windows.Automation.AutomationProperties.GetName(tile).StartsWith(Loc.Get("ChooserAttachAction"),StringComparison.Ordinal),"running choices explicitly describe attaching");
            tile.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }));
        Check(attachChooser.ShowDialog()==true && attachChooser.HistoryTileCount==0 && attachChooser.SteamTileCount==0 && attachChooser.WindowTileCount==1 && attachChooser.Picked?.App.Pid==123 &&
            attachChooser.Picked.Steam==null && !attachChooser.Picked.Restart && !attachChooser.Picked.ReviewSteamSetup,"running games appear only under Attach and only attach (the saved method decides; never a Steam restart)");
#if DEBUG
        // Exercise the actual Steam-wrapper entry point and literal argument
        // forwarding, with a harmless child fixture rather than a real game.
        string childOutput=Path.Combine(root,"child.json");
        const string fixtureId="4294967294";
        Check(!File.Exists(Path.Combine(SteamGeometry.DataRoot,"steam",fixtureId+".pending.json")),"fixture ID must not collide with a real pending launch");
        var start=new ProcessStartInfo(Environment.ProcessPath!) {UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=root};
        foreach(string arg in new[]{"--steam-game",fixtureId,"--",Environment.ProcessPath!,"--steam-child-test="+childOutput,"literal space","quote\"value"}) start.ArgumentList.Add(arg);
        start.Environment["VRX_STEAM_TEST_MARKER"]="inherited";
        using var wrapper=Process.Start(start)!;
        await wrapper.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(20));
        Check(wrapper.ExitCode==0 && File.Exists(childOutput),"ordinary Steam wrapper launch starts and waits for its child");
        using var child=JsonDocument.Parse(File.ReadAllText(childOutput));
        Check(child.RootElement.GetProperty("Arguments").EnumerateArray().Select(a=>a.GetString()).Skip(1).SequenceEqual(new[]{"literal space","quote\"value"}) && child.RootElement.GetProperty("Marker").GetString()=="inherited" && child.RootElement.GetProperty("Directory").GetString()==root,"wrapper preserves Steam arguments, working directory, and environment");
        // A geometry request's profile goes through JSON without its per-launch method (the mode
        // is app-wide and never saved), so the wrapper must set true geometry itself.
        var asked=new Profile{ExecutablePath=exe}; asked.ChooseGeometryRenderer(StereoPaths.Vkd3d); asked.UseGeometry(true);
        var arrived=JsonSerializer.Deserialize<SteamGeometry.Request>(JsonSerializer.Serialize(new SteamGeometry.Request("nonce",exe,asked,DateTimeOffset.UtcNow.AddMinutes(3))))!.Profile;
        Check(!arrived.GeometryStereo,"the per-launch method is not saved with a request's profile");
        SteamGeometry.RequestedGeometry(arrived);
        Check(arrived.GeometryStereo && arrived.EffectiveStereoBackend==StereoPaths.Vkd3d,"a Steam geometry request is true geometry in the wrapper, with the game's renderer");
        // A cancelled or expired request: the next ordinary Steam launch starts the game normally.
        string expiredRoot=Path.Combine(root,"expired-data"), expiredChild=Path.Combine(root,"expired-child.json");
        var expired=new SteamGeometry.Request("nonce",exe,new Profile{ExecutablePath=exe},DateTimeOffset.MinValue);
        ProfileStore.AtomicWrite(Path.Combine(expiredRoot,"steam","123.pending.json"),JsonSerializer.Serialize(expired));
        int expiredExit=await SteamGeometry.RunFixture("123",[Environment.ProcessPath!,"--steam-child-test="+expiredChild],expiredRoot,[game]);
        Check(expiredExit==0 && File.Exists(expiredChild) && !File.Exists(Path.Combine(expiredRoot,"steam","123.pending.json")),"an expired VRX request falls through to a normal launch instead of an error");
#endif
        File.WriteAllText(Path.Combine(output,"steam-library-pass.txt"),"PASS: installed library discovery and partial failures; path containment; existing profile reuse; installed chooser selection; literal Steam argument merge; actual wrapper child launch and lifecycle.");
        var local=SteamLibrary.Discover();
        File.WriteAllText(Path.Combine(output,"steam-discovered.json"),JsonSerializer.Serialize(local,new JsonSerializerOptions{WriteIndented=true}));
    }
}
