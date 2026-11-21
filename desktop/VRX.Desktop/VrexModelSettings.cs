using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed class VrexModelSettings : StackPanel, IDisposable
{
    private readonly VrexClient client;
    private readonly TextBox endpoint = new(), model = new();
    private readonly PasswordBox key = new();
    private readonly TextBlock mask = new()
    {
        Text = "••••••••", Margin = new(9, 0, 0, 0), IsHitTestVisible = false,
        VerticalAlignment = VerticalAlignment.Center
    };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button provider, save;
    private readonly Control[] fields;
    private (string Provider, string Endpoint, string Model, bool HasKey)? saved;
    private Control? activeField;
    private bool replaceKeyOnEdit, saving;
    private string providerId = "openai-compatible";

    internal bool IsPasswordActive => activeField == key;

    internal VrexModelSettings(VrexClient client, Action<string>? openBrowser = null,
        Action<Button, Action>? registerButton = null, Action? back = null)
    {
        this.client = client;
        if (back != null) Children.Add(MakeButton("Back to chat", back));
        provider = MakeButton("OpenAI compatible", () =>
        {
            providerId = providerId == "openai-compatible" ? "openai-responses" : "openai-compatible";
            UpdateProvider();
        });
        fields = [endpoint, model, key];
        foreach (var field in fields)
        {
            field.MinHeight = 38;
            field.VerticalContentAlignment = VerticalAlignment.Center;
            field.Padding = new(8, 0, 8, 0);
        }
        key.ToolTip = "Type or paste a new API key to replace the saved key.";
        mask.SetResourceReference(TextBlock.ForegroundProperty, "Muted");
        key.PasswordChanged += (_, _) => UpdateKeyMask();
        var keyField = new Grid();
        keyField.Children.Add(key);
        keyField.Children.Add(mask);
        AddField("Provider", provider, provider);
        AddField("Model", model, model);
        AddField("Endpoint", endpoint, endpoint);
        AddField("API key", keyField, key);
        var shortcuts = new WrapPanel();
        shortcuts.Children.Add(MakeButton("Use DeepSeek Flash", () =>
        {
            providerId = "openai-compatible";
            endpoint.Text = "https://api.deepseek.com";
            model.Text = "deepseek-flash";
            UpdateProvider();
        }));
        shortcuts.Children.Add(MakeButton("Get DeepSeek key", () =>
        {
            const string url = "https://platform.deepseek.com/api_keys";
            try
            {
                if (openBrowser != null) openBrowser(url);
                else System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
            { status.Text = "Couldn't open the DeepSeek key page."; }
        }));
        Children.Add(shortcuts);
        save = MakeButton("Save AI settings", Submit);
        Children.Add(save);
        Children.Add(status);
        if (client.ConfigurationLoaded) Fill(client.Configuration);
        UpdateKeyMask();
        save.IsEnabled = client.ConfigurationLoaded;
        client.Changed += Changed;

        Button MakeButton(string label, Action action)
        {
            var button = new Button { Content = label, MinHeight = 40, Margin = new(0, 3, 8, 3) };
            button.Click += (_, _) => action();
            registerButton?.Invoke(button, action);
            return button;
        }

        void AddField(string label, FrameworkElement element, Control field)
        {
            Children.Add(new TextBlock { Text = label, Margin = new(0, 4, 0, 2) });
            Children.Add(element);
            AutomationProperties.SetName(field, label);
        }
    }

    internal void Connect()
    {
        try { client.EnsureWorker(); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; }
    }

    private void Changed(VrexEvent message)
    {
        if (message.Type == "settings") Fill(message);
        if (saving && message.Type == "model-saved")
        {
            saving = false;
            key.Clear();
            status.Text = "AI settings saved.";
        }
        if (saving && message.Type is ("model-settings-error" or "error"))
        {
            saving = false;
            status.Text = message.Text;
        }
        save.IsEnabled = client.ConfigurationLoaded && !saving;
    }

    private void Fill(VrexEvent message)
    {
        var configuration = (message.Provider ?? "openai-compatible", message.Endpoint ?? "", message.Model ?? "", message.HasKey);
        if (saved == configuration) return;
        if (saved?.Endpoint != configuration.Item2) key.Clear();
        saved = configuration;
        (providerId, endpoint.Text, model.Text, _) = configuration;
        UpdateProvider();
        UpdateKeyMask();
    }

    private void UpdateProvider() => provider.Content =
        providerId == "openai-responses" ? "OpenAI Responses" : "OpenAI compatible";

    private string? ReplacementKey => key.Password.Length > 0 ? key.Password : null;

    private void UpdateKeyMask()
    {
        bool hasKey = saved?.HasKey == true;
        mask.Visibility = hasKey && ReplacementKey == null ? Visibility.Visible : Visibility.Collapsed;
        AutomationProperties.SetHelpText(key, hasKey ? "A key is saved. Type a new key to replace it." : "Enter an API key for your model provider.");
    }

    private void Submit()
    {
        if (!save.IsEnabled) return;
        try
        {
            if (!client.Send(new("configure-model", Model: model.Text, Endpoint: endpoint.Text,
                Provider: providerId, ApiKey: ReplacementKey)))
            { status.Text = client.Status; return; }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { status.Text = ex.Message; return; }
        saving = true;
        save.IsEnabled = false;
        status.Text = "Saving AI settings…";
    }

    internal void ClearSelection() => activeField = null;

    internal bool SelectField(DependencyObject? hit)
    {
        for (var node = hit; node != null; node = VisualTreeHelper.GetParent(node))
        {
            if (node is not Control field || !fields.Contains(field)) continue;
            activeField = field;
            if (field is TextBox text) text.SelectAll();
            if (field == key) { key.SelectAll(); replaceKeyOnEdit = true; }
            return true;
        }
        return false;
    }

    internal void Edit(KeyboardEdit edit)
    {
        if (edit.Key == 13) { Submit(); return; }
        if (activeField is TextBox field) { VrexSetup.EditTextField(field, edit); return; }
        if (!IsPasswordActive) return;
        if (edit.Key == 65 && edit.Modifiers.HasFlag(KeyboardModifiers.Control)) { replaceKeyOnEdit = true; return; }
        int remove = edit.Backspaces + (edit.Key == 8 ? 1 : 0);
        if (remove <= 0 && edit.Text.Length == 0) return;
        string existing = replaceKeyOnEdit ? "" : key.Password;
        replaceKeyOnEdit = false;
        key.Password = existing[..Math.Max(0, existing.Length - remove)] + edit.Text;
    }

    internal static void SmokeSavedKey()
    {
        using var setup = new VrexModelSettings(VrexClient.Current);
        setup.Fill(new("settings", HasKey: true));
        if (setup.mask.Visibility != Visibility.Visible || setup.ReplacementKey != null)
            throw new InvalidOperationException("Saved key masking must leave the replacement field empty.");
        setup.SelectField(setup.key);
        setup.Edit(new("fixture-replacement"));
        if (setup.mask.Visibility != Visibility.Collapsed || setup.ReplacementKey != "fixture-replacement")
            throw new InvalidOperationException("Typing a replacement must hide the saved key mask.");
        setup.saving = true;
        setup.Changed(new("model-saved"));
        if (setup.mask.Visibility != Visibility.Visible || setup.ReplacementKey != null)
            throw new InvalidOperationException("Saving must restore the masked key field.");
    }

    public void Dispose()
    {
        client.Changed -= Changed;
        key.Clear();
    }
}
