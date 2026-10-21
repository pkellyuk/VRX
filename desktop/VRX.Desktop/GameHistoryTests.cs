using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace VRX.Desktop;

internal static class GameHistoryTests
{
    public static async Task Run(string output,Window owner)
    {
        static void Check(bool ok,string reason) { if(!ok) throw new Exception("Recent games: "+reason); }
        string root=Path.Combine(output,"history-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        var history=new GameHistory(root);
        var first=new GameHistory.Entry(Environment.ProcessPath!,"Previously played game",["literal space","--quote=\"value\""],AppContext.BaseDirectory,"",DateTimeOffset.UtcNow);
        history.Remember(first); history.Remember(first with {Executable=first.Executable.ToUpperInvariant(),Title="Updated game",LastPlayedUtc=DateTimeOffset.UtcNow.AddSeconds(1)});
        Check(history.Load().Count==1 && history.Load()[0].Title=="Updated game" && history.Load()[0].Arguments.SequenceEqual(first.Arguments),"one panel per executable, updated title and literal launch arguments survive reload");
        for(int i=0;i<26;++i) history.Remember(first with {Executable=Path.Combine(root,$"game-{i}.exe"),LastPlayedUtc=DateTimeOffset.UtcNow.AddSeconds(i+2)});
        Check(history.Load().Count==24 && history.Load()[0].Executable.EndsWith("game-25.exe"),"history is bounded and ordered most recently used first");
        bool refused=false; try{history.Remember(first with {SteamAppId="482400/invalid"});}catch(InvalidDataException){refused=true;}
        Check(refused,"Steam launch identifiers cannot contain URI commands");
        var captured=new GameHistory(Path.Combine(root,"captured"));
        using var self=Process.GetCurrentProcess();
        var app=new RunningApp(self.Id,"fixture",Environment.ProcessPath!,[new GameWindow(42,"Recorded game")],null);
        await captured.Record(app,app.Windows[0],new Profile{ExecutablePath=app.FullPath},CancellationToken.None);
        Check(captured.Load().Single().Arguments.Contains("--smoke-test"),"a successful attachment records the real launch command for a later geometry launch");
        var raw=ProcessCommandLine.Read(self.Id);
        Check(raw!=null && raw.Contains("--smoke-test") && ProcessCommandLine.Parent(self.Id)>0,"the command line and parent are read natively, without PowerShell");
        string[] launcher=["-AUTH_LOGIN=unused","-AUTH_PASSWORD=abc123","-AUTH_TYPE=exchangecode","-epicapp=Game","-epicusername=\"Someone\"","-epicuserid=42","-epicsandboxid=x","-windowed",
            "-token","secret-value","--session=abc","-ResX=2560","/AccessToken:xyz","-EAAuthCode=1","-uplay_steam_mode","-epiclocale=en"];
        var kept=ProcessCommandLine.WithoutCredentials(launcher);
        Check(kept.SequenceEqual(new[]{"-epicapp=Game","-windowed","-ResX=2560","-epiclocale=en"}),"credential-like launch arguments are never stored: "+string.Join(' ',kept));
        var profileStore=new ProfileStore(root);
        var profile=new Profile{ExecutablePath=first.Executable,PreferredWindowTitle=first.Title}; profileStore.Save(profile);
        ProfileStore.AtomicWrite(Path.Combine(root,"last-game.txt"),first.Executable);
        var imported=new GameHistory(Path.Combine(root,"import")); // a separate root must not invent history
        imported.ImportLastGame(new ProfileStore(Path.Combine(root,"import")));
        Check(imported.Load().Count==0,"a fresh install has no invented game history");
        string importRoot=Path.Combine(root,"legacy");
        var legacyStore=new ProfileStore(importRoot); legacyStore.Save(profile);
        ProfileStore.AtomicWrite(Path.Combine(importRoot,"last-game.txt"),first.Executable);
        var legacyHistory=new GameHistory(importRoot); legacyHistory.ImportLastGame(legacyStore);
        Check(legacyHistory.Load().Single().Executable==first.Executable,"existing users get their last used game without needing another launch");
        static IEnumerable<Button> Buttons(DependencyObject node) {
            if(node is Button button && button.Tag is string tag && StereoPaths.Valid(tag)) yield return button;
            for(int i=0;i<VisualTreeHelper.GetChildrenCount(node);++i)
                foreach(var child in Buttons(VisualTreeHelper.GetChild(node,i))) yield return child;
        }
        var missing=first with {Executable=Path.Combine(root,"missing.exe"),Title="Missing game"};
        var chooser=new ChooserWindow([],[],null,0,null,_=>StereoPaths.GeometryAuto,_=>StereoPaths.Vkd3d,[first,missing]) {
            Owner=owner,ShowActivated=false,WindowStartupLocation=WindowStartupLocation.Manual,Left=-20000,Top=-20000
        };
        _=owner.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle,new Action(()=>{
            chooser.UpdateLayout();
            var buttons=Buttons(chooser).ToArray();
            if(chooser.HistoryTileCount!=2 || chooser.WindowTileCount!=0 || buttons.Count(b=>b.IsEnabled)!=2 || buttons.Count(b=>!b.IsEnabled)!=2) { chooser.Close(); return; }
            var image=new RenderTargetBitmap((int)chooser.ActualWidth,(int)chooser.ActualHeight,96,96,PixelFormats.Pbgra32); image.Render(chooser);
            var encoder=new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(image));
            using(var file=File.Create(Path.Combine(output,"ui-chooser-history.png"))) encoder.Save(file);
            buttons.First(b=>b.IsEnabled && (string)b.Tag==StereoPaths.Vkd3d).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        }));
        Check(chooser.ShowDialog()==true && chooser.Picked?.History==first && chooser.Picked.App.Pid==0 && chooser.Picked.Backend==StereoPaths.Vkd3d,"a stopped game's history panel returns its direct launch and selected renderer without needing a live window");
        File.WriteAllText(Path.Combine(output,"game-history-pass.txt"),"PASS: persistent bounded history; case-insensitive deduplication; actual process launch capture; literal argument preservation; missing-game and invalid Steam handling; history panels select a cold geometry launch without a running window.");
    }
}
