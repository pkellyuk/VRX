using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed partial class FloatingVrexChatPanel : IFloatingPanel, IFloatingTextTarget
{
    private readonly IVrexClient client;
    private readonly Action<IFloatingTextTarget> focus;
    private readonly Action openKeyboard;
    private readonly Func<(VrexScreenCapture.Frame Frame, string Surface)?>? captureGuidePanel;
    private readonly UserControl surface;
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth, ControllerMenu.PixelHeight, 192, 192, PixelFormats.Pbgra32);
    private readonly byte[] pixels = new byte[ControllerMenu.PixelBytes];
    private readonly Dictionary<ButtonBase, Action> actions = [];
    private readonly PanelFeedback.Hover hover = new();
    private readonly TextBox input = new() { FontSize = 22, MinHeight = 48, VerticalContentAlignment = VerticalAlignment.Center };
    private readonly TextBox liveInput = new() { FontSize = 22, MinHeight = 48, VerticalContentAlignment = VerticalAlignment.Center,
        IsReadOnly = true, IsHitTestVisible = false, Visibility = Visibility.Collapsed, TextWrapping = TextWrapping.Wrap };
    private readonly ScrollViewer transcript = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden };
    private readonly StackPanel messages = new() { Margin = new(12) };
    private readonly StackPanel chatContent = new();
    private readonly StackPanel goalsPage = new() { Margin = new(12) };
    private readonly StackPanel goalRows = new();
    private readonly StackPanel goalDetails = new();
    private readonly TextBlock goalStatus = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap, Margin = new(4) };
    private readonly TextBlock goalActivity = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap, Margin = new(4), Visibility = Visibility.Collapsed };
    private readonly List<VrexGoal> goals = [];
    private readonly List<Button> goalButtons = [];
    private Button? achieveGoal;
    private Button? goalBanner;
    private readonly TextBlock goalBannerTitle = new() { FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock goalBannerActivity = new() { FontSize = 16, TextTrimming = TextTrimming.CharacterEllipsis };
    private VrexGoal? selectedGoal;
    private string? deletingGoalId;
    private bool achieving;
    private VrexGoal? activeGoal;
    private TextBlock? activeStepSpinner;
    private string currentGoalActivity = "";
    private int goalRequestNumber;
    private long goalActivitySince, nextGoalAnimation;
    private readonly Dictionary<CheckBox, string> pcChoices = [];
    private readonly TextBlock heading = new() { Text = "Vrex chat", FontSize = 26, VerticalAlignment = VerticalAlignment.Center };
    private readonly Button stop;
    private readonly Border activityTrack = new() { Height = 5, Margin = new(0, 0, 0, 4),
        CornerRadius = new(2.5), ClipToBounds = true, IsHitTestVisible = false, Visibility = Visibility.Hidden };
    private readonly Border activitySegment = new() { CornerRadius = new(2.5) };
    private readonly TranslateTransform activityPosition = new();
    private long nextActivityFrame;
    private bool activityVisible;
    private bool ChatIsActive => client.State is "responding" or "working" or "speaking";
    private readonly StackPanel footer = new() { Margin = new(0, 8, 0, 0) };
    private readonly StackPanel taskSteps = new();
    private readonly Border taskCard = new() { Visibility = Visibility.Collapsed, Margin = new(12), Padding = new(10), CornerRadius = new(8) };
    private static readonly (string Key, string Label)[] ThinkingLevels =
        [("default", "Default"), ("none", "Off"), ("low", "Low"), ("high", "High"), ("max", "Max")];
    private readonly List<VrexChatLine> fixtureHistory = [];
    private readonly StackPanel chatList = new();
    private readonly ScrollViewer chatListScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Hidden };
    private readonly DockPanel chatSidebar = new() { Width = 226, Margin = new(0, 4, 8, 0) };
    private readonly List<Button> chatRows = [];
    private Button? newChat, deleteChat, clearChat, cancelChatAction;
    private readonly TextBlock chatConfirmation = new() { TextWrapping = TextWrapping.Wrap, FontSize = 18, Margin = new(4) };
    private string? confirmedAction;
    private bool showChatList = true;
    private readonly TextBlock status = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar micLevel = new() { Height = 5, Maximum = 100, Visibility = Visibility.Collapsed };
    private ButtonBase? held;
    private Button? mic;
    private bool micActive;
    private Button? mode, voiceOutput, pcScreen, panelScreen, thinking;
    private Button? verbosity;
    private CheckBox? ubuntuMarks;
    private CheckBox? botClipboard, botFileCopy;
    private bool sending, dirty = true;
    private int stopRevision;
    private ScrollViewer? draggingScroll;
    private bool draggingTranscript => ReferenceEquals(draggingScroll, transcript);
    private double dragStartY, dragStartOffset;
    private bool followTranscript;
    private readonly VrexModelSettings? modelSettings;
    private readonly StackPanel? toolbar;
    private bool ShowingChat => ReferenceEquals(transcript.Content, chatContent);
    internal bool IsPasswordActive => ReferenceEquals(transcript.Content, modelSettings) && modelSettings?.IsPasswordActive == true;
    private double chatOffset;
    private int appearance = -1;
    public bool Closed { get; private set; }
    public bool Dirty => dirty || appearance != Appearance.Revision ||
        ChatIsActive != activityVisible || (activityVisible && Environment.TickCount64 >= nextActivityFrame) ||
        (achieving && (ShowingChat || ReferenceEquals(transcript.Content, goalsPage)) &&
            Environment.TickCount64 >= nextGoalAnimation);

    internal FloatingVrexChatPanel(IVrexClient client, Action<IFloatingTextTarget> focus, Action openKeyboard,
        Func<(VrexScreenCapture.Frame Frame, string Surface)?>? captureGuidePanel = null, Action<string>? openBrowser = null,
        Func<bool>? openGrid = null)
    {
        this.client = client;
        this.focus = focus;
        this.openKeyboard = openKeyboard;
        this.captureGuidePanel = captureGuidePanel;
        var root = new DockPanel { Margin = new(48) };
        var title = new Border { Height = 56, Child = FloatingPanelChrome.Title(heading,
            Button(Loc.Get("SnapshotClose"), Dispose)) };
        DockPanel.SetDock(title, Dock.Top);
        root.Children.Add(title);
        var activityCanvas = new Canvas();
        activityCanvas.Children.Add(activitySegment);
        activityTrack.Child = activityCanvas;
        activityTrack.SetResourceReference(Border.BackgroundProperty, "Well");
        activitySegment.SetResourceReference(Border.BackgroundProperty, "Accent");
        activitySegment.Height = activityTrack.Height;
        activitySegment.RenderTransform = activityPosition;
        DockPanel.SetDock(activityTrack, Dock.Top);
        root.Children.Add(activityTrack);
        footer.Children.Add(status);
        footer.Children.Add(micLevel);
        System.Windows.Automation.AutomationProperties.SetName(micLevel, "Microphone level");
        var row = new DockPanel();
        footer.Children.Add(row);
        DockPanel.SetDock(footer, Dock.Bottom);
        root.Children.Add(footer);
        AddRight(Button("Send", () => _ = SendAsync(false)));
        stop = Button("Stop", () =>
        {
            stopRevision++;
            if (client is VrexClient live) live.StopChat();
            else client.Send(new("chat-stop"));
        });
        stop.ToolTip = "Stop Vrex’s current task and voice";
        AddRight(stop);
        var clear = Button("Clear draft", () => { input.Clear(); dirty = true; });
        clear.ToolTip = "Clear the unsent message";
        System.Windows.Automation.AutomationProperties.SetName(clear, "Clear chat draft");
        AddRight(clear);
        if (client is VrexClient voice)
        {
            mic = Button("🎙", voice.ToggleLiveVoice);
            System.Windows.Automation.AutomationProperties.SetName(mic, "Live voice");
            AddRight(mic);
            var guide = Button("Guide Me", RequestGuide);
            guide.Width = guide.Height = 86;
            guide.Padding = new(0);
            guide.FontWeight = FontWeights.SemiBold;
            guide.SetResourceReference(Control.BackgroundProperty, "Accent");
            guide.SetResourceReference(Control.ForegroundProperty, "OnPanelAccent");
            guide.Template = (ControlTemplate)System.Windows.Markup.XamlReader.Parse("""
                <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Button">
                  <Grid>
                    <Ellipse Fill="{TemplateBinding Background}" Stroke="{TemplateBinding BorderBrush}" StrokeThickness="2"/>
                    <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                  </Grid>
                </ControlTemplate>
                """);
            guide.ToolTip = "Get advice using the screen you’ve shared (Ctrl+Alt+G)";
            AddRight(guide);
            modelSettings = new VrexModelSettings(voice, openBrowser, (button, action) => actions.Add(button, action),
                () => ShowPage(chatContent, "Vrex chat")) { Margin = new(12) };
            toolbar = new StackPanel { Margin = new(0, 4, 0, 5) };
            var options = new WrapPanel();
            options.Children.Add(Button("Chats", () =>
            {
                showChatList = !showChatList;
                chatSidebar.Visibility = showChatList ? Visibility.Visible : Visibility.Collapsed;
            }));
            options.Children.Add(Button("Goals", () => OpenGoals()));
            options.Children.Add(Button("Calendar", () =>
            {
                ShowPage(calendarPage, "Vrex calendar");
                LoadCalendar();
            }));
            if (openGrid != null)
                options.Children.Add(Button("Zoom grid", () =>
                {
                    if (!openGrid()) status.Text = "Close another floating panel to open the zoom grid.";
                }));
            var clearMarks = Button("Clear annotations", voice.ClearAnnotations);
            clearMarks.ToolTip = "Clear marks on the PC and Ubuntu screens";
            options.Children.Add(clearMarks);
            mode = Button("", voice.ToggleHelpful);
            voiceOutput = Button("", () => { voice.SpeakReplies = !voice.SpeakReplies; UpdateVoice(); dirty = true; });
            pcScreen = Button("", voice.TogglePcScreen);
            pcScreen.ToolTip = "Share the main PC display with your configured model.";
            panelScreen = Button("", voice.TogglePanelScreen);
            panelScreen.ToolTip = "In Helpful mode, share the browser or settings panel you point at with your configured model.";
            thinking = Button("", () =>
            {
                int current = Array.FindIndex(ThinkingLevels, level => level.Key == voice.Configuration.Thinking);
                voice.Send(new("thinking", ThinkingLevels[(current + 1) % ThinkingLevels.Length].Key));
            });
            thinking.ToolTip = "Cycle thinking effort for Vrex’s model";
            foreach (var option in new[] { mode, voiceOutput, pcScreen, panelScreen, thinking }) options.Children.Add(option);
            options.Children.Add(Button("Settings", () => ShowPage(modelSettings, "Vrex AI settings")));
            toolbar.Children.Add(options);
            // Keep the choice list in the rendered panel so VR pointer hit testing can see it.
            var verbosityChoices = new WrapPanel { Visibility = Visibility.Collapsed };
            verbosity = Button("", () => verbosityChoices.Visibility =
                verbosityChoices.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
            System.Windows.Automation.AutomationProperties.SetName(verbosity, "Reply verbosity");
            options.Children.Add(verbosity);
            foreach (var (key, label) in new[] { ("terse", "Terse"), ("normal", "Normal"), ("verbose", "Verbose") })
                verbosityChoices.Children.Add(Button(label, () =>
                {
                    voice.Send(new("verbosity", key));
                    verbosityChoices.Visibility = Visibility.Collapsed;
                }));
            toolbar.Children.Add(verbosityChoices);
            DockPanel.SetDock(toolbar, Dock.Top);
            root.Children.Add(toolbar);
            modelSettings.Children.Add(BuildPcPermissions(voice));
            BuildChatSidebar(voice);
            BuildCalendarPage();
            UpdateChatList();
            UpdateTasks();
        }
        BuildGoalBanner(root);
        BuildGoalsPage();
        if (client is VrexClient)
        {
            DockPanel.SetDock(chatSidebar, Dock.Left);
            root.Children.Add(chatSidebar);
        }
        var composer = new Grid();
        composer.Children.Add(input);
        composer.Children.Add(liveInput);
        row.Children.Add(composer);
        System.Windows.Automation.AutomationProperties.SetName(liveInput, "Live voice transcript");
        taskCard.Child = taskSteps;
        chatContent.Children.Add(messages);
        chatContent.Children.Add(taskCard);
        transcript.Content = chatContent;
        root.Children.Add(FloatingPanelChrome.Scrollable(transcript));
        surface = new UserControl { Width = ControllerMenu.Width, Height = ControllerMenu.Height,
            Content = FloatingPanelChrome.Wrap(root), FontFamily = new FontFamily("Segoe UI") };
        surface.Resources["LiveMicBackground"] = Appearance.Brush("#AB2235");
        surface.Resources["LiveMicForeground"] = Brushes.White;
        surface.Resources["AchieveGoalBackground"] = Appearance.Brush("#236A42");
        surface.Resources["AchieveGoalForeground"] = Brushes.White;
        client.Changed += Changed;
        UpdateTranscript();
        UpdateVoice();
        if (client is VrexClient activeClient)
        {
            try { activeClient.SetChatOpen(true, captureGuidePanel); UpdateVoice(); }
            catch (Exception ex) when (ex is System.IO.IOException or System.ComponentModel.Win32Exception)
            { status.Text = ex.Message; }
        }

        void AddRight(Button button)
        { DockPanel.SetDock(button, Dock.Right); row.Children.Add(button); }
    }

    private Button Button(string label, Action action)
    {
        var button = new Button { Content = label, FontSize = 19, MinHeight = 48, Padding = new(12, 6, 12, 6), Margin = new(4) };
        actions.Add(button, action);
        return button;
    }

    private void BuildChatSidebar(VrexClient voice)
    {
        newChat = Button("＋ New chat", () => voice.ChangeChat("chat-new"));
        DockPanel.SetDock(newChat, Dock.Top);
        chatSidebar.Children.Add(newChat);
        var controls = new StackPanel();
        DockPanel.SetDock(controls, Dock.Bottom);
        controls.Children.Add(chatConfirmation);
        clearChat = Button("Clear chat", () => ConfirmChatAction("chat-clear"));
        deleteChat = Button("Delete chat", () => ConfirmChatAction("chat-delete"));
        cancelChatAction = Button("Cancel", () => { confirmedAction = null; UpdateChatList(); });
        controls.Children.Add(clearChat);
        controls.Children.Add(deleteChat);
        controls.Children.Add(cancelChatAction);
        chatSidebar.Children.Add(controls);
        chatListScroll.Content = chatList;
        chatSidebar.Children.Add(FloatingPanelChrome.Scrollable(chatListScroll));

        void ConfirmChatAction(string action)
        {
            if (voice.ActiveChatId is not { } id || voice.ChatChanging) return;
            if (confirmedAction == action)
            {
                confirmedAction = null;
                voice.ChangeChat(action, id);
            }
            else
            {
                confirmedAction = action;
                UpdateChatList();
            }
        }
    }

    private bool SpeakGoalReplies => client is VrexClient { SpeakReplies: true };

    private void BuildGoalBanner(DockPanel root)
    {
        goalBanner = Button("", () => OpenGoals(VisibleGoal?.Id));
        goalBanner.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        goalBanner.Visibility = Visibility.Collapsed;
        goalBanner.Effect = new System.Windows.Media.Effects.DropShadowEffect
        {
            Color = Color.FromRgb(97, 224, 147), BlurRadius = 18, ShadowDepth = 0, Opacity = .65
        };
        goalBanner.Content = new StackPanel { Children = { goalBannerTitle, goalBannerActivity } };
        HighlightGoal(goalBanner);
        DockPanel.SetDock(goalBanner, Dock.Top);
        root.Children.Add(goalBanner);
    }

    private void BuildGoalsPage()
    {
        var controls = new WrapPanel();
        controls.Children.Add(Button("Back to chat", () => ShowPage(chatContent, "Vrex chat")));
        achieveGoal = Button("Achieve goal", () =>
        {
            if (client.Send(new("goal-run", Key: achieving ? 0 : 1, SpeakSummary: SpeakGoalReplies)))
                goalStatus.Text = achieving ? "Stopping goal work…" : "Starting goal work…";
        });
        achieveGoal.ToolTip = "Work continuously through active goals";
        controls.Children.Add(achieveGoal);
        goalsPage.Children.Add(controls);
        goalsPage.Children.Add(goalStatus);
        goalsPage.Children.Add(goalActivity);
        goalsPage.Children.Add(goalRows);
        goalsPage.Children.Add(goalDetails);
    }

    private VrexGoal? VisibleGoal => activeGoal ?? goals.FirstOrDefault(goal => goal.Status == "Active");

    private void OpenGoals(string? goalId = null)
    {
        ShowPage(goalsPage, "Vrex goals");
        if (goalId != null) client.Send(new("goal-detail", goalId));
        client.Send(new("goals"));
    }

    private static void HighlightGoal(Button button)
    {
        button.SetResourceReference(Control.BackgroundProperty, "AchieveGoalBackground");
        button.SetResourceReference(Control.ForegroundProperty, "AchieveGoalForeground");
    }

    private void UpdateGoalBanner()
    {
        if (goalBanner == null) return;
        var goal = VisibleGoal;
        goalBanner.Visibility = ShowingChat && (goal != null || achieving) ? Visibility.Visible : Visibility.Collapsed;
        if (goalBanner.Visibility != Visibility.Visible) return;
        goalBannerTitle.Text = goal?.Title ?? "Active goals";
        goalBannerActivity.Text = achieving ? "Working · " + GoalActivityText() : "Active goal · ready to continue";
    }

    private void DrawGoals()
    {
        UpdateGoalBanner();
        foreach (var button in goalButtons) actions.Remove(button);
        goalButtons.Clear();
        goalRows.Children.Clear();
        goalDetails.Children.Clear();
        activeStepSpinner = null;
        if (goals.Count == 0)
        {
            goalRows.Children.Add(new TextBlock { Text = "No goals yet. Ask Vrex in chat to set one.",
                TextWrapping = TextWrapping.Wrap, FontSize = 20, Margin = new(4, 8, 4, 4) });
            return;
        }
        foreach (var goal in goals)
        {
            bool running = achieving && activeGoal?.Id == goal.Id;
            string state = running ? "Working" : goal.Status == "Backlog" ? "Paused" : goal.Status;
            var row = Button($"{goal.Title}  ·  {state}  ·  {goal.Done}/{goal.Total} steps", () =>
                client.Send(new("goal-detail", goal.Id)));
            if (VisibleGoal?.Id == goal.Id) HighlightGoal(row);
            row.HorizontalContentAlignment = HorizontalAlignment.Left;
            goalButtons.Add(row);
            goalRows.Children.Add(row);
        }
        if (selectedGoal is not { } selected || !goals.Any(goal => goal.Id == selected.Id)) return;
        goalDetails.Children.Add(new TextBlock { Text = selected.Title, FontSize = 24, Margin = new(4, 12, 4, 4), FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        goalDetails.Children.Add(new TextBlock { Text = selected.Description, FontSize = 20, Margin = new(4), TextWrapping = TextWrapping.Wrap });
        foreach (var step in selected.Steps ?? [])
        {
            var line = new TextBlock { Text = $"{step.Status}  ·  {step.Label}", FontSize = 19, TextWrapping = TextWrapping.Wrap, Margin = new(4) };
            if (achieving && selected.Id == activeGoal?.Id && selected.ActiveStep is { } reference &&
                step.Label.StartsWith(reference + "  ", StringComparison.Ordinal))
            {
                var row = new DockPanel();
                activeStepSpinner = new TextBlock { Width = 30, Margin = new(4), FontSize = 23, Text = "◐",
                    Foreground = Appearance.Brush(Appearance.FloatingPalette.Accent) };
                DockPanel.SetDock(activeStepSpinner, Dock.Left);
                row.Children.Add(activeStepSpinner);
                row.Children.Add(line);
                goalDetails.Children.Add(row);
            }
            else goalDetails.Children.Add(line);
            if (!string.IsNullOrWhiteSpace(step.Note))
                goalDetails.Children.Add(new TextBlock
                {
                    Text = step.Note, FontSize = 18, Margin = new(18, 0, 4, 8), TextWrapping = TextWrapping.Wrap
                });
        }
        if (selected.Status == "Awaiting review")
        {
            var accept = Button("Accept completed work", () => client.Send(new("goal-accept", selected.Id)));
            goalButtons.Add(accept);
            goalDetails.Children.Add(accept);
        }
        else if (selected.Status is "Active" or "Blocked" or "Backlog")
        {
            if (selected.Status == "Active")
            {
                var pauseGoal = Button("Pause goal", () => client.Send(new("goal-pause", selected.Id)));
                goalButtons.Add(pauseGoal);
                goalDetails.Children.Add(pauseGoal);
            }
            if (!achieving)
            {
                var resumeGoal = Button("Resume goal", () => client.Send(new("goal-resume", selected.Id,
                    SpeakSummary: SpeakGoalReplies)));
                goalButtons.Add(resumeGoal);
                goalDetails.Children.Add(resumeGoal);
            }
        }
        if (deletingGoalId == selected.Id)
        {
            goalDetails.Children.Add(new TextBlock
            {
                Text = $"Permanently delete “{selected.Title}” and its steps?",
                FontSize = 19, TextWrapping = TextWrapping.Wrap, Margin = new(4, 12, 4, 4)
            });
            var controls = new WrapPanel();
            var confirm = Button("Confirm delete", () =>
            {
                if (!client.Send(new("goal-delete", selected.Id, Key: 1))) return;
                deletingGoalId = null;
                DrawGoals();
            });
            var cancel = Button("Keep goal", () => { deletingGoalId = null; DrawGoals(); });
            goalButtons.Add(confirm);
            goalButtons.Add(cancel);
            controls.Children.Add(confirm);
            controls.Children.Add(cancel);
            goalDetails.Children.Add(controls);
        }
        else
        {
            var delete = Button("Delete goal", () => { deletingGoalId = selected.Id; DrawGoals(); });
            goalButtons.Add(delete);
            goalDetails.Children.Add(delete);
        }
    }

    private void UpdateGoalRun(VrexEvent message)
    {
        if (achieveGoal == null) return;
        bool active = message.X == 1;
        if (active != achieving)
        {
            hover.Set(null, () => dirty = true);
            achieving = active;
            if (active)
            {
                achieveGoal.SetResourceReference(Control.BackgroundProperty, "AchieveGoalBackground");
                achieveGoal.SetResourceReference(Control.ForegroundProperty, "AchieveGoalForeground");
            }
            else
            {
                achieveGoal.ClearValue(Control.BackgroundProperty);
                achieveGoal.ClearValue(Control.ForegroundProperty);
                activeGoal = null;
                currentGoalActivity = "";
            }
            goalActivity.Visibility = active ? Visibility.Visible : Visibility.Collapsed;
            DrawGoals();
        }
        achieveGoal.ToolTip = active ? "Stop continuous goal work" : "Work continuously through active goals";
        goalStatus.Text = message.Text;
        UpdateGoalBanner();
    }

    private void AnimateGoalProgress()
    {
        if (!achieving || (!ShowingChat && !ReferenceEquals(transcript.Content, goalsPage))) return;
        long now = Environment.TickCount64;
        nextGoalAnimation = now + 180;
        string[] frames = ["◐", "◓", "◑", "◒"];
        string frame = frames[(int)(now / 180 % frames.Length)];
        if (activeStepSpinner != null) activeStepSpinner.Text = frame;
        string title = activeGoal?.Title ?? "Active goals";
        goalActivity.Text = $"{(activeStepSpinner == null ? frame + "  " : "")}{title} — {GoalActivityText()}";
        UpdateGoalBanner();
    }

    private string GoalActivityText()
    {
        string activity = currentGoalActivity.Length == 0 ? "Preparing the next step" : currentGoalActivity;
        if (goalRequestNumber > 0)
            activity += $" · Request {goalRequestNumber} · {(Environment.TickCount64 - goalActivitySince) / 1000}s";
        return activity;
    }

    private void UpdateChatList()
    {
        if (client is not VrexClient voice || newChat == null) return;
        foreach (var button in chatRows) actions.Remove(button);
        chatRows.Clear();
        chatList.Children.Clear();
        foreach (var chat in voice.Chats)
        {
            bool selected = chat.Id == voice.ActiveChatId;
            var row = Button("", () =>
            {
                confirmedAction = null;
                voice.ChangeChat("chat-switch", chat.Id);
            });
            string label = chat.OnHold ? "On hold · " + chat.Title : chat.Title;
            row.Content = new TextBlock { Text = label, TextTrimming = TextTrimming.CharacterEllipsis };
            row.ToolTip = label;
            row.HorizontalContentAlignment = HorizontalAlignment.Left;
            row.IsEnabled = !voice.ChatChanging;
            if (selected)
            {
                row.SetResourceReference(Control.BackgroundProperty, "Accent");
                row.SetResourceReference(Control.ForegroundProperty, "OnPanelAccent");
            }
            chatRows.Add(row);
            chatList.Children.Add(row);
        }
        bool hasChat = voice.ActiveChatId != null && !voice.ChatChanging;
        newChat.IsEnabled = !voice.ChatChanging;
        clearChat!.IsEnabled = deleteChat!.IsEnabled = hasChat;
        clearChat.Content = confirmedAction == "chat-clear" ? "Confirm clear" : "Clear chat";
        deleteChat.Content = confirmedAction == "chat-delete" ? "Confirm delete" : "Delete chat";
        chatConfirmation.Text = confirmedAction == "chat-clear" ? "Clear the messages and memory from this chat?" :
            confirmedAction == "chat-delete" ? "Delete this chat and its memory?" : "";
        chatConfirmation.Visibility = cancelChatAction!.Visibility = confirmedAction == null ? Visibility.Collapsed : Visibility.Visible;
    }

    private StackPanel BuildPcPermissions(VrexClient voice)
    {
        var permissions = new StackPanel { Margin = new(0, 22, 0, 0) };
        permissions.Children.Add(new TextBlock { Text = "Ubuntu permissions", FontSize = 22, FontWeight = FontWeights.SemiBold });
        var annotate = new CheckBox { Content = "Annotate Ubuntu", FontSize = 19, MinHeight = 48, Margin = new(4) };
        ubuntuMarks = annotate;
        actions.Add(annotate, () => voice.SetUbuntuMarks(annotate.IsChecked != true));
        permissions.Children.Add(annotate);
        permissions.Children.Add(new TextBlock { Text = "Clipboard and file permissions", FontSize = 22, FontWeight = FontWeights.SemiBold, Margin = new(0, 12, 0, 0) });
        botClipboard = new CheckBox { Content = "Let Vrex read and write clipboard text", FontSize = 19, MinHeight = 48, Margin = new(4) };
        botFileCopy = new CheckBox { Content = "Let Vrex copy selected files between Windows and Ubuntu", FontSize = 19, MinHeight = 48, Margin = new(4) };
        actions.Add(botClipboard, () => voice.SetBotTransferPermissions(botClipboard.IsChecked != true, voice.AllowBotFileCopy));
        actions.Add(botFileCopy, () => voice.SetBotTransferPermissions(voice.AllowBotClipboard, botFileCopy.IsChecked != true));
        permissions.Children.Add(botClipboard);
        permissions.Children.Add(botFileCopy);
        permissions.Children.Add(new TextBlock { Text = "Vrex can share clipboard text with your configured model. File copies use the files selected in Explorer or Ubuntu’s file manager.",
            FontSize = 18, TextWrapping = TextWrapping.Wrap, Margin = new(0, 0, 0, 8) });
        permissions.Children.Add(new TextBlock { Text = "PC tool permissions", FontSize = 22, FontWeight = FontWeights.SemiBold });
        permissions.Children.Add(new TextBlock
        {
            Text = "Annotations need PC screen sharing. Other screen tools also need Take control. File tools use the Vrex workspace and share file contents with your model. " +
                "Permissions are saved and active while chat is open. Disable all or Ctrl+Alt+Shift+F12 stops Windows input.", FontSize = 18, TextWrapping = TextWrapping.Wrap,
            Margin = new(0, 0, 0, 8)
        });
        var controls = new WrapPanel();
        controls.Children.Add(Button("Enable all", () => voice.SetAllPcTools(true)));
        controls.Children.Add(Button("Disable all", () => voice.SetAllPcTools(false)));
        permissions.Children.Add(controls);
        var list = new UniformGrid { Columns = 2 };
        foreach (var (id, label) in VrexPcPermissions.Tools)
        {
            var choice = new CheckBox { Content = label, FontSize = 19, MinHeight = 48, Margin = new(4), VerticalContentAlignment = VerticalAlignment.Center };
            pcChoices.Add(choice, id);
            actions.Add(choice, () => voice.SetPcTool(id, choice.IsChecked != true));
            list.Children.Add(choice);
        }
        permissions.Children.Add(list);
        return permissions;
    }

    private void ShowPage(StackPanel page, string title)
    {
        if (ShowingChat) chatOffset = transcript.VerticalOffset;
        bool leavingModelSettings = ReferenceEquals(transcript.Content, modelSettings);
        modelSettings?.ClearSelection();
        followTranscript = false;
        transcript.Content = page;
        heading.Text = title;
        footer.Visibility = ShowingChat ? Visibility.Visible : Visibility.Collapsed;
        if (toolbar != null) toolbar.Visibility = footer.Visibility;
        chatSidebar.Visibility = ShowingChat && showChatList ? Visibility.Visible : Visibility.Collapsed;
        transcript.ScrollToVerticalOffset(ShowingChat ? chatOffset : 0);
        UpdateGoalBanner();
        if (leavingModelSettings) focus(this);
        dirty = true;
    }

    private void UpdateVoice()
    {
        if (client is not VrexClient voice || mic == null) return;
        if (micActive != voice.LiveVoiceActive)
        {
            hover.Set(null, () => dirty = true);
            micActive = voice.LiveVoiceActive;
            if (micActive)
            {
                mic.SetResourceReference(Control.BackgroundProperty, "LiveMicBackground");
                mic.SetResourceReference(Control.ForegroundProperty, "LiveMicForeground");
            }
            else
            {
                mic.ClearValue(Control.BackgroundProperty);
                mic.ClearValue(Control.ForegroundProperty);
            }
        }
        mic.ToolTip = voice.LiveVoiceActive ? "Stop live voice" : "Start live voice with barge-in";
        mode!.Content = voice.Helpful ? "Mode: Helpful" : "Mode: Quiet";
        voiceOutput!.Content = voice.SpeakReplies ? "Voice: On" : "Voice: Off";
        pcScreen!.Content = voice.AllowPcScreen ? "PC screen: On" : "PC screen: Off";
        panelScreen!.Content = voice.AllowPanelScreen ? "Panels: On" : "Panels: Off";
        thinking!.Content = "Thinking: " + (ThinkingLevels.FirstOrDefault(level => level.Key == voice.Configuration.Thinking).Label ?? "Low");
        thinking.IsEnabled = voice.ConfigurationLoaded;
        verbosity!.Content = $"Verbosity: {voice.Configuration.Verbosity switch { "normal" => "Normal", "verbose" => "Verbose", _ => "Terse" }} ▾";
        verbosity.IsEnabled = voice.ConfigurationLoaded;
        foreach (var (choice, id) in pcChoices) choice.IsChecked = voice.PcTools.Contains(id);
        if (ubuntuMarks != null) ubuntuMarks.IsChecked = voice.AllowUbuntuMarks;
        if (botClipboard != null) botClipboard.IsChecked = voice.AllowBotClipboard;
        if (botFileCopy != null) botFileCopy.IsChecked = voice.AllowBotFileCopy;
        status.Text = voice.State == "error" ? voice.Status : !string.IsNullOrWhiteSpace(voice.VoiceStatus) ? voice.VoiceStatus :
            voice.CanConverse ? "" : voice.ConfigurationLoaded ? "Choose a model in Vrex chat Settings." : "Connecting to Vrex…";
        micLevel.Visibility = voice.LiveVoiceActive ? Visibility.Visible : Visibility.Collapsed;
        if (!voice.LiveVoiceActive) ShowLiveInput(null);
        micLevel.Value = 0;
    }

    private void ShowLiveInput(string? words)
    {
        liveInput.Text = words ?? "";
        bool visible = liveInput.Text.Length > 0;
        input.Visibility = visible ? Visibility.Hidden : Visibility.Visible;
        liveInput.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (visible) liveInput.ScrollToEnd();
    }

    private void Changed(VrexEvent message)
    {
        if (message.Type is "reply" or "suggestion" or "voice-heard" or "chat-written")
        {
            if (message.Type == "voice-heard") ShowLiveInput(null);
            if (client is not VrexClient && message.Text != null)
                fixtureHistory.Add(new(message.Type is "chat-written" or "voice-heard", message.Text));
            if (message.Type == "reply") UpdateVoice();
            UpdateTranscript();
        }
        else if (message.Type == "chat-list-ready")
        {
            UpdateChatList();
            if (message.X == 1) { followTranscript = true; UpdateTranscript(); UpdateVoice(); }
        }
        else if (message.Type == "chat-list-pending") UpdateChatList();
        else if (message.Type == "chat-list-error") { UpdateChatList(); status.Text = message.Text; }
        else if (message.Type == "chat-changed") { ShowLiveInput(null); input.Clear(); confirmedAction = null; }
        else if (message.Type == "voice-partial") ShowLiveInput(message.Text);
        else if (message.Type == "goals-start") goals.Clear();
        else if (message is { Type: "goal-item", Goal: { } goal }) goals.Add(goal);
        else if (message.Type == "goals-ready")
        {
            DrawGoals();
            if (selectedGoal is { } selected && goals.Any(goal => goal.Id == selected.Id))
                client.Send(new("goal-detail", selected.Id));
        }
        else if (message.Type == "goals-changed")
        {
            client.Send(new("goals"));
        }
        else if (message.Type == "goal-detail")
        {
            selectedGoal = message.Goal;
            if (selectedGoal?.Id != deletingGoalId) deletingGoalId = null;
            DrawGoals();
        }
        else if (message.Type == "goal-deleted")
        {
            goals.RemoveAll(goal => goal.Id == message.Text);
            if (selectedGoal?.Id == message.Text) selectedGoal = null;
            if (deletingGoalId == message.Text) deletingGoalId = null;
            DrawGoals();
        }
        else if (message.Type == "goal-current")
        {
            activeGoal = selectedGoal = message.Goal;
            currentGoalActivity = "";
            goalRequestNumber = 0;
            DrawGoals();
        }
        else if (message.Type == "goal-activity")
        {
            currentGoalActivity = message.Text ?? "";
            goalRequestNumber = message.X;
            goalActivitySince = Environment.TickCount64;
            UpdateGoalBanner();
        }
        else if (message.Type == "goal-run") UpdateGoalRun(message);
        else if (message.Type == "goal-status") goalStatus.Text = message.Text;
        else if (HandleCalendarEvent(message)) { }
        else if (message.Type == "voice-meter") micLevel.Value = message.X;
        else if (message.Type is "task-list" or "state") UpdateTasks();
        else if (message.Type is "voice-state" or "ready" or "settings" or "pc-mode" or "pc-permissions" or "stopped" or "error") UpdateVoice();
        else if (message.Type is not ("model-saved" or "model-settings-error")) return;
        dirty = true;
    }

    internal void VerifyGoalBanner(Action<VrexEvent> emit)
    {
        var first = new VrexGoal("first", "Find the exit", "Active", 0, 2);
        var running = new VrexGoal("running", "Beat the boss", "Active", 1, 3);
        emit(new("goals-start"));
        emit(new("goal-item", Goal: first));
        emit(new("goal-item", Goal: running));
        emit(new("goals-ready"));
        emit(new("goal-run", X: 1));
        emit(new("goal-current", Goal: running));
        emit(new("goal-activity", "Checking the guest screen"));
        Render();
        if (goalBanner?.Visibility != Visibility.Visible || goalBannerTitle.Text != running.Title ||
            !goalBannerActivity.Text.Contains("Checking the guest screen"))
            throw new Exception("Chat goal banner did not show the active goal and its live activity.");
        var centre = goalBanner.TranslatePoint(new Point(goalBanner.ActualWidth / 2, goalBanner.ActualHeight / 2), surface);
        float u = (float)(centre.X / ControllerMenu.Width), v = (float)(centre.Y / ControllerMenu.Height);
        Pointer(1, u, v);
        Pointer(2, u, v);
        if (!ReferenceEquals(transcript.Content, goalsPage) || goalBanner.Visibility != Visibility.Collapsed ||
            goalRows.Children.OfType<Button>().Last().Background != goalBanner.Background)
            throw new Exception("The chat banner did not open Goals or highlight the matching row.");
    }

    private void UpdateTasks()
    {
        if (client is not VrexClient voice) return;
        followTranscript |= ShowingChat && !draggingTranscript && transcript.ScrollableHeight - transcript.VerticalOffset < 2;
        taskSteps.Children.Clear();
        var palette = Appearance.FloatingPalette;
        taskCard.Background = Appearance.Brush(palette.Card);
        taskCard.Visibility = voice.TaskSteps.Count > 0 || voice.State == "responding" ? Visibility.Visible : Visibility.Collapsed;
        if (taskCard.Visibility == Visibility.Collapsed) return;
        taskSteps.Children.Add(new TextBlock { Text = voice.TaskSteps.Count == 0 ? "Preparing a reply…" : "Tasks",
            FontSize = 19, FontWeight = FontWeights.SemiBold, Foreground = Appearance.Brush(palette.Ink) });
        foreach (var step in voice.TaskSteps)
        {
            string mark = step.Status switch { "completed" => "✓", "in_progress" => "◉", _ => "○" };
            taskSteps.Children.Add(new TextBlock { Text = $"{mark}  {step.Label}", TextWrapping = TextWrapping.Wrap,
                FontSize = 18, Margin = new(0, 4, 0, 0), Foreground = Appearance.Brush(step.Status == "in_progress" ? palette.Accent : palette.Ink) });
        }
    }

    private void UpdateTranscript()
    {
        double offset = transcript.VerticalOffset;
        followTranscript = ShowingChat && !draggingTranscript && (followTranscript || transcript.ScrollableHeight - offset < 2);
        var palette = Appearance.FloatingPalette;
        messages.Children.Clear();
        foreach (var line in client is VrexClient voice ? voice.ChatHistory : fixtureHistory)
        {
            var text = new StackPanel();
            text.Children.Add(new TextBlock { Text = line.FromUser ? "You" : "Vrex", FontSize = 23,
                FontWeight = FontWeights.SemiBold, Foreground = Appearance.Brush(line.FromUser ? palette.Ink : palette.Accent) });
            text.Children.Add(new TextBlock { Text = line.Text, FontSize = 23, TextWrapping = TextWrapping.Wrap,
                Foreground = Appearance.Brush(palette.Ink) });
            messages.Children.Add(new Border { Margin = new(0, 0, 0, 12), Padding = new(10), Child = text,
                Background = Appearance.Brush(line.FromUser ? palette.Card : palette.Page) });
        }
    }

    internal void RequestGuide() => _ = SendAsync(true);

    private async Task SendAsync(bool guide)
    {
        if (liveInput.Visibility == Visibility.Visible) return;
        string draft = input.Text.Trim();
        if (sending || (!guide && draft.Length == 0)) return;
        if (client is VrexClient { CanConverse: false })
        { status.Text = "Choose a model in Vrex chat Settings before chatting."; dirty = true; return; }
        sending = true;
        int revision = stopRevision;
        try
        {
            if (guide && client is VrexClient guideClient)
            {
                status.Text = "Looking at the shared screen…";
                dirty = true;
                string? issue = await guideClient.GuideAsync(draft, captureGuidePanel);
                if (revision != stopRevision || Closed) return;
                if (issue != null) { status.Text = issue; return; }
                if (input.Text.Trim() == draft) input.Clear();
                return;
            }
            bool sent = client is VrexClient voice
                ? await voice.SendChatAsync(draft)
                : client.Send(new("chat", draft));
            if (revision != stopRevision) return;
            if (!sent) { status.Text = "Vrex is busy. Try again in a moment."; return; }
            if (client is not VrexClient) Changed(new("chat-written", draft));
            if (input.Text.Trim() == draft) input.Clear();
        }
        catch (Exception ex) when (ex is System.IO.IOException or InvalidOperationException)
        { status.Text = ex.Message; }
        finally { sending = false; dirty = true; }
    }

    public void Pointer(int kind, float x, float y, float value = 0)
    {
        if (Closed) return;
        var point = new Point(x * ControllerMenu.Width, y * ControllerMenu.Height);
        DependencyObject? hit = kind is 4 or PanelFeedback.Leave ? null : VisualTreeHelper.HitTest(surface, point)?.VisualHit;
        ButtonBase? button = null;
        for (var node = hit; node != null; node = VisualTreeHelper.GetParent(node))
            if (node is ButtonBase { IsEnabled: true } candidate && actions.ContainsKey(candidate)) { button = candidate; break; }
        if (kind is 0 or 4 or PanelFeedback.Leave) hover.Set(button, () => dirty = true);
        if (kind == 1)
        {
            held = button;
            PanelFeedback.Down(held, () => dirty = true);
            if (button == null && modelSettings != null && ReferenceEquals(transcript.Content, modelSettings) && modelSettings.SelectField(hit))
            {
                focus(this);
                openKeyboard();
                dirty = true;
                return;
            }
            var scroll = ScrollAt(point);
            if (button == null && scroll != null)
            {
                draggingScroll = scroll;
                dragStartY = point.Y;
                dragStartOffset = scroll.VerticalOffset;
                return;
            }
            if (ShowingChat)
            {
                var local = surface.TranslatePoint(point, input);
                if (!new Rect(input.RenderSize).Contains(local)) return;
                focus(this);
                int index = input.GetCharacterIndexFromPoint(local, true);
                if (index >= 0) input.Select(index, 0);
                openKeyboard();
                dirty = true;
            }
        }
        else if (kind is 2 or 4)
        {
            draggingScroll = null;
            var pressed = held;
            held = null;
            if (kind == 2 && pressed != null && ReferenceEquals(pressed, button))
            { PanelFeedback.Up(pressed, () => dirty = true); actions[pressed](); }
            else PanelFeedback.Cancel(pressed, () => dirty = true);
        }
        else if (kind == 0 && draggingScroll is { } dragging)
        {
            dragging.ScrollToVerticalOffset(dragStartOffset + dragStartY - point.Y);
            dirty = true;
        }
        else if (kind == 3)
        {
            var scroll = ScrollAt(point);
            if (scroll == null) return;
            scroll.ScrollToVerticalOffset(scroll.VerticalOffset - value);
            dirty = true;
        }
    }

    private ScrollViewer? ScrollAt(Point point)
    {
        if (chatSidebar.IsArrangeValid && chatSidebar.Visibility == Visibility.Visible &&
            chatListScroll.ActualWidth > 0 && chatListScroll.ActualHeight > 0 &&
            new Rect(chatListScroll.RenderSize).Contains(surface.TranslatePoint(point, chatListScroll)))
            return chatListScroll;
        return new Rect(transcript.RenderSize).Contains(surface.TranslatePoint(point, transcript)) ? transcript : null;
    }

    public void Edit(KeyboardEdit edit)
    {
        if (!ShowingChat)
        {
            if (modelSettings != null && ReferenceEquals(transcript.Content, modelSettings))
            {
                modelSettings.Edit(edit);
                dirty = true;
            }
            return;
        }
        if (liveInput.Visibility == Visibility.Visible) return;
        if (edit.Key == 13) { _ = SendAsync(false); return; }
        if (edit.Key == 65 && edit.Modifiers.HasFlag(KeyboardModifiers.Control)) { input.SelectAll(); dirty = true; return; }
        if (edit.Key is 37 or 39)
        { input.Select(Math.Clamp(input.CaretIndex + (edit.Key == 37 ? -1 : 1), 0, input.Text.Length), 0); dirty = true; return; }
        int start = input.SelectionStart, length = input.SelectionLength;
        int remove = edit.Backspaces + (edit.Key == 8 ? 1 : 0);
        if (length == 0 && remove > 0) { remove = Math.Min(start, remove); start -= remove; length = remove; }
        if (length == 0 && edit.Key == 46 && start < input.Text.Length) length = 1;
        if (length > 0 || edit.Text.Length > 0) { input.Select(start, length); input.SelectedText = edit.Text; input.Select(start + edit.Text.Length, 0); }
        dirty = true;
    }

    public byte[] Render()
    {
        AnimateGoalProgress();
        if (ChatIsActive != activityVisible)
        {
            activityVisible = ChatIsActive;
            activityTrack.Visibility = activityVisible ? Visibility.Visible : Visibility.Hidden;
            if (activityVisible)
            {
                stop.SetResourceReference(Control.BackgroundProperty, "Accent");
                stop.SetResourceReference(Control.ForegroundProperty, "OnPanelAccent");
            }
            else
            {
                stop.ClearValue(Control.BackgroundProperty);
                stop.ClearValue(Control.ForegroundProperty);
            }
        }
        if (appearance != Appearance.Revision) { UpdateTranscript(); UpdateTasks(); }
        var size = new Size(ControllerMenu.Width, ControllerMenu.Height);
        surface.Measure(size); surface.Arrange(new Rect(size)); surface.UpdateLayout();
        if (activityVisible)
        {
            long now = Environment.TickCount64;
            nextActivityFrame = now + 50;
            double trackWidth = activityTrack.ActualWidth;
            activitySegment.Width = trackWidth * .22;
            double phase = now % 1400 / 1400.0;
            activityPosition.X = (trackWidth + activitySegment.Width) * phase - activitySegment.Width;
            surface.UpdateLayout();
        }
        if (followTranscript)
        {
            followTranscript = false;
            transcript.ScrollToVerticalOffset(transcript.ScrollableHeight);
            surface.UpdateLayout();
        }
        bitmap.Clear(); bitmap.Render(surface); bitmap.CopyPixels(pixels, ControllerMenu.PixelWidth * 4, 0);
        return pixels;
    }

    public void Published() { dirty = false; appearance = Appearance.Revision; }

    internal void VerifyTranscriptDrag()
    {
        Render();
        Published();
        double initialOffset = transcript.VerticalOffset;
        var position = transcript.TranslatePoint(new Point(64, 120), surface);
        float x = (float)(position.X / ControllerMenu.Width);
        float y = (float)(position.Y / ControllerMenu.Height);
        float down = y + 120f / ControllerMenu.Height;
        Pointer(1, x, y);
        Pointer(0, x, down);
        Pointer(2, x, down);
        Render();
        if (transcript.VerticalOffset >= initialOffset - 20)
            throw new InvalidOperationException($"Dragging chat text down should reveal earlier replies (offset {initialOffset} → {transcript.VerticalOffset}, range {transcript.ScrollableHeight}).");
    }

    internal void VerifyStopClick()
    {
        Render();
        var point = stop.TranslatePoint(new Point(stop.ActualWidth / 2, stop.ActualHeight / 2), surface);
        float x = (float)(point.X / ControllerMenu.Width), y = (float)(point.Y / ControllerMenu.Height);
        Pointer(1, x, y);
        Pointer(2, x, y);
    }

    internal void VerifyActivityIndicator(Action<string> setState)
    {
        foreach (string state in new[] { "responding", "working", "speaking", "paused", "idle" })
        {
            setState(state);
            Render();
            bool active = state is "responding" or "working" or "speaking";
            if ((activityTrack.Visibility == Visibility.Visible) != active ||
                (stop.ReadLocalValue(Control.BackgroundProperty) != DependencyProperty.UnsetValue) != active)
                throw new InvalidOperationException($"Vrex activity and Stop highlight disagree in {state} state.");
        }
    }

    internal void VerifyLiveTranscript(Action<VrexEvent> emit)
    {
        input.Text = "Keep my draft";
        emit(new("voice-partial", "A spoken preview"));
        if (liveInput.Visibility != Visibility.Visible || liveInput.Text != "A spoken preview" || input.Visibility != Visibility.Hidden)
            throw new InvalidOperationException("Interim recognition must appear in the chat composer.");
        emit(new("voice-partial", "A revised preview"));
        if (liveInput.Text != "A revised preview")
            throw new InvalidOperationException("Interim recognition must replace the previous preview.");
        emit(new("voice-heard", "A final utterance"));
        if (liveInput.Visibility != Visibility.Collapsed || input.Text != "Keep my draft" || input.Visibility != Visibility.Visible)
            throw new InvalidOperationException("A final voice message must uncover the original typed draft.");
        input.Clear();
    }

    public void Dispose()
    {
        if (Closed) return;
        Closed = true;
        client.Changed -= Changed;
        modelSettings?.Dispose();
        if (client is VrexClient voice) voice.SetChatOpen(false);
    }
}
