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
        // A game fixture (never run): VRX itself is not a game, so it is never recorded.
        string gameExe=Path.Combine(root,"fixture-game","FixtureGame.exe");
        GeometryLaunchTests.FakePe(gameExe,["d3d11.dll"],[]);
        var first=new GameHistory.Entry(gameExe,"Previously played game",["literal space","--quote=\"value\""],AppContext.BaseDirectory,"",DateTimeOffset.UtcNow);
        history.Remember(first); history.Remember(first with {Executable=first.Executable.ToUpperInvariant(),Title="Updated game",LastPlayedUtc=DateTimeOffset.UtcNow.AddSeconds(1)});
        Check(history.Load().Count==1 && history.Load()[0].Title=="Updated game" && history.Load()[0].Arguments.SequenceEqual(first.Arguments),"one panel per executable, updated title and literal launch arguments survive reload");
        for(int i=0;i<26;++i) history.Remember(first with {Executable=Path.Combine(root,$"game-{i}.exe"),LastPlayedUtc=DateTimeOffset.UtcNow.AddSeconds(i+2)});
        Check(history.Load().Count==24 && history.Load()[0].Executable.EndsWith("game-25.exe"),"history is bounded and ordered most recently used first");
        bool refused=false; try{history.Remember(first with {SteamAppId="482400/invalid"});}catch(InvalidDataException){refused=true;}
        Check(refused,"Steam launch identifiers cannot contain URI commands");
        var captured=new GameHistory(Path.Combine(root,"captured"));
        using var self=Process.GetCurrentProcess();
        // This process's command line stands in for the game's (Record reads it by process id).
        var app=new RunningApp(self.Id,"FixtureGame.exe",gameExe,[new GameWindow(42,"Recorded game")],null);
        await captured.Record(app,app.Windows[0],new Profile{ExecutablePath=app.FullPath},CancellationToken.None);
        Check(captured.Load().Single().Arguments.Contains("--smoke-test"),"a successful attachment records the real launch command for a later geometry launch");

        // Not games: desktop apps, browsers, terminals, Windows' shell and VRX are never recorded,
        // whatever window was attached (a whole display is not recorded either); Steam games are.
        foreach(string name in new[]{"ms-teams.exe","Teams.exe","olk.exe","OUTLOOK.EXE","devenv.exe","Code.exe","explorer.exe","notepad.exe","ApplicationFrameHost.exe",
            "claude.exe","ChatGPT.exe","slack.exe","Discord.exe","WhatsApp.exe","WhatsApp.Root.exe","Telegram.exe","WINWORD.EXE","EXCEL.EXE","POWERPNT.EXE","Acrobat.exe","AcroRd32.exe",
            "mspaint.exe","Photos.exe","chrome.exe","msedge.exe","firefox.exe","WindowsTerminal.exe","cmd.exe","steam.exe","vrmonitor.exe","VRX.Desktop.exe"})
            Check(GameHistory.IsNotAGame(Path.Combine(@"C:\Program Files\Apps",name)),$"{name} is not a game");
        foreach(string name in new[]{"FixtureGame.exe","SystemReShock-Win64-Shipping.exe","Cyberpunk2077.exe","eldenring.exe","Factorio.exe"})
            Check(!GameHistory.IsNotAGame(Path.Combine(@"D:\Games",name)),$"{name} is a game");
        string notGamesRoot=Path.Combine(root,"not-games"); var notGames=new GameHistory(notGamesRoot);
        string teams=Path.Combine(root,"apps","ms-teams.exe");
        var teamsApp=new RunningApp(self.Id,"ms-teams.exe",teams,[new GameWindow(43,"Fraegra - TRS Data | Per-Scent LTD | Microsoft Teams")],null);
        await notGames.Record(teamsApp,teamsApp.Windows[0],new Profile{ExecutablePath=teams},CancellationToken.None);
        notGames.Remember(first with {Executable=Path.Combine(root,"apps","Code.exe"),Title="Visual Studio Code"});
        Check(notGames.Load().Count==0 && !File.Exists(Path.Combine(notGamesRoot,"recent-games.json")),"Teams and other desktop apps are never recorded as recent games");
        var steamNamed=first with {Executable=Path.Combine(root,"steamapps","common","Odd","explorer.exe"),Title="A Steam game named explorer",SteamAppId="480"};
        notGames.Remember(steamNamed);
        Check(notGames.Load().SingleOrDefault()?.Executable==steamNamed.Executable,"a Steam game is recorded whatever its executable is called");
        // Recent games saved before this filter: non-games are dropped when the list loads; their
        // profiles stay.
        string oldRoot=Path.Combine(root,"old-history"); var oldHistory=new GameHistory(oldRoot);
        var oldStore=new ProfileStore(oldRoot);
        var teamsProfile=new Profile{ExecutablePath=teams}; oldStore.Save(teamsProfile);
        GameHistory.Entry[] saved=[first with {Executable=teams,Title="Fraegra - TRS Data | Per-Scent LTD | Microsoft Teams",LastPlayedUtc=DateTimeOffset.UtcNow.AddMinutes(1)},first];
        ProfileStore.AtomicWrite(Path.Combine(oldRoot,"recent-games.json"),System.Text.Json.JsonSerializer.Serialize(saved));
        Check(oldHistory.Load().Select(e=>e.Executable).SequenceEqual(new[]{first.Executable}),"an existing Teams entry is dropped from recent games when they load");
        Check(oldStore.HasProfile(teams),"dropping a non-game from recent games keeps its profile");
        oldHistory.Remember(first with {LastPlayedUtc=DateTimeOffset.UtcNow.AddMinutes(2)});
        Check(!File.ReadAllText(Path.Combine(oldRoot,"recent-games.json")).Contains("ms-teams",StringComparison.OrdinalIgnoreCase),"the next save leaves the non-game out of recent-games.json");
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
        File.WriteAllText(Path.Combine(output,"game-history-pass.txt"),"PASS: persistent bounded history; case-insensitive deduplication; actual process launch capture; literal argument preservation; missing-game and invalid Steam handling; non-games (desktop apps, browsers, shell, VRX) never recorded and dropped on load, profiles kept; Steam games recorded.");
    }
}
