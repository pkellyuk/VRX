using System.IO;
using System.Text.Json;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

public sealed partial class ControllerMenu
{
    private readonly string? placementFile;
    private sealed record SavedPanel(int Slot, string Kind, float[]? Pose = null, string? Content = null,
        bool ShowSetup = false, bool BootUbuntu = false, double ScrollOffset = 0, double? BrowserZoom = null);

    private void RestoreWorkspace()
    {
        if (placementFile == null || !File.Exists(placementFile)) return;
        try
        {
            var file = new FileInfo(placementFile);
            if (file.Length > 128 * 1024) return;
            var panels = JsonSerializer.Deserialize<SavedPanel[]>(File.ReadAllText(placementFile)) ?? [];
            foreach (var entry in panels.OfType<SavedPanel>().DistinctBy(panel => panel.Slot))
            {
                if (entry.Slot < -1 || entry.Slot >= MaxSnapshots) continue;
                if (entry.Slot < 0)
                {
                    WritePlacement(0, entry.Pose);
                    RestoreSettingsPage(entry.Content, entry.ScrollOffset);
                    continue;
                }
                int slot = entry.Kind switch
                {
                    "browser" when WebAddress(entry.Content) is string url =>
                        AddPanel(1, () => new FloatingBrowser(FocusBrowser, OpenKeyboard, OpenBrowser, url,
                            initialZoom: entry.BrowserZoom ?? 1), entry.Slot),
                    "webcam" => AddPanel(6, CreateWebcamPanel, entry.Slot),
                    "keyboard" => AddPanel(2, () => new FloatingKeyboard(predictor, SendKeyboardEdit), entry.Slot),
                    "ubuntu" => AddPanel(3, CreateUbuntuPanel, entry.Slot),
                    "vrex-chat" => AddPanel(7, CreateVrexChatPanel, entry.Slot),
                    "image" when File.Exists(entry.Content) => AddPanel(0, () => LoadSavedSnapshot(entry.Content!), entry.Slot),
                    _ => -1
                };
                if (slot < 0) continue;
                WritePlacement(slot + 1, entry.Pose);
                if (snapshots[slot] is FloatingUbuntuPanel ubuntu)
                {
                    if (entry.ShowSetup) ubuntu.ShowSetup(true);
                    if (entry.BootUbuntu) VrexClient.Current.Send(new("start"));
                }
            }
            SetKeyboardTarget();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or NotSupportedException or FormatException)
        { System.Diagnostics.Debug.WriteLine($"[VR workspace] Restore failed: {ex.Message}"); }
    }

    private FloatingSnapshot LoadSavedSnapshot(string path)
    {
        using var stream = File.OpenRead(path);
        var image = new BitmapImage();
        image.BeginInit();
        image.CacheOption = BitmapCacheOption.OnLoad;
        image.StreamSource = stream;
        image.EndInit();
        image.Freeze();
        return new FloatingSnapshot(image, imagesFolder, path);
    }

    private void SaveWorkspace()
    {
        if (placementFile == null || !Lock()) return;
        var panels = new List<SavedPanel>();
        try
        {
            panels.Add(new(-1, "settings", ReadPlacement(0), pageKey, ScrollOffset: restoreOffset ?? scroll.VerticalOffset));
            for (int slot = 0; slot < snapshots.Length; slot++)
            {
                float[]? pose = ReadPlacement(slot + 1);
                switch (snapshots[slot])
                {
                    case FloatingBrowser { Closed: false, IsWebcam: true }:
                        panels.Add(new(slot, "webcam", pose)); break;
                    case FloatingBrowser { Closed: false, IsWebcam: false } browser:
                        panels.Add(new(slot, "browser", pose, WebAddress(browser.LoadedAddress), BrowserZoom: browser.Zoom)); break;
                    case FloatingKeyboard { Closed: false }:
                        panels.Add(new(slot, "keyboard", pose)); break;
                    case FloatingVrexChatPanel { Closed: false }:
                        panels.Add(new(slot, "vrex-chat", pose)); break;
                    case FloatingUbuntuPanel { Closed: false } ubuntu:
                        panels.Add(new(slot, "ubuntu", pose, ShowSetup: ubuntu.ShowsSetup,
                            BootUbuntu: VrexClient.Current.Ready)); break;
                    case FloatingSnapshot { Closed: false, SavedPath: string path }:
                        panels.Add(new(slot, "image", pose, path)); break;
                }
            }
        }
        finally { mutex.ReleaseMutex(); }
        try { ProfileStore.AtomicWrite(placementFile, JsonSerializer.Serialize(panels)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        { System.Diagnostics.Debug.WriteLine($"[VR workspace] Save failed: {ex.Message}"); }
    }

    private void RestoreSettingsPage(string? key, double offset)
    {
        if (key == null) return;
        if (key.StartsWith("section:", StringComparison.Ordinal) &&
            sections.FirstOrDefault(section => "section:" + section.Name == key) is { } section)
        {
            Section(section);
        }
        else if (key.StartsWith("category:", StringComparison.Ordinal) &&
            sections.FirstOrDefault(section => section.Name == "RoomSection") is { } room &&
            room.Content is System.Windows.DependencyObject content &&
            Categories(content).FirstOrDefault(category => "category:" + category.Name == key) is { } category)
        {
            Section(room);
            Category(category, () => Section(room), breadcrumb);
        }
        if (pageKey == key && double.IsFinite(offset)) restoreOffset = Math.Max(0, offset);
    }

    private void WritePlacement(int index, float[]? pose)
    {
        if (!ValidPose(pose) || !Lock()) return;
        try
        {
            long at = PlacementAt + index * PlacementBytes;
            memory.WriteArray(at + 4, pose!, 0, 9);
            memory.Write(at, 1);
        }
        finally { mutex.ReleaseMutex(); }
    }

    private float[]? ReadPlacement(int index)
    {
        long at = PlacementAt + index * PlacementBytes;
        if (memory.ReadInt32(at) is not (1 or 2)) return null;
        var pose = new float[9];
        memory.ReadArray(at + 4, pose, 0, 9);
        return ValidPose(pose) ? pose : null;
    }

    private static bool ValidPose(float[]? pose) =>
        pose is { Length: 9 } && pose.All(float.IsFinite) &&
        pose.Take(4).Sum(component => component * component) is > .98f and < 1.02f &&
        pose.Skip(4).Take(3).All(component => Math.Abs(component) <= 60) &&
        pose[7] is >= .1f and <= 10 && pose[8] is >= .1f and <= 10;

    private static string? WebAddress(string? value) =>
        Uri.TryCreate(value, UriKind.Absolute, out var uri) && uri.Scheme is "http" or "https" &&
        uri.UserInfo.Length == 0 ? uri.AbsoluteUri : null;

    internal static async Task SmokeWorkspace(string directory, IReadOnlyList<System.Windows.Controls.Expander> sections)
    {
        string file = Path.Combine(directory, $"workspace-{Guid.NewGuid():N}.json");
        string unreadable = Path.ChangeExtension(file, ".png");
        using var page = new FloatingBrowser.PermissionTestServer("<html><title>Saved floating page</title></html>");
        string url = page.Url + "saved-page?test=workspace";
        ControllerMenu Open() => new(@"Local\VRX.Controllers.Test." + Guid.NewGuid().ToString("N"),
            sections, new System.Windows.Controls.TextBlock(), new System.Windows.Controls.Button(), () => { }, placementFile: file);
        float[] pose = [0, 0, 0, 1, .4f, .2f, 1f, .8f, .64f];
        var section = sections.First(item => item.Name == "RoomSection");
        try
        {
            using (var first = Open())
            {
                if (first.AddPanel(2, () => new FloatingKeyboard(first.predictor, first.SendKeyboardEdit), 3) != 3)
                    throw new InvalidOperationException("Could not reserve a workspace panel slot.");
                if (first.AddPanel(6, CreateWebcamPanel, 4) != 4)
                    throw new InvalidOperationException("Could not reserve the webcam workspace slot.");
                if (first.AddPanel(1, () => new FloatingBrowser(_ => { }, () => { }, _ => { }, url, initialZoom: 1.4), 5) != 5)
                    throw new InvalidOperationException("Could not reserve the browser workspace slot.");
                first.Section(section);
                first.restoreOffset = 124.5;
                first.WritePlacement(0, pose);
                first.WritePlacement(4, pose);
                first.WritePlacement(5, pose);
                first.WritePlacement(6, pose);
            }
            File.WriteAllText(unreadable, "An old snapshot whose image is damaged.");
            var saved = JsonSerializer.Deserialize<SavedPanel[]>(File.ReadAllText(file))!;
            File.WriteAllText(file, JsonSerializer.Serialize(new[] { new SavedPanel(0, "image", pose, unreadable) }.Concat(saved)));
            using var restored = Open();
            if (restored.snapshots[3] is not FloatingKeyboard ||
                restored.snapshots[4] is not FloatingBrowser { IsWebcam: true } ||
                restored.snapshots[5] is not FloatingBrowser browser || browser.LoadedAddress != url || browser.Zoom != 1.4 ||
                restored.pageKey != "section:RoomSection" || restored.restoreOffset != 124.5 ||
                restored.ReadPlacement(0)?.SequenceEqual(pose) != true ||
                restored.ReadPlacement(4)?.SequenceEqual(pose) != true ||
                restored.ReadPlacement(5)?.SequenceEqual(pose) != true ||
                restored.ReadPlacement(6)?.SequenceEqual(pose) != true)
                throw new InvalidOperationException("The workspace lost a panel, its placement, or its content on reopen.");
            // Reopen and close before rendering a frame. The deferred scroll
            // offset should survive when the overlay has not yet been opened.
            restored.Dispose();
            using var reopened = Open();
            if (reopened.restoreOffset != 124.5 || reopened.snapshots[5] is not FloatingBrowser reloaded)
                throw new InvalidOperationException("The workspace lost the settings scroll offset before the panel was visible.");
            long deadline = Environment.TickCount64 + 8000;
            while (reloaded.PageTitle != "Saved floating page" && Environment.TickCount64 < deadline) await Task.Delay(50);
            if (reloaded.PageTitle != "Saved floating page")
                throw new InvalidOperationException("The restored floating browser did not load the saved page.");
            reopened.enabled = true;
            void AimAt(int panel)
            {
                reopened.TrackObservedPanel([(panel * 16, .8f, .3f, 0)]);
                reopened.panelDwellStart -= 1200;
            }
            AimAt(6);
            if (reopened.CaptureObservedPanel(false) is not { Surface: "browser", Frame.PointerX: > 500 })
                throw new InvalidOperationException("Helpful mode did not capture the pointed browser and cursor position.");
            reopened.TrackObservedPanel([(6 * 16, .8f, .3f, 0), (6 * 16 + PanelFeedback.Leave, 0, 0, 0)]);
            if (reopened.CaptureGuidePanel() is not { Surface: "browser", Frame.PointerX: > 500 })
                throw new InvalidOperationException("Guide Me must retain the pointed browser and cursor after the beam leaves it.");
            AimAt(5);
            if (reopened.CaptureObservedPanel(false) != null)
                throw new InvalidOperationException("Helpful mode must exclude the webcam viewport.");
            AimAt(0);
            if (reopened.CaptureObservedPanel(true) is not { Surface: "settings" } || reopened.CaptureObservedPanel(false) != null)
                throw new InvalidOperationException("Helpful mode must capture settings only while the settings panel is visible.");
            File.WriteAllText(Path.Combine(directory, "workspace-pass.txt"),
                "PASS: workspace restore and pointed-panel capture, including webcam exclusion.");
        }
        finally { File.Delete(file); File.Delete(unreadable); }
    }
}
