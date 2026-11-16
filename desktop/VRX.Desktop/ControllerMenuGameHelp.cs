using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace VRX.Desktop;

public sealed partial class ControllerMenu
{
    private GameHelpProvider? gameHelpCaptureProvider;
    private uint gameHelpCaptureSerial;
    private long gameHelpCaptureStarted;
    private void BeginGameHelp()
    {
        var preferences = GameHelpPreferences.Current;
        if (!preferences.Allowed(preferences.Selected)) { ShowGameHelpSettings(); return; }
        if (gameHelpCaptureProvider != null) { interactionHint.Text = Loc.Get("GameHelpCapturing"); return; }
        if (!enabled || !Lock()) { interactionHint.Text = Loc.Get("GameHelpNeedsSession"); return; }
        try
        {
            if (!Enumerable.Range(0, MaxSnapshots).Any(i => snapshots[i] == null && memory.ReadInt32(SnapshotsAt + i * SnapshotBytes + 8) == 0 && memory.ReadInt32(SnapshotsAt + i * SnapshotBytes + 12) == 0))
            { interactionHint.Text = Loc.Get("SnapshotLimit"); return; }
            gameHelpCaptureSerial = unchecked(memory.ReadUInt32(48) + 1);
            gameHelpCaptureProvider = preferences.Selected; gameHelpCaptureStarted = Environment.TickCount64;
            memory.Write(48, gameHelpCaptureSerial); interactionHint.Text = Loc.Get("GameHelpCapturing");
        }
        finally { mutex.ReleaseMutex(); }
    }
    private void CheckGameHelpCapture()
    {
        if (gameHelpCaptureProvider == null) return;
        if (!enabled || Environment.TickCount64 - gameHelpCaptureStarted > 15000)
        { gameHelpCaptureProvider = null; interactionHint.Text = Loc.Get("GameHelpCaptureFailed"); }
    }
    private void ShowGameHelpSettings()
    {
        var preferences = GameHelpPreferences.Current;
        Page(Loc.Get("GameHelpSettings"), Home, "game-help"); currentPage = ShowGameHelpSettings;
        body.Children.Add(new TextBlock { Text = Loc.Get("GameHelpSharingHint"), FontSize = 19, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 12) });
        foreach (var provider in GameHelpPreferences.Providers)
        {
            var choice = ActionButton(provider.Name, () =>
            {
                try { preferences.Select(provider); ShowGameHelpSettings(); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { interactionHint.Text = Loc.Get("GameHelpSaveFailed"); }
            });
            choice.Tag = preferences.Selected.Id == provider.Id ? "✓" : "○"; body.Children.Add(choice);
        }
        var selected = preferences.Selected;
        body.Children.Add(new TextBlock { Text = Loc.Format("GameHelpSharingWith", selected.Name), FontSize = 19, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 8) });
        if (preferences.Allowed(selected))
        {
            body.Children.Add(ActionButton(Loc.Get("GameHelpStart"), BeginGameHelp));
            body.Children.Add(ActionButton(Loc.Get("GameHelpStopSharing"), () =>
            {
                try { preferences.Permit(selected, false); ShowGameHelpSettings(); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { interactionHint.Text = Loc.Get("GameHelpSaveFailed"); }
            }));
        }
        else body.Children.Add(ActionButton(Loc.Get("GameHelpAllowAndStart"), () =>
        {
            try { preferences.Permit(selected, true); BeginGameHelp(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { interactionHint.Text = Loc.Get("GameHelpSaveFailed"); }
        }));
    }
}
