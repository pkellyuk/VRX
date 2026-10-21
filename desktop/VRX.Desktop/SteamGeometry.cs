using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;

namespace VRX.Desktop;

// Steam starts this wrapper through its per-game Launch Options. Environment
// changes stay in the game child, never in Steam or the user's environment.
internal static class SteamGeometry
{
    internal sealed record Request(string Nonce,string Executable,Profile Profile,DateTimeOffset Expires,bool Claimed=false);
    internal sealed record Result(string Nonce,GeometryLaunch.Run? Run,string? Error);
    internal static string DataRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"VRX");
    private static string RequestFile(string root,string id) => SteamLibrary.ValidId(id)?Path.Combine(root,"steam",id+".pending.json"):throw new InvalidDataException(Loc.Get("SteamInvalidId"));
    public static string LaunchOptions(string id) {
        if(!SteamLibrary.ValidId(id)) throw new InvalidDataException(Loc.Get("SteamInvalidId"));
        // The fixed launcher (SteamLauncher): Launch Options that survive moving or reinstalling VRX.
        SteamLauncher.Refresh();
        if(SteamLauncher.Available && SteamLauncher.Target()!=null) return GeometryLaunch.QuoteArgument(SteamLauncher.InstalledPath)+" --steam-game "+id+" -- %command%";
        Debug.WriteLine("[Steam setup] LaunchOptions: no fixed launcher; naming this VRX copy directly");
        string executable=Environment.ProcessPath ?? throw new InvalidOperationException(Loc.Get("ErrorVrxPathUnavailable"));
        if(!Path.GetFileNameWithoutExtension(executable).Equals("VRX.Desktop",StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException(Loc.Get("SteamUsePublishedApp"));
        return GeometryLaunch.QuoteArgument(executable)+" --steam-game "+id+" -- %command%";
    }
    // Steam games whose launch options start them through VRX: these must be removed in Steam
    // before VRX is moved or uninstalled (Steam would otherwise fail to start them). VRX does
    // not edit Steam's settings, so this lists exactly what to remove.
    public static void ShowRemoval(Window owner,string root,IReadOnlyList<SteamLibrary.Game> games) {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(root);
        ArgumentNullException.ThrowIfNull(games);
        Debug.WriteLine($"[Steam setup] ShowRemoval enter: {games.Count} game(s)");
        var window=new Window {Title=Loc.Get("SteamRemoveTitle"),Owner=owner,Width=650,SizeToContent=SizeToContent.Height,MaxHeight=Math.Min(800,SystemParameters.WorkArea.Height-40),
            WindowStartupLocation=WindowStartupLocation.CenterOwner,ShowInTaskbar=false};
        window.Background=(System.Windows.Media.Brush)owner.FindResource("Page"); window.Foreground=(System.Windows.Media.Brush)owner.FindResource("Ink");
        var body=new StackPanel {Margin=new Thickness(24)};
        window.Content=new ScrollViewer {Content=body,VerticalScrollBarVisibility=ScrollBarVisibility.Auto};
        body.Children.Add(new TextBlock {Text=Loc.Get(games.Count==0?"SteamRemoveNone":"SteamRemoveInfo"),TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,12)});
        foreach(var game in games) {
            body.Children.Add(new TextBlock {Text=game.Title,FontWeight=FontWeights.SemiBold,Margin=new Thickness(0,8,0,4)});
            body.Children.Add(new TextBox {Text=LaunchOptions(game.AppId),IsReadOnly=true,TextWrapping=TextWrapping.Wrap});
        }
        var done=new Button {Content=Loc.Get(games.Count==0?"SteamRemoveClose":"SteamRemoveDone"),Margin=new Thickness(0,16,0,0),HorizontalAlignment=HorizontalAlignment.Right,Padding=new Thickness(14,8,14,8)};
        done.Click+=(_,_)=>window.DialogResult=true; body.Children.Add(done);
        if(window.ShowDialog()!=true) { Debug.WriteLine("[Steam setup] ShowRemoval exit: closed"); return; }
        // "I've removed them": VRX shows the setup again before the next Steam geometry launch.
        foreach(var game in games) {
            if(SteamLibrary.LoadBinding(root,game) is not {} binding) continue;
            try { SteamLibrary.SaveBinding(root,game,binding with {WrapperConfigured=false}); }
            catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or InvalidDataException) { Debug.WriteLine("[Steam setup] "+ex.Message); }
        }
        Debug.WriteLine("[Steam setup] ShowRemoval exit: marked removed");
    }
    // True when Steam still starts this game through a VRX install folder named directly
    // (an older setup, or a development build): those must be replaced with the fixed launcher.
    public static bool NeedsUpdate(SteamLibrary.Game game) {
        ArgumentNullException.ThrowIfNull(game);
        string? current=SteamLibrary.LaunchOptionsFor(game.AppId);
        bool stale=SteamLauncher.IsDirectOption(current);
        Debug.WriteLine($"[Steam setup] NeedsUpdate {game.AppId}: {stale}");
        return stale;
    }
    public static bool Setup(Window owner,SteamLibrary.Game game) {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(game);
        string options=LaunchOptions(game.AppId);
        bool replacing=NeedsUpdate(game);
        Debug.WriteLine($"[Steam setup] Setup {game.AppId}: replacing {replacing}");
        var window=new Window {Title=Loc.Get("SteamSetupTitle"),Owner=owner,Width=650,SizeToContent=SizeToContent.Height,WindowStartupLocation=WindowStartupLocation.CenterOwner,ShowInTaskbar=false};
        window.Background=(System.Windows.Media.Brush)owner.FindResource("Page"); window.Foreground=(System.Windows.Media.Brush)owner.FindResource("Ink");
        var body=new StackPanel {Margin=new Thickness(24)}; window.Content=body;
        body.Children.Add(new TextBlock {Text=Loc.Format(replacing?"SteamSetupReplace":"SteamSetupInfo",game.Title),TextWrapping=TextWrapping.Wrap});
        body.Children.Add(new TextBox {Text=options,IsReadOnly=true,TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,12,0,12)});
        var copy=new Button {Content=Loc.Get("SteamCopyOptions"),Margin=new Thickness(0,0,0,12)};
        copy.Click+=(_,_)=>Clipboard.SetText(options); body.Children.Add(copy);
        var launch=new Button {Content=Loc.Get("SteamSetupLaunch")}; launch.Click+=(_,_)=>window.DialogResult=true; body.Children.Add(launch);
        return window.ShowDialog()==true;
    }
    public static async Task<GeometryLaunch.Run> Launch(SteamLibrary.Game game,GeometryLaunch.Plan plan,Profile profile,string root,Action<string> status,CancellationToken token)
    {
        string pending=RequestFile(root,game.AppId), resultFile=pending+".result";
        var request=new Request(Guid.NewGuid().ToString("N"),plan.App.FullPath,profile,DateTimeOffset.UtcNow.AddMinutes(3));
        // The launcher wrapper's paths are fixed to the normal per-user VRX root.
        if(!Path.GetFullPath(root).Equals(Path.GetFullPath(DataRoot),StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException(Loc.Get("SteamDataDirectory"));
        using var gate=new Mutex(false,@"Local\VRX.steam.launch."+game.AppId);
        bool held; try {held=gate.WaitOne(0);} catch(AbandonedMutexException) {held=true;}
        if(!held) throw new InvalidOperationException(Loc.Get("SteamLaunchBusy"));
        // A Mutex is thread-affine: release before the first await. The pending
        // request also guards launches after Steam has started the wrapper.
        try {
            if(File.Exists(pending)) {
                var previous=JsonSerializer.Deserialize<Request>(File.ReadAllText(pending));
                if(previous?.Expires>DateTimeOffset.UtcNow) throw new InvalidOperationException(Loc.Get("SteamLaunchBusy"));
            }
            ProfileStore.AtomicWrite(pending,JsonSerializer.Serialize(request));
        } finally {gate.ReleaseMutex();}
        bool completed=false;
        try {
            SteamLibrary.Launch(game.AppId); status(Loc.Get("SteamWaitingWrapper"));
            long until=Environment.TickCount64+120000;
            while(Environment.TickCount64<until) {
                token.ThrowIfCancellationRequested();
                if(File.Exists(resultFile)) {
                    var result=JsonSerializer.Deserialize<Result>(File.ReadAllText(resultFile));
                    if(result?.Nonce==request.Nonce) {
                        completed=true;
                        if(result.Run!=null) return result.Run;
                        throw new InvalidOperationException(result.Error ?? Loc.Get("ErrorGeometryLaunch"));
                    }
                }
                await Task.Delay(250,token);
            }
            throw new InvalidOperationException(Loc.Get("SteamWrapperTimeout"));
        } finally {
            // Expire instead of deleting, so a wrapper already started for this request knows
            // it was cancelled; a later Steam start then launches the game normally.
            if(completed) File.Delete(pending);
            else ProfileStore.AtomicWrite(pending,JsonSerializer.Serialize(request with {Expires=DateTimeOffset.MinValue}));
        }
    }
    internal static string[] MergeArguments(string[] steamArguments,Profile profile) => GeometryLaunch.ApiArguments(
        steamArguments.Concat(GeometryLaunch.ParseArguments(profile.GeometryLaunchArguments ?? "").Where(a=>!steamArguments.Contains(a,StringComparer.Ordinal))),
        profile.EffectiveStereoBackend,Path.GetFileName(profile.ExecutablePath).EndsWith("-Shipping.exe",StringComparison.OrdinalIgnoreCase));
    public static Task<int> Run(string id,string[] command) => RunCore(id,command,DataRoot,null,true);
    internal static Task<int> RunFixture(string id,string[] command,string dataRoot,IReadOnlyList<SteamLibrary.Game> games) => RunCore(id,command,dataRoot,games,false);
    private static async Task<int> RunCore(string id,string[] command,string dataRoot,IReadOnlyList<SteamLibrary.Game>? games,bool showErrors)
    {
        string pending=""; Request? request=null;
        try {
            if(!SteamLibrary.ValidId(id) || command.Length==0 || !Path.IsPathFullyQualified(command[0]) || !File.Exists(command[0])) throw new InvalidDataException(Loc.Get("SteamCommandInvalid"));
            pending=RequestFile(dataRoot,id);
            using var claim=new Mutex(false,@"Local\VRX.steam.wrapper."+id);
            bool held; try {held=claim.WaitOne(0);} catch(AbandonedMutexException) {held=true;}
            if(!held) throw new InvalidOperationException(Loc.Get("SteamLaunchBusy"));
            try {
                if(File.Exists(pending)) {
                    request=JsonSerializer.Deserialize<Request>(File.ReadAllText(pending)) ?? throw new InvalidDataException(Loc.Get("SteamCommandInvalid"));
                    if(request.Expires<=DateTimeOffset.UtcNow) {
                        // A cancelled or expired VRX request: this is an ordinary Steam launch,
                        // so the game starts normally (no geometry) instead of showing an error.
                        Debug.WriteLine($"[Steam wrapper] request for {id} expired at {request.Expires:O}; launching normally");
                        File.Delete(pending); request=null;
                    } else {
                        if(request.Claimed) {request=null; throw new InvalidOperationException(Loc.Get("SteamLaunchBusy"));}
                        // Consume atomically under the wrapper gate; only one child
                        // may install the translation libraries for this request.
                        ProfileStore.AtomicWrite(pending,JsonSerializer.Serialize(request with {Claimed=true}));
                    }
                }
            } finally {claim.ReleaseMutex();}
            if(request==null) {
                // Keeping the wrapper in Steam must preserve ordinary launches.
                GeometryLaunch.Recover(dataRoot);
                var start=new ProcessStartInfo(command[0]) {UseShellExecute=false,WorkingDirectory=Environment.CurrentDirectory};
                foreach(string arg in command.Skip(1)) start.ArgumentList.Add(arg);
                using var plain=Process.Start(start) ?? throw new InvalidOperationException(Loc.Get("ErrorGeometryLaunch"));
                await plain.WaitForExitAsync(); return plain.ExitCode;
            }
            var installed=(games ?? SteamLibrary.Discover().Games).FirstOrDefault(g=>g.AppId==id);
            if(installed==null || !SteamLibrary.Inside(command[0],installed.Directory) || !SteamLibrary.Inside(request.Executable,installed.Directory)) throw new InvalidOperationException(Loc.Get("SteamCommandMismatch"));
            var profile=request.Profile;
            if(!profile.Valid() || !profile.ExecutablePath.Equals(request.Executable,StringComparison.OrdinalIgnoreCase) || profile.GeometrySteamAppId!=id) throw new InvalidDataException(Loc.Get("SteamCommandInvalid"));
            profile.GeometryLaunchArguments=string.Join(' ',MergeArguments(command.Skip(1).ToArray(),profile).Select(GeometryLaunch.QuoteArgument));
            if(string.IsNullOrEmpty(profile.GeometryWorkingDirectory)) profile.GeometryWorkingDirectory=Environment.CurrentDirectory;
            var app=new RunningApp(0,Path.GetFileName(request.Executable),request.Executable,[],null);
            var plan=await GeometryLaunch.Prepare(app,profile,dataRoot,_=>{},CancellationToken.None,[]);
            var run=await GeometryLaunch.LaunchFromSteam(plan,profile,dataRoot,_=>{},CancellationToken.None,command[0]);
            ProfileStore.AtomicWrite(pending+".result",JsonSerializer.Serialize(new Result(request.Nonce,run,null)));
            try {using var game=Process.GetProcessById(run.Pid); await game.WaitForExitAsync();} catch(ArgumentException) {}
            GeometryLaunch.Recover(dataRoot); return 0;
        } catch(Exception ex) {
            if(request!=null) {
                ProfileStore.AtomicWrite(pending+".result",JsonSerializer.Serialize(new Result(request.Nonce,null,ex.Message)));
                // A stream failure does not authorise closing a playable game.
                // Keep this wrapper alive to recover its DLLs when the game exits.
                foreach(var game in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(request.Executable))) {
                    using(game) {
                        if(RunningApps.ProcessPath(game.Id).Equals(request.Executable,StringComparison.OrdinalIgnoreCase)) await game.WaitForExitAsync();
                    }
                }
                GeometryLaunch.Recover(dataRoot);
            } else if(showErrors) MessageBox.Show(ex.Message,Loc.Get("SteamSetupTitle"),MessageBoxButton.OK,MessageBoxImage.Error);
            return 1;
        }
    }
}
