using System.Text.Json;
using System.Text.Json.Serialization;
using Automaton.Core.Models;

namespace PersonaBot;

/// <summary>
/// Protects the provider API key at rest. Windows uses DPAPI for the current user; Android uses
/// an AES key held in Android Keystore. Each host sets <see cref="Settings.KeyStore"/> at startup.
/// </summary>
public interface IKeyStore
{
    string Protect(string plain);

    string Unprotect(string protectedValue);
}

public sealed class Settings
{
    public Settings ForMachine(Machine machine)
    {
        var copy = (Settings)MemberwiseClone();
        var resources = MachineResources.Load(machine, this);
        copy.MemoryMb = resources.MemoryMb; copy.DiskGb = resources.DiskGb;
        return copy;
    }
    /// <summary>Set by the host before settings are read or written. Unset means no key can be stored.</summary>
    [JsonIgnore] public static IKeyStore? KeyStore { get; set; }

    public string ProviderType { get; set; } = "openai-compatible";
    public string BaseUrl { get; set; } = "https://api.openai.com/v1";
    public string Model { get; set; } = "";
    public string? Verbosity { get; set; }
    public bool MultilineInput { get; set; }

    /// <summary>After each reply, predict the person's next message and offer it in the empty chat box (Automaton's "Suggest replies"; Tab takes it).</summary>
    public bool SuggestReplies { get; set; } = true;

    /// <summary>Show the model's thinking in the chat (Automaton's "Show Thinking"); one setting for every chat.</summary>
    public bool ShowThinking { get; set; } = true;

    /// <summary>Show tool calls and their results in the chat (Automaton's "Show Tool Calls").</summary>
    public bool ShowToolCalls { get; set; } = true;
    public string ChatDockPosition { get; set; } = "Right";
    public bool ChatFloating { get; set; }
    public bool SharedClipboard { get; set; } = true;
    public bool SharedFiles { get; set; } = true;
    public bool TaskListCollapsed { get; set; }
    public string Theme { get; set; } = "default";
    public string Language { get; set; } = "system";
    public double FullscreenExitX { get; set; }
    public double FullscreenExitY { get; set; }
    public double ChatOverlayOpacity { get; set; } = .86;
    public double ChatOverlayWidth { get; set; } = 460;
    public double ChatOverlayHeight { get; set; } = 600;
    public double ChatOverlayX { get; set; } = .95;
    public double ChatOverlayY { get; set; } = .12;

    /// <summary>The floating chat minimised to the bot (speech bubbles only); and where the bot sits, as shares of the free space.</summary>
    public bool ChatMinimized { get; set; }

    /// <summary>The desktop toolbar shows icons only; the chat list is collapsed to a narrow rail.</summary>
    public bool ToolbarCollapsed { get; set; }
    public bool ChatListCollapsed { get; set; }

    /// <summary>
    /// The helper bot (the open persona's 3D bot): "Helpful" keeps you company beside the pointer and explains what it's
    /// near, as on personabot.uk; "Quiet" stays in its corner, watching, saying nothing; "Off" has none. The Bot control
    /// switch, Settings → Helper bot, and Shift+F1 set it.
    /// </summary>
    public string HelperBot { get; set; } = "Helpful";

    /// <summary>
    /// In Helpful mode the helper bot glances around the pointer now and then and, when something's new, makes a
    /// remark (Observer): it sends the text read near the pointer to the AI provider.
    /// </summary>
    public bool HelperWatches { get; set; } = true;

    /// <summary>The Control switch ("Off", "On" or "Shared"): who uses the desktop, kept between app restarts.</summary>
    public string Control { get; set; } = "Off";

    /// <summary>The first helper bot setting (16 Nov 2026 builds): read once (false meant Off), never written.</summary>
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public bool? MascotCompanion
    {
        get => null;
        set { if (value == false) HelperBot = "Off"; }
    }

    /// <summary>The bottom panel (recordings strip and toolbar) folded down to a slim icon row.</summary>
    public bool BottomPanelCollapsed { get; set; }
    public double ChatBubbleX { get; set; } = .97;
    public double ChatBubbleY { get; set; } = .9;

    /// <summary>The floating calendar window: size, and position as a share of the free space (like the floating chat).</summary>
    public double CalendarWindowWidth { get; set; } = 940;
    public double CalendarWindowHeight { get; set; } = 680;
    public double CalendarWindowX { get; set; } = .5;
    public double CalendarWindowY { get; set; } = .1;

    /// <summary>The floating VM window (machines and snapshots).</summary>
    public double VmWindowWidth { get; set; } = 720;
    public double VmWindowHeight { get; set; } = 640;
    public double VmWindowX { get; set; } = .9;
    public double VmWindowY { get; set; } = .08;

    /// <summary>The floating recording viewer's size and place.</summary>
    public double RecordingWindowWidth { get; set; } = 900;
    public double RecordingWindowHeight { get; set; } = 680;
    public double RecordingWindowX { get; set; } = .5;
    public double RecordingWindowY { get; set; } = .1;
    public string? ReasoningEffort { get; set; }
    public List<string>? ReasoningEffortLevels { get; set; }
    public string ProtectedApiKey { get; set; } = "";
    /// <summary>The Windows host pins its packaged runtime here, overriding legacy saved paths.</summary>
    [JsonIgnore] public static string? QemuPathOverride { get; set; }
    private string _qemuPath = Path.Combine(AppContext.BaseDirectory, "qemu", "qemu-system-x86_64.exe");
    public string QemuPath { get => QemuPathOverride ?? _qemuPath; set => _qemuPath = value; }
    public int CpuCount { get; set; } = Environment.ProcessorCount;
    public int MemoryMb { get; set; } = 4096;

    /// <summary>Use the host's hypervisor (WHPX on Windows) for machines of its own architecture, when it is switched on. Applies at the next start.</summary>
    public bool UseHypervisor { get; set; } = true;

    /// <summary>
    /// Each bot works on a screen of its own inside a Debian or Ubuntu machine (XMEETING.md §3), instead of
    /// all bots sharing the main display. The Website Tester, the desktop's own user, stays on the main display.
    /// </summary>
    public bool BotsUseOwnScreens { get; set; } = true;

    /// <summary>Try shared VirGL graphics for Windows Debian/Ubuntu machines at the next boot.</summary>
    public bool UseSharedGpu { get; set; }

    /// <summary>Give Windows Debian/Ubuntu machines a sound card played on this PC, at the next boot.</summary>
    public bool PlaySound { get; set; } = true;
    public bool VrexGuestAudio { get; set; } = true;
    public string? VrexThinking { get; set; }
    public int VrexAvatar { get; set; }
    public uint VrexShellRgb { get; set; }
    public Dictionary<int, string> VrexAvatarVoices { get; set; } = [];

    /// <summary>Everything PersonaBot plays is muted: the bots' voices and the computers' sound (the Sound pill).</summary>
    public bool SoundMuted { get; set; }

    /// <summary>The volume of everything PersonaBot plays, 0–1 (the Sound pill).</summary>
    public double SoundVolume { get; set; } = 1.0;

    /// <summary>Syslog, and whether the machines' own logs go there too (PersonaBotLogging).</summary>
    public LogSettings Logging { get; set; } = new();

    /// <summary>The bots' voice: which one, how fast, and when they speak (see <see cref="VoiceModels"/>).</summary>
    public VoiceSettings Voice { get; set; } = new();

    /// <summary>Which sections of the Settings window are open, by name; missing ones use their default.</summary>
    public Dictionary<string, bool> SettingsSections { get; set; } = [];
    public int DiskGb { get; set; } = 10;
    public int MaxModelTurns { get; set; } = 300;
    /// <summary>Automaton's "Max Context Tokens": how much the model can read. Blank uses Automaton's default.</summary>
    public int? MaxContextTokens { get; set; }
    /// <summary>Automaton's "Max Output Tokens": the cap on one reply. Blank uses Automaton's default.</summary>
    public int? MaxOutputTokens { get; set; }
    /// <summary>Automaton's defaults when the two are left blank (LlmConfig.ContextLimitTokens and MaxTokens).</summary>
    public const int DefaultContextTokens = 90000;
    public const int DefaultOutputTokens = 8192;
    [JsonIgnore] public int EffectiveContextTokens => MaxContextTokens ?? DefaultContextTokens;
    [JsonIgnore] public int EffectiveOutputTokens => MaxOutputTokens ?? DefaultOutputTokens;
    /// <summary>The compaction settings Automaton builds from the same values (MainWindowViewModel.BuildCompressionSettings).</summary>
    public Automaton.Core.Services.ConversationCompressionSettings CreateCompressionSettings() => new()
    {
        ContextLimitTokens = EffectiveContextTokens, RecentMessagesToKeep = 12, SummaryMaxCharacters = 6000
    };
    public bool SetupComplete { get; set; }
    /// <summary>Optional host-owned data directory; existing hosts retain their original default.</summary>
    [JsonIgnore] public static string? DataDirectory { get; set; }
    [JsonIgnore] public static string Root => DataDirectory ?? DefaultRoot;
    /// <summary>The data folder under LocalApplicationData.</summary>
    [JsonIgnore] public static string DefaultRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PersonaBot");
    /// <summary>
    /// Where the virtual machines' files go (their disks, seeds and snapshots), as the person chose it;
    /// null for the default, <see cref="DefaultVmDirectory"/>. Changing it on its own moves nothing: the
    /// VM window moves the machines with <see cref="MachineFiles.MoveAsync"/> and then saves it.
    /// </summary>
    public string? VmFolder { get; set; }
    public string? AndroidDisplayResolution { get; set; }
    public string? VrexUbuntuVmFolder { get; set; }
    public string? VrexUbuntuDisplayResolution { get; set; }

    /// <summary>The saved VM folder; a settings file that can't be read means the default.</summary>
    private static string? SavedVmFolder()
    {
        try
        {
            return Load().VmFolder;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            System.Diagnostics.Trace.WriteLine($"Settings.SavedVmFolder: settings unreadable ({ex.Message}); using the default VM folder");
            return null;
        }
    }

    /// <summary>The default VM folder: Vm in PersonaBot's data folder.</summary>
    [JsonIgnore] public static string DefaultVmDirectory => Path.Combine(Root, "Vm");

    private static readonly object VmFolderGate = new();
    private static string? _savedVmFolder;
    private static string? _savedVmFolderRoot;

    /// <summary>
    /// The VM folder in use: the saved <see cref="VmFolder"/>, else <see cref="DefaultVmDirectory"/>. Read
    /// from settings.json once per data folder and kept current by <see cref="Save"/>.
    /// </summary>
    [JsonIgnore] public static string VmDirectory
    {
        get
        {
            lock (VmFolderGate)
            {
                if (_savedVmFolderRoot != Root)
                {
                    _savedVmFolderRoot = Root;
                    _savedVmFolder = SavedVmFolder();
                }
                return string.IsNullOrWhiteSpace(_savedVmFolder) ? DefaultVmDirectory : Path.GetFullPath(_savedVmFolder);
            }
        }
    }
    [JsonIgnore] public string ApiKey
    {
        get => ProtectedApiKey.Length == 0 ? "" : RequireKeyStore().Unprotect(ProtectedApiKey);
        set => ProtectedApiKey = string.IsNullOrEmpty(value) ? "" : RequireKeyStore().Protect(value);
    }
    private static IKeyStore RequireKeyStore() =>
        KeyStore ?? throw new InvalidOperationException("No key store is configured on this platform, so the API key cannot be stored or read.");
    public static Settings Load()
    {
        var path = Path.Combine(Root, "settings.json");
        System.Diagnostics.Trace.WriteLine($"Settings.Load enter: {path}");
        if (!File.Exists(path))
        {
            System.Diagnostics.Trace.WriteLine("Settings.Load exit: no settings file, using the defaults");
            return new();
        }
        var settings = JsonSerializer.Deserialize<Settings>(File.ReadAllText(path)) ?? new();
        System.Diagnostics.Trace.WriteLine($"Settings.Load exit: provider {settings.ProviderType} at {settings.BaseUrl}, model {settings.Model}, API key {(string.IsNullOrEmpty(settings.ProtectedApiKey) ? "none" : "stored")}, setup complete={settings.SetupComplete}, {settings.CpuCount} CPUs, {settings.MemoryMb} MB, VM folder {settings.VmFolder ?? "default"}");
        return settings;
    }
    public ProviderConfig CreateProviderConfig()
    {
        // Never the key itself: only whether there is one.
        System.Diagnostics.Trace.WriteLine($"Settings.CreateProviderConfig: {ProviderType} at {BaseUrl}, model {Model}, API key {(ProtectedApiKey.Length == 0 ? "none" : "set")}, reasoning {VrexThinking ?? ReasoningEffort ?? "default"}, context {MaxContextTokens?.ToString() ?? "default"}, output {MaxOutputTokens?.ToString() ?? "default"}");
        var config = new ProviderConfig
        {
            Type = ProviderType, BaseUrl = BaseUrl, ApiKey = ApiKey, DefaultModel = Model,
            MaxContextTokens = MaxContextTokens, MaxOutputTokens = MaxOutputTokens,
            ReasoningEffortLevels = ReasoningEffortSettings.ParseLevels(string.Join(',', ReasoningEffortLevels ?? []))
        };
        if (VrexThinking == null) return ReasoningEffortSettings.ApplyTo(config, ReasoningEffort);
        if (VrexThinking is "none" or "low" or "high" or "max")
            config.RequestParameters["reasoning_effort"] = JsonSerializer.SerializeToElement(VrexThinking);
        if (VrexThinking == "none" && ProviderType == "openai-compatible" &&
            Uri.TryCreate(BaseUrl, UriKind.Absolute, out var endpoint) &&
            endpoint.Host.Equals("api.deepseek.com", StringComparison.OrdinalIgnoreCase))
            config.RequestParameters["thinking"] = JsonSerializer.SerializeToElement(new { type = "disabled" });
        return config;
    }
    public void Save()
    {
        System.Diagnostics.Trace.WriteLine($"Settings.Save enter: {Root}, provider {ProviderType}, model {Model}, VM folder {VmFolder ?? "default"}");
        Directory.CreateDirectory(Root);
        var path = Path.Combine(Root, "settings.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(path + ".tmp", path, true);
        lock (VmFolderGate)
        {
            _savedVmFolderRoot = Root;
            _savedVmFolder = VmFolder;
        }
        System.Diagnostics.Trace.WriteLine($"Settings.Save exit: wrote {path}");
    }
    /// <summary>
    /// Checks the provider and chat settings every platform shares. The testing computer's own
    /// settings (QEMU path, CPUs, disk) are checked by the platform that uses them.
    /// </summary>
    public void Validate()
    {
        System.Diagnostics.Trace.WriteLine($"Settings.Validate enter: {MaxModelTurns} turns, context {MaxContextTokens?.ToString() ?? "default"}, output {MaxOutputTokens?.ToString() ?? "default"}, {BaseUrl}, model {Model}");
        if (MaxModelTurns is < 1 or > 10000)
        {
            System.Diagnostics.Trace.TraceWarning($"Settings.Validate: refused {MaxModelTurns} model turns");
            throw new ArgumentException("Use 1–10000 model turns per instruction.");
        }
        if (MaxContextTokens is < 4096 or > 10_000_000)
        {
            System.Diagnostics.Trace.TraceWarning($"Settings.Validate: refused {MaxContextTokens} context tokens");
            throw new ArgumentException("Use 4,096–10,000,000 max context tokens, or leave it blank.");
        }
        if (MaxOutputTokens is < 64 or > 1_000_000)
        {
            System.Diagnostics.Trace.TraceWarning($"Settings.Validate: refused {MaxOutputTokens} output tokens");
            throw new ArgumentException("Use 64–1,000,000 max output tokens, or leave it blank.");
        }
        if (MaxOutputTokens >= EffectiveContextTokens)
        {
            System.Diagnostics.Trace.TraceWarning($"Settings.Validate: refused {MaxOutputTokens} output tokens with {EffectiveContextTokens} context tokens");
            throw new ArgumentException("Max output tokens must be smaller than max context tokens.");
        }
        if (!Uri.TryCreate(BaseUrl, UriKind.Absolute, out var uri) || (uri.Scheme != "https" && !(uri.Scheme == "http" && uri.IsLoopback)))
        {
            System.Diagnostics.Trace.TraceWarning($"Settings.Validate: refused provider URL {BaseUrl}");
            throw new ArgumentException("Use an HTTPS provider URL, or HTTP for a loopback provider.");
        }
        if (string.IsNullOrWhiteSpace(Model))
        {
            System.Diagnostics.Trace.TraceWarning("Settings.Validate: refused: no model");
            throw new ArgumentException("Enter your provider's vision-capable model ID.");
        }
        System.Diagnostics.Trace.WriteLine("Settings.Validate exit: valid");
    }
}
