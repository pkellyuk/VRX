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
        Check(SteamLibrary.ProfileKey(store.Root,unknown)!=exe,"unconfigured titles have separate provisional profiles");
        string[] merged=SteamGeometry.MergeArguments(["--bundle-dir","data","literal space","-dx11"],new Profile {ExecutablePath="game-Shipping.exe",StereoBackend=StereoPaths.Vkd3d,GeometryLaunchArguments="-dx12 -norhithread"});
        Check(merged.Contains("literal space") && merged.Take(2).SequenceEqual(new[]{"--bundle-dir","data"}) && !merged.Contains("-dx11") && merged.Count(a=>a=="-dx12")==1,"Steam arguments retained literally while the selected Unreal renderer is applied");
        var chooser=new ChooserWindow([],[],null,0,null,_=>StereoPaths.Vkd3d,_=>StereoPaths.Vkd3d,[],[game],store.Root) {
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
            Buttons(chooser).First(b=>b.Tag is string tag && tag==StereoPaths.Vkd3d).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }));
        Check(chooser.ShowDialog()==true && chooser.SteamTileCount==1 && chooser.Picked?.Steam==game && chooser.Picked.Backend==StereoPaths.Vkd3d && chooser.Picked.App.FullPath==exe,"installed card launches without a running window and returns the saved stereo route");
        var runningApp=new RunningApp(123,"game.exe",exe,[new GameWindow(42,"Test gameplay")],null);
        var recent=new GameHistory.Entry(exe,game.Title,[],game.Directory,game.AppId,DateTimeOffset.UtcNow);
        var attachChooser=new ChooserWindow([runningApp],[],null,0,null,_=>StereoPaths.Depth,_=>StereoPaths.Vkd3d,[recent],[game],store.Root) {
            Owner=owner,ShowActivated=false,WindowStartupLocation=WindowStartupLocation.Manual,Left=-20000,Top=-20000
        };
        _=owner.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle,new Action(()=>{
            attachChooser.UpdateLayout();
            var image=new RenderTargetBitmap((int)attachChooser.ActualWidth,(int)attachChooser.ActualHeight,96,96,PixelFormats.Pbgra32);image.Render(attachChooser);
            var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(image));
            using(var file=File.Create(Path.Combine(output,"ui-chooser-attach.png"))) encoder.Save(file);
            var depth=Buttons(attachChooser).First(b=>b.Tag is string tag && tag==StereoPaths.Depth);
            Check(System.Windows.Automation.AutomationProperties.GetName(depth).StartsWith(Loc.Get("ChooserAttachAction"),StringComparison.Ordinal),"running choices explicitly describe attaching");
            depth.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }));
        Check(attachChooser.ShowDialog()==true && attachChooser.HistoryTileCount==0 && attachChooser.SteamTileCount==0 && attachChooser.WindowTileCount==1 && attachChooser.Picked?.App.Pid==123 && attachChooser.Picked.Steam==game,"running games appear only under Attach and retain their Steam/profile identity");
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
#endif
        File.WriteAllText(Path.Combine(output,"steam-library-pass.txt"),"PASS: installed library discovery and partial failures; path containment; existing profile reuse; installed chooser selection; literal Steam argument merge; actual wrapper child launch and lifecycle.");
        var local=SteamLibrary.Discover();
        File.WriteAllText(Path.Combine(output,"steam-discovered.json"),JsonSerializer.Serialize(local,new JsonSerializerOptions{WriteIndented=true}));
    }
}
