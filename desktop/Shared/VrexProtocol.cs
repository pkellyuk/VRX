namespace VRX.VrexBridge;

internal static class VrexProtocol
{
    internal const int Version = 1, Magic = 0x56525841, HeaderBytes = 64;
    internal const int MaxWidth = 1920, MaxHeight = 1080, Capacity = HeaderBytes + MaxWidth * MaxHeight * 4;
    internal const int MaxPcImageBytes = 4 * 1024 * 1024, MaxRequestChars = MaxPcImageBytes * 4 / 3 + 65536;
    internal const int MaxEventChars = 65536, MaxGridEventChars = MaxPcImageBytes * 4 / 3 + MaxEventChars;
    internal const int GuestAudioRate = 48000, GuestAudioPacketBytes = 19200;
    internal const string MachineId = "vrex-ubuntu", PersonaId = "vrex", Distribution = "ubuntu-24.04", Architecture = "x86_64";
    internal const string ChannelPrefix = @"Local\VRX.Vrex.";
    internal static bool IsChannel(string value) => value.StartsWith(ChannelPrefix, StringComparison.Ordinal) &&
        Guid.TryParseExact(value[ChannelPrefix.Length..], "N", out _);
}

internal static class VrexAppearance
{
    internal static readonly string[] Avatars = ["Robot", "Vampire", "Winged demon"];
    internal static readonly (string Name, uint Rgb)[] Colours =
        [("Teal", 0x06947D), ("Red", 0xCA3042), ("Blue", 0x297FE2), ("Yellow", 0xE6A91C)];
    internal static string DefaultVoice(int avatar) => avatar switch
    {
        1 => "bf_emma",
        2 => "bm_george",
        _ => "bm_fable"
    };
}

internal readonly record struct VrexResolution(int Width, int Height)
{
    internal static readonly VrexResolution Default = new(1280, 800);
    internal static readonly IReadOnlyList<VrexResolution> Options =
        [new(960, 600), Default, new(1600, 1000), new(1920, 1080)];
    internal string Key => $"{Width}x{Height}";
    public override string ToString() => $"{Width} × {Height}";
    internal static VrexResolution? Find(string? key)
    {
        foreach (var option in Options)
            if (option.Key == key) return option;
        return null;
    }
}

internal static class VrexPcPrompt
{
    internal const string MissingContext = "\n[The PC display was unavailable for this question. Don't infer what is on it from the Ubuntu screen.]";
    internal const string GuidePrefix = "Guide me based on the supplied screen.";
    private const string GameContext = @"(?:game|boss|quest|mission|level|checkpoint|puzzle|objective|encounter)";

    internal static string Guide(string question, bool hasScreen = true)
    {
        string prompt = GuidePrefix + " Use the pointer, if visible, as a hint and consider the whole image. " +
            "Identify the game or task if possible and suggest a specific next step. Give advice in chat and leave the controls to me.";
        if (!hasScreen) prompt += MissingContext + " Ask me to enable PC screen sharing or point at a shared browser or Settings panel and try again.";
        return question.Length == 0 ? prompt : prompt + " My question: " + question;
    }

    internal static bool IsSpokenGuide(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"^\s*(?:(?:hey\s+)?v-?rex[\s,]+)?(?:please[\s,]+)?(?:can you\s+|could you\s+)?(?:help me|guide me|what should i do next|show me|look at the screen)(?:[\s,]+please)?[.!?\s]*$",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool IsGameGuidance(string text) =>
        text.StartsWith(GuidePrefix, StringComparison.OrdinalIgnoreCase) ||
        text.StartsWith("Guide me with this request:", StringComparison.OrdinalIgnoreCase) ||
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\b(?:research|look up|search (?:the web )?for|help (?:me )?with|advice (?:on|about)|keep an eye on|how (?:do|can|should) I|what (?:do|should) I do)\b.{0,65}\b" + GameContext +
            @"\b|\b(?:my|the) (?:first|next) move\b|\bwhat should I (?:play|move)\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool RequestsAnnotation(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\b(?:annotat\w*|draw\s+(?:an?\s+)?(?:arrow|circle|box|over)|(?:circle|box|highlight|underline)\s+(?:it|that|this|the)|mark\s+(?:where|(?:the|my)\s+(?:move|spot|target|item|route|location|enemy|object)))\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool RequestsPointing(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\b(?:point (?:to|at|out)|show me (?:where|.{1,60} on (?:the )?screen|.{0,45}\b(?:graph|chart|headline|icon|marker|button)))\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool TargetsUbuntu(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\b(?:Ubuntu (?:display|screen|desktop|window|panel)|(?:on|in) (?:the )?Ubuntu)\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool RequestsZoomInspection(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text, @"\b(?:zoom[\s-]+(?:in(?:to)?|out)|zoom\s*grid|magnify)\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool IsAnnotationRetry(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"^\s*(?:(?:and|okay|yes|please)\b[, ]+)*(?:(?:can|could|would)\s+you\s+|you\s+)?(?:try(?:\s+that)?\s+(?:again|one more time)|redo that)(?:\s+please)?[.?! ]*$",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool IsZoomFeedback(string text) =>
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\byou\s+(?:didn['’]t|did\s+not)\s+(?:use\s+(?:the\s+)?zoom\s*grid|zoom\s+in)\s*[.?! ]*$",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    internal static bool RequestsPcScreen(string text) =>
        IsGameGuidance(text) || (RequestsPointing(text) && !TargetsUbuntu(text)) ||
        (RequestsZoomInspection(text) && !TargetsUbuntu(text)) ||
        (RequestsAnnotation(text) && !TargetsUbuntu(text) &&
            System.Text.RegularExpressions.Regex.IsMatch(text, @"\b(?:screen|display|game|move|route|location|target|enemy|item|icon|headline|graph|chart|button)\b",
                System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant)) ||
        System.Text.RegularExpressions.Regex.IsMatch(text,
            @"\b(?:(?:my|main)\s+(?:PC\s+)?(?:screen|display|monitor)|PC\s+(?:screen|display|monitor)|what\s+(?:game\s+is\s+(?:this|on\s+(?:the\s+)?screen)|is\s+this\s+game|(?:game\s+)?am\s+I\s+playing|do\s+you\s+see)|(?:help|guide)\b.{0,32}\bthis\s+game)\b",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);
}

internal sealed record VrexRequest(string Type, string? Text = null, int X = 0, int Y = 0, int Key = 0,
    string? Model = null, string? Endpoint = null, string? Provider = null, string? ApiKey = null,
    int CpuCount = 2, int MemoryMb = 4096, string? VmPath = null, string? Resolution = null, bool SpeakSummary = false,
    string? PcImage = null, float Pan = 0, bool? PlayGuestAudio = null, float Rear = 0, string[]? PcTools = null,
    string? ScreenSurface = null, bool AllowUbuntuMarks = false, bool AllowBotClipboard = false,
    bool AllowBotFileCopy = false);

internal sealed record VrexTaskStep(string Label, string Status, string? Note = null);
internal sealed record VrexMarkPoint(double X, double Y);
internal sealed record VrexMark(string Shape, VrexMarkPoint[] Points, string? Label, string Colour);
internal sealed record VrexGridFrame(string Kind, byte[] Image);
internal sealed record VrexChatSummary(string Id, string Title, bool OnHold = false);
internal sealed record VrexChatLine(bool FromUser, string Text);
internal sealed record VrexGoal(string Id, string Title, string Status, int Done, int Total,
    string? Description = null, VrexTaskStep[]? Steps = null, string? ActiveStep = null);
internal sealed record VrexBooking(string Id, string Title, DateTime Start, int Minutes, string? Notes, string? Repeat);

internal static class VrexPcPermissions
{
    internal static readonly (string Id, string Label)[] Tools =
    [
        ("pc_begin", "Take control"), ("pc_end", "Release control"), ("pc_screens", "List displays"),
        ("pc_screenshot", "Take screenshots"), ("pc_grid", "Inspect with a numbered grid"),
        ("pc_zoom_out", "Zoom out of the grid"), ("pc_find_text", "Find on-screen text"),
        ("pc_peek", "Inspect a grid point"), ("pc_focus_window", "Focus a window"),
        ("pc_mark", "Annotate main screen"),
        ("pc_move", "Move the pointer"), ("pc_click", "Click"), ("pc_click_at", "Click an image position"),
        ("pc_drag", "Drag"), ("pc_scroll", "Scroll"), ("pc_type", "Type text"), ("pc_key", "Press keys"),
        ("pc_open_app", "Open a Windows app"), ("pc_list_files", "List workspace files"),
        ("pc_read_file", "Read workspace files"), ("pc_write_file", "Write workspace files")
    ];

    internal static bool IsWorkspaceTool(string id) => id is "pc_open_app" or "pc_list_files" or "pc_read_file" or "pc_write_file";
}

internal sealed record VrexEvent(string Type, string? Text = null, int X = 0, int Y = 0,
    string? Model = null, string? Endpoint = null, string? Provider = null, bool HasKey = false,
    int CpuCount = 2, int MemoryMb = 4096, string? VmPhase = null, string? VmPath = null, bool VmInstalled = false,
    string? Resolution = null, string[]? Choices = null, bool VoiceLive = false, bool VoiceInstalled = false,
    bool PlayGuestAudio = true, int Avatar = 0, uint ShellRgb = 0, string? ObservedSurface = null,
    string? Thinking = null, VrexTaskStep[]? Tasks = null, VrexMark[]? Marks = null, string? Verbosity = null,
    VrexMarkPoint? Point = null, VrexGridFrame? Grid = null,
    VrexChatSummary? Chat = null, VrexChatLine? ChatLine = null, VrexGoal? Goal = null, VrexBooking? Booking = null);
