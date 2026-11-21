using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed class VrexCompanionSettings : StackPanel, IDisposable
{
    private readonly VrexClient client = VrexClient.Current;
    private readonly Action<Button, Action>? register;
    private readonly Action<Button>? unregister;
    private readonly Action? invalidate;
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private bool fillingVoice;
    private sealed record VoiceOption(string Id, string Label) { public override string ToString() => Label; }
    private readonly ComboBox? voiceCombo;
    private readonly Button? voicePicker;
    private readonly StackPanel? voiceChoices;
    private readonly Button installVoice;
    private readonly Button avatarPicker;
    private readonly StackPanel avatarChoices = new() { Visibility = Visibility.Collapsed };
    private readonly StackPanel shellColours = new();
    private readonly Dictionary<uint, Button> colourButtons = [];

    internal VrexCompanionSettings(Action<Button, Action>? register = null, Action<Button>? unregister = null, Action? invalidate = null)
    {
        this.register = register;
        this.unregister = unregister;
        this.invalidate = invalidate;
        Children.Add(new TextBlock { Text = "Companion model", Margin = new(0, 5, 0, 2) });
        avatarPicker = MakeButton("Robot ▾", () => avatarChoices.Visibility =
            avatarChoices.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
        AutomationProperties.SetName(avatarPicker, "Companion model");
        Children.Add(avatarPicker);
        Children.Add(avatarChoices);
        for (int index = 0; index < VrexAppearance.Avatars.Length; index++)
        {
            int avatar = index;
            avatarChoices.Children.Add(MakeButton(VrexAppearance.Avatars[index], () =>
            {
                avatarChoices.Visibility = Visibility.Collapsed;
                if (avatar != client.Avatar) client.Send(new("avatar-select", Key: avatar));
            }));
        }
        shellColours.Children.Add(new TextBlock { Text = "Robot shell colour", Margin = new(0, 5, 0, 2) });
        var swatches = new WrapPanel();
        foreach (var (name, rgb) in VrexAppearance.Colours)
        {
            var swatch = MakeButton(name, () => client.Send(new("shell-select", Key: (int)rgb)));
            colourButtons.Add(rgb, swatch);
            swatches.Children.Add(swatch);
        }
        shellColours.Children.Add(swatches);
        Children.Add(shellColours);
        Children.Add(new TextBlock { Text = "Companion voice", Margin = new Thickness(0, 5, 0, 2) });
        var voiceControls = new DockPanel();
        installVoice = MakeButton("Install local voices", () => client.Send(new("voice-install")));
        installVoice.ToolTip = "Downloads the local speech model. Live voice also installs recognition and barge-in models.";
        DockPanel.SetDock(installVoice, Dock.Right);
        voiceControls.Children.Add(installVoice);
        if (register == null)
        {
            voiceCombo = new ComboBox { MinHeight = 34 };
            AutomationProperties.SetName(voiceCombo, "Companion voice");
            voiceCombo.SelectionChanged += (_, _) =>
            {
                if (!fillingVoice && voiceCombo.SelectedItem is VoiceOption option) client.Send(new("voice-select", option.Id));
            };
            voiceControls.Children.Add(voiceCombo);
        }
        else
        {
            voiceChoices = new StackPanel { Visibility = Visibility.Collapsed };
            voicePicker = MakeButton("Voice ▾", () => voiceChoices.Visibility =
                voiceChoices.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
            AutomationProperties.SetName(voicePicker, "Companion voice");
            voiceControls.Children.Add(voicePicker);
        }
        Children.Add(voiceControls);
        if (voiceChoices != null) Children.Add(voiceChoices);
        Children.Add(status);
        FillVoice(client.VoiceOptions);
        Refresh();
        client.Changed += Changed;

        Button MakeButton(string label, Action action)
        {
            var button = new Button { Content = label, Margin = new(0, 3, 7, 0), MinHeight = 34 };
            button.Click += (_, _) => action();
            register?.Invoke(button, action);
            return button;
        }
    }

    internal void Connect()
    {
        if (client.Running) return;
        try { client.EnsureWorker(); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; status.Visibility = Visibility.Visible; }
    }

    private void Changed(VrexEvent message)
    {
        if (message.Type == "voice-options") FillVoice(message);
        if (message.Type is "settings" or "voice-options" or "voice-state" or "appearance" or "error")
        {
            Refresh();
            invalidate?.Invoke();
        }
    }

    private void FillVoice(VrexEvent message)
    {
        var options = (message.Choices ?? []).Select(item => item.Split('\t', 2))
            .Where(parts => parts.Length == 2).Select(parts => new VoiceOption(parts[0], parts[1])).ToArray();
        var selected = options.FirstOrDefault(option => option.Id == message.Text);
        if (voiceCombo != null)
        {
            fillingVoice = true;
            voiceCombo.ItemsSource = options;
            voiceCombo.SelectedItem = selected;
            fillingVoice = false;
        }
        if (voiceChoices != null)
        {
            foreach (var previous in voiceChoices.Children.OfType<Button>()) unregister?.Invoke(previous);
            voiceChoices.Children.Clear();
            voicePicker!.Content = (selected?.Label ?? "Voice") + " ▾";
            foreach (var option in options)
            {
                var button = new Button { Content = option.Label, MinHeight = 36, Margin = new Thickness(0, 2, 8, 0) };
                Action select = () =>
                {
                    voiceChoices.Visibility = Visibility.Collapsed;
                    client.Send(new("voice-select", option.Id));
                };
                button.Click += (_, _) => select();
                register?.Invoke(button, select);
                voiceChoices.Children.Add(button);
            }
        }
    }

    private void Refresh()
    {
        int avatar = Math.Clamp(client.Avatar, 0, VrexAppearance.Avatars.Length - 1);
        avatarPicker.Content = VrexAppearance.Avatars[avatar] + " ▾";
        avatarPicker.IsEnabled = client.ConfigurationLoaded;
        shellColours.Visibility = avatar == 0 ? Visibility.Visible : Visibility.Collapsed;
        uint shell = client.ShellRgb == 0 ? VrexAppearance.Colours[0].Rgb : client.ShellRgb;
        foreach (var (name, rgb) in VrexAppearance.Colours)
        {
            colourButtons[rgb].IsEnabled = client.ConfigurationLoaded;
            colourButtons[rgb].Content = shell == rgb ? name + " ✓" : name;
        }
        installVoice.IsEnabled = client.ConfigurationLoaded && !client.LocalVoiceInstalled && !client.LiveVoiceActive;
        installVoice.Content = client.LocalVoiceInstalled ? "Local voices ready" : "Install local voices";
        status.Text = client.VoiceStatus;
        status.Visibility = string.IsNullOrWhiteSpace(status.Text) ? Visibility.Collapsed : Visibility.Visible;
    }

    public void Dispose() => client.Changed -= Changed;
}
