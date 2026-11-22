using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed class VrexSetup : StackPanel, IDisposable
{
    private readonly VrexClient client = VrexClient.Current;
    private readonly TextBox memory = new(), vmPath = new();
    private const int MinCpuCount = 1, MaxCpuCount = 8;
    private int cpuCount;
    private readonly TextBlock cpuValue = new() { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button fewerCpus, moreCpus;
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap, Margin = new(0, 6, 0, 0) };
    private readonly VrexVmIndicator vmIndicator = new();
    private readonly TextBlock currentPath = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button save, start, stop;
    private readonly Button applyPath;
    private readonly Button? browsePath;
    private readonly Button guestAudio;
    private bool playGuestAudio = true;
    private VrexResolution resolution = VrexResolution.Default;
    private readonly ComboBox? resolutionCombo;
    private readonly Button? resolutionButton;
    private readonly StackPanel? resolutionChoices;
    private TextBox? activeField;

    internal VrexSetup(Dictionary<Button, Action>? floatingActions = null, Action? confirmStop = null)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new());
        grid.ColumnDefinitions.Add(new());
        foreach (var field in new[] { memory, vmPath })
        {
            field.VerticalContentAlignment = VerticalAlignment.Center;
            field.Padding = new Thickness(8, 0, 8, 0);
        }
        var cpuPicker = new DockPanel();
        fewerCpus = CpuButton("−", Loc.Get("UbuntuDecreaseCpus"), Dock.Left, -1);
        moreCpus = CpuButton("+", Loc.Get("UbuntuIncreaseCpus"), Dock.Right, 1);
        var valueFace = new Border { Child = cpuValue, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6) };
        valueFace.SetResourceReference(Border.BackgroundProperty, "Field");
        valueFace.SetResourceReference(Border.BorderBrushProperty, "RaisedEdge");
        cpuPicker.Children.Add(valueFace);

        var items = new (string Label, FrameworkElement Field)[]
        {
            (Loc.Get("UbuntuVirtualCpus"), cpuPicker), (Loc.Get("UbuntuMemory"), memory)
        };
        for (int i = 0; i < items.Length; i++)
        {
            var (label, field) = items[i];
            var group = new StackPanel { Margin = new(0, 3, 8, 3) };
            group.Children.Add(new TextBlock { Text = label });
            field.MinHeight = 34;
            group.Children.Add(field);
            if (i % 2 == 0) grid.RowDefinitions.Add(new() { Height = GridLength.Auto });
            Grid.SetColumn(group, i % 2);
            Grid.SetRow(group, i / 2);
            grid.Children.Add(group);
        }
        if (floatingActions == null) Children.Add(vmIndicator);
        Children.Add(grid);
        Children.Add(new TextBlock { Text = Loc.Get("UbuntuResolutionNextStart"), Margin = new Thickness(0, 5, 0, 2) });
        if (floatingActions == null)
        {
            resolutionCombo = new ComboBox { ItemsSource = VrexResolution.Options, SelectedItem = resolution, MinHeight = 34 };
            AutomationProperties.SetName(resolutionCombo, Loc.Get("UbuntuResolution"));
            resolutionCombo.SelectionChanged += (_, _) =>
            {
                if (resolutionCombo.SelectedItem is VrexResolution choice) resolution = choice;
            };
            Children.Add(resolutionCombo);
        }
        else
        {
            resolutionChoices = new StackPanel { Visibility = Visibility.Collapsed };
            resolutionButton = MakeButton("", () => resolutionChoices.Visibility =
                resolutionChoices.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
            AutomationProperties.SetName(resolutionButton, Loc.Get("UbuntuResolution"));
            Children.Add(resolutionButton);
            Children.Add(resolutionChoices);
            foreach (var choice in VrexResolution.Options)
                resolutionChoices.Children.Add(MakeButton(choice.ToString(), () => SelectResolution(choice)));
        }
        guestAudio = MakeButton("", () => { playGuestAudio = !playGuestAudio; UpdateLabels(); });
        Children.Add(guestAudio);
        Children.Add(new TextBlock { Text = Loc.Get("UbuntuVmFolder"), Margin = new Thickness(0, 5, 0, 2) });
        var location = new DockPanel();
        applyPath = MakeButton(Loc.Get("UbuntuSetFolder"), ChangeStorage);
        DockPanel.SetDock(applyPath, Dock.Right);
        location.Children.Add(applyPath);
        if (floatingActions == null)
        {
            browsePath = MakeButton(Loc.Get("UbuntuBrowseFolder"), () =>
            {
                var picker = new Microsoft.Win32.OpenFolderDialog { Title = Loc.Get("UbuntuChooseFolder") };
                if (picker.ShowDialog(Window.GetWindow(this)) == true) vmPath.Text = picker.FolderName;
            });
            DockPanel.SetDock(browsePath, Dock.Right);
            location.Children.Add(browsePath);
        }
        vmPath.MinHeight = 34;
        location.Children.Add(vmPath);
        Children.Add(location);
        Children.Add(currentPath);
        Children.Add(new TextBlock
        {
            Text = Loc.Get("UbuntuVmFolderHint"),
            TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 3, 0, 0)
        });
        var buttons = new WrapPanel { Margin = new(0, 6, 0, 4) };
        save = MakeButton(Loc.Get("SnapshotSave"), () => Submit(false));
        start = MakeButton(Loc.Get("UbuntuStart"), () => Submit(true));
        stop = MakeButton(Loc.Get("UbuntuStop"), () =>
        {
            if (client.VmPhase == VrexVmPhase.Moving) { client.StopUbuntu(); return; }
            if (confirmStop != null) { confirmStop(); return; }
            if (MessageBox.Show(Loc.Get("UbuntuPowerOffQuestion") + " " + Loc.Get("UbuntuUnsavedWork"), Loc.Get("UbuntuStop"),
                    MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) == MessageBoxResult.Yes)
                client.StopUbuntu();
        });
        foreach (var button in new[] { save, start, stop }) buttons.Children.Add(button);
        Children.Add(buttons);
        Children.Add(status);
        Children.Add(new TextBlock
        {
            Text = Loc.Get("UbuntuFirstStartHint"),
            TextWrapping = TextWrapping.Wrap, Margin = new(0, 4, 0, 0)
        });
        Fill(client.Configuration);
        client.Changed += Changed;
        Refresh();

        Button MakeButton(string label, Action action)
        {
            var button = new Button { Content = label, Margin = new(0, 3, 7, 0), MinHeight = 34 };
            button.Click += (_, _) => action();
            floatingActions?.Add(button, action);
            return button;
        }

        Button CpuButton(string label, string description, Dock side, int adjustment)
        {
            var button = MakeButton(label, () => SetCpuCount(cpuCount + adjustment));
            button.Width = 44;
            button.Padding = new Thickness(0);
            button.Margin = side == Dock.Left ? new Thickness(0, 0, 5, 0) : new Thickness(5, 0, 0, 0);
            button.ToolTip = description;
            AutomationProperties.SetName(button, description);
            DockPanel.SetDock(button, side);
            cpuPicker.Children.Add(button);
            return button;
        }
    }

    internal void Connect()
    {
        if (client.Running) return;
        try { client.EnsureWorker(); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; }
    }

    private void Fill(VrexEvent settings)
    {
        SetCpuCount(settings.CpuCount);
        memory.Text = settings.MemoryMb.ToString();
        playGuestAudio = settings.PlayGuestAudio;
        SelectResolution(VrexResolution.Find(settings.Resolution) ?? VrexResolution.Default);
        vmPath.Text = settings.VmPath ?? "";
        currentPath.Text = string.IsNullOrEmpty(settings.VmPath) ? "" : Loc.Format("UbuntuCurrentFolder", settings.VmPath);
        applyPath.Content = settings.VmInstalled ? Loc.Get("UbuntuMoveVm") : Loc.Get("UbuntuSetFolder");
        UpdateLabels();
    }

    private void SelectResolution(VrexResolution choice)
    {
        resolution = choice;
        if (resolutionCombo != null) resolutionCombo.SelectedItem = choice;
        if (resolutionButton != null) resolutionButton.Content = $"{choice}  ▾";
        if (resolutionChoices != null) resolutionChoices.Visibility = Visibility.Collapsed;
    }

    private void SetCpuCount(int count)
    {
        cpuCount = Math.Clamp(count, MinCpuCount, MaxCpuCount);
        cpuValue.Text = cpuCount.ToString();
        fewerCpus.IsEnabled = cpuCount > MinCpuCount;
        moreCpus.IsEnabled = cpuCount < MaxCpuCount;
    }

    private void UpdateLabels()
    {
        guestAudio.Content = Loc.Format("UbuntuAudioNextStart", Loc.Get(playGuestAudio ? "ControllersOn" : "ControllersOff"));
    }

    private void Changed(VrexEvent message)
    {
        if (message.Type == "settings") Fill(message);
        if (message.Type is "settings" or "stopped" or "state" or "status" or "ready" or "error" or "vm-status") Refresh();
    }

    private void Refresh()
    {
        vmIndicator.Update(client);
        start.IsEnabled = client.ConfigurationLoaded && !client.VmBusy && !client.Ready;
        save.IsEnabled = client.ConfigurationLoaded && !client.VmBusy;
        bool moving = client.VmPhase == VrexVmPhase.Moving;
        stop.Content = moving ? Loc.Get("UbuntuCancelMove") : Loc.Get("UbuntuStop");
        stop.IsEnabled = client.Running && client.VmPhase != VrexVmPhase.Stopping &&
            (moving || client.Ready || client.VmPhase is VrexVmPhase.Preparing or VrexVmPhase.Booting);
        vmPath.IsEnabled = applyPath.IsEnabled = start.IsEnabled;
        if (browsePath != null) browsePath.IsEnabled = vmPath.IsEnabled;
        status.Text = client.Status == client.VmDetail ? "" : client.Status;
    }

    private void ChangeStorage()
    {
        try
        {
            string action = client.Configuration.VmInstalled ? "move-vm" : "set-vm-path";
            if (!client.Send(new(action, VmPath: vmPath.Text))) status.Text = client.Status;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; }
    }

    private void Submit(bool boot)
    {
        if (!int.TryParse(memory.Text, out int ram) || ram is < 4096 or > 16384)
        { status.Text = Loc.Get("UbuntuMemoryRange"); return; }
        try
        {
            if (!client.Send(new(boot ? "configure-ubuntu-start" : "configure-ubuntu",
                CpuCount: cpuCount, MemoryMb: ram, Resolution: resolution.Key,
                PlayGuestAudio: playGuestAudio)))
            { status.Text = client.Status; return; }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; return; }
        UpdateLabels();
    }

    internal bool SelectField(DependencyObject? hit)
    {
        activeField = null;
        while (hit != null)
        {
            if (hit is TextBox field && (field == memory || field == vmPath))
            {
                activeField = field;
                field.SelectAll();
                return true;
            }
            hit = System.Windows.Media.VisualTreeHelper.GetParent(hit);
        }
        return false;
    }

    internal void Edit(KeyboardEdit edit)
    {
        if (activeField != null) EditTextField(activeField, edit);
    }

    internal static void EditTextField(TextBox field, KeyboardEdit edit)
    {
        int remove = edit.Backspaces + (edit.Key == 8 ? 1 : 0);
        var (start, count) = (field.SelectionStart, field.SelectionLength);
        if (edit.Key == 65 && edit.Modifiers.HasFlag(KeyboardModifiers.Control)) { field.SelectAll(); return; }
        if (edit.Key is 37 or 39)
        {
            field.Select(Math.Clamp(start + (edit.Key == 37 ? -1 : 1), 0, field.Text.Length), 0);
            return;
        }
        if (count == 0 && remove > 0) { count = Math.Min(remove, start); start -= count; }
        if (count == 0 && edit.Key == 46 && start < field.Text.Length) count = 1;
        if (remove == 0 && edit.Key != 46 && edit.Text.Length == 0) return;
        field.Select(start, count);
        field.SelectedText = edit.Text;
        field.Select(start + edit.Text.Length, 0);
    }

    public void Dispose() => client.Changed -= Changed;
}
