using System.Runtime.InteropServices;
using Automaton.Core.Interfaces;
using Automaton.Core.Services.ComputerUse;
using Automaton.Tools.Tools;
using PersonaBot;
using SkiaSharp;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed class VrexDesktopTools : IDisposable
{
    internal const string InspectTool = "pc_inspect";
    private readonly object gate = new();
    private readonly HashSet<string> enabled = new(StringComparer.Ordinal);
    private readonly PrimaryDesktop desktop = new();
    private readonly ComputerUseSession session;
    private readonly Dictionary<string, ITool> tools;
    private CancellationTokenSource? active;
    private bool sharing;
    internal event Action? EmergencyStop;
    internal event Action<IReadOnlyList<ScreenMark>, bool, ScreenRect>? MarksChanged;
    internal event Action<GridView, string>? ViewShown;
    internal event Action<ScreenPoint>? ClickPerformed;

    internal VrexDesktopTools()
    {
        session = new(desktop, new GridOverlayRenderer(), desktop, desktopWindows: desktop);
        session.ViewShown += (view, kind) => ViewShown?.Invoke(view, kind);
        session.ClickPerformed += point => ClickPerformed?.Invoke(point);
        session.EmergencyStop += StopFromKeyboard;
        ITool[] actions = [new ComputerUseBeginTool(session), new ComputerUseEndTool(session),
            new ComputerUseScreensTool(session), new ComputerUseScreenshotTool(session), new ComputerUseGridTool(session),
            new ComputerUseZoomOutTool(session), new ComputerUseFindTextTool(session), new ComputerUsePeekTool(session),
            new ComputerUseFocusWindowTool(session), new ComputerUseMoveTool(session), new ComputerUseClickTool(session),
            new ComputerUseClickAtTool(session), new ComputerUseDragTool(session), new ComputerUseScrollTool(session),
            new ComputerUseTypeTool(session), new ComputerUseKeyTool(session)];
        tools = actions.ToDictionary(tool => "pc_" + tool.Id[3..], StringComparer.Ordinal);
        var mark = new MarkTool(desktop.GetVirtualDesktopBounds, (marks, replace) =>
        {
            lock (gate)
                if (CanUse("pc_mark") && active is { IsCancellationRequested: false })
                    MarksChanged?.Invoke(marks, replace, desktop.GetVirtualDesktopBounds());
        }, "main PC display", desktop.CaptureVirtualDesktop);
        mark.ReferenceShown += view => ViewShown?.Invoke(view, "annotation reference");
        tools.Add("pc_mark", mark);
        foreach (var tool in new VrexWorkspaceTools().Tools()) tools.Add(tool.Id, tool);
    }

    internal string[] Enabled { get { lock (gate) return enabled.ToArray(); } }
    internal void ResetMarkReference() => ((MarkTool)tools["pc_mark"]).ResetReference();
    internal bool HasZoomedMarkReference => ((MarkTool)tools["pc_mark"]).HasZoomedReference;

    internal void EnableAll()
    {
        lock (gate) enabled.UnionWith(tools.Keys);
    }

    internal void SetPermission(string? id, bool allow)
    {
        if (id == null || !tools.ContainsKey(id)) return;
        lock (gate)
        {
            if (allow) enabled.Add(id);
            else if (enabled.Remove(id)) ReleaseControl();
        }
    }

    internal void SetSharing(bool allow)
    {
        lock (gate)
        {
            sharing = allow;
            if (!allow) ReleaseControl();
        }
    }

    internal void RevokeAll()
    {
        lock (gate) { enabled.Clear(); ReleaseControl(); }
    }

    private void StopFromKeyboard()
    {
        RevokeAll();
        EmergencyStop?.Invoke();
    }

    internal void ReleaseControl()
    {
        lock (gate)
        {
            active?.Cancel();
            if (session.IsActive) session.End("Vrex released PC control");
        }
    }

    internal Dictionary<string, JsonSchema> Schemas()
    {
        lock (gate)
        {
            var schemas = enabled.Where(CanUse).ToDictionary(id => id, id =>
            {
                var source = tools[id];
                var schema = source.Parameters;
                if (VrexPcPermissions.IsWorkspaceTool(id)) return schema;
                if (id == "pc_begin") return new JsonSchema
                {
                    Description = "Take control of the primary Windows display for the user’s requested task.",
                    Properties = new() { ["reason"] = schema.Properties["reason"] }, Required = ["reason"]
                };
                return new JsonSchema { Description = source.Description.Replace("cu_", "pc_") + " This tool targets the primary Windows display.",
                    Properties = schema.Properties, Required = schema.Required, AdditionalProperties = schema.AdditionalProperties };
            }, StringComparer.Ordinal);
            if (sharing)
            {
                var properties = tools["pc_mark"].Parameters.Properties;
                schemas.Add(InspectTool, new JsonSchema
                {
                    Description = "Read-only view of the shared Windows display. Omit both labels for the full display, or supply two labels from the latest view to zoom in. Use the returned view to ground a point or annotation.",
                    Properties = new() { ["zoom_from"] = properties["zoom_from"], ["zoom_to"] = properties["zoom_to"] }
                });
            }
            return schemas;
        }
    }

    internal async Task<ToolResult> ExecuteAsync(string id, string arguments, CancellationToken token)
    {
        ITool tool;
        CancellationTokenSource permission;
        lock (gate)
        {
            if (id == InspectTool)
            {
                if (!sharing) return new ToolResult { Title = "PC screen sharing off", Output = "PC screen sharing must be on to inspect the Windows display." };
                var inspection = System.Text.Json.Nodes.JsonNode.Parse(arguments) as System.Text.Json.Nodes.JsonObject
                    ?? throw new ArgumentException("Provide a JSON object for screen inspection.");
                inspection["inspect"] = true;
                arguments = inspection.ToJsonString();
                tool = tools["pc_mark"];
            }
            else if (!CanUse(id) || !tools.TryGetValue(id, out tool!)) return Denied();
            if (DesktopInputService.IsChordDown(ComputerUseSession.KillChord)) { StopFromKeyboard(); return Denied(); }
            permission = CancellationTokenSource.CreateLinkedTokenSource(token);
            active = permission;
        }
        desktop.Access.Value = permission.Token;
        try
        {
            permission.Token.ThrowIfCancellationRequested();
            var result = await tool.ExecuteAsync(arguments, new ToolContext { SessionId = "vrex", CancellationToken = permission.Token });
            permission.Token.ThrowIfCancellationRequested();
            if (!VrexPcPermissions.IsWorkspaceTool(id)) result.Output = result.Output.Replace("cu_", "pc_");
            return result;
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested && permission.IsCancellationRequested) { return Denied(); }
        finally
        {
            desktop.Access.Value = default;
            lock (gate)
            {
                if (permission.IsCancellationRequested && session.IsActive) session.End("PC permission ended");
                active = null;
                permission.Dispose();
            }
        }
    }

    private bool CanUse(string id) => enabled.Contains(id) &&
        (VrexPcPermissions.IsWorkspaceTool(id) || sharing && (id == "pc_mark" || enabled.Contains("pc_begin")));

    private static ToolResult Denied() => new() { Title = "PC permission required",
        Output = "The user must enable this action in PC permissions. Screen tools also need PC screen sharing. Input and inspection tools need Take control." };

    public void Dispose() { session.EmergencyStop -= StopFromKeyboard; RevokeAll(); session.Dispose(); }

    private sealed class PrimaryDesktop : IComputerScreen, IComputerInput, IComputerWindows
    {
        private readonly DesktopCaptureService capture = new();
        private readonly DesktopWindowService windows = new();
        private readonly DesktopInputService input;
        internal readonly AsyncLocal<CancellationToken?> Access = new();

        internal PrimaryDesktop() => input = new(capture);

        private bool CanAct => Access.Value is { IsCancellationRequested: false } && !DesktopInputService.IsChordDown(ComputerUseSession.KillChord);
        private void CheckAccess()
        {
            Access.Value?.ThrowIfCancellationRequested();
            if (!CanAct) throw new InvalidOperationException("PC control is inactive or the stop shortcut is held.");
        }
        private MonitorInfo Primary() => capture.GetMonitors().FirstOrDefault(monitor => monitor.IsPrimary)
            ?? throw new InvalidOperationException("The primary Windows display is unavailable.");
        public ScreenRect GetVirtualDesktopBounds() => Primary().Bounds;
        public IReadOnlyList<MonitorInfo> GetMonitors() { var monitor = Primary(); monitor.Index = 1; return [monitor]; }
        public SKBitmap CaptureRegion(ScreenRect region)
        {
            CheckAccess();
            return ((IComputerScreen)capture).CaptureRegion(region.ClampInside(GetVirtualDesktopBounds()));
        }
        public SKBitmap CaptureVirtualDesktop() => CaptureRegion(GetVirtualDesktopBounds());

        private void CheckPoint(ScreenPoint point)
        {
            CheckAccess();
            var bounds = GetVirtualDesktopBounds();
            if (point.X < bounds.X || point.X >= bounds.Right || point.Y < bounds.Y || point.Y >= bounds.Bottom)
                throw new InvalidOperationException("PC input is limited to the primary Windows display.");
        }
        public ScreenPoint GetCursorPosition()
        {
            var bounds = GetVirtualDesktopBounds();
            var point = input.GetCursorPosition();
            return new(Math.Clamp(point.X, bounds.X, bounds.Right - 1), Math.Clamp(point.Y, bounds.Y, bounds.Bottom - 1));
        }
        public void MoveTo(ScreenPoint point) { CheckPoint(point); input.MoveTo(point); }
        public void Click(ScreenPoint point, MouseButton button, int count) { CheckPoint(point); input.Click(point, button, count); }
        public void Scroll(ScreenPoint point, int notches, bool horizontal) { CheckPoint(point); input.Scroll(point, notches, horizontal); }
        public void Drag(ScreenPoint from, ScreenPoint to, MouseButton button, PointerPath? path, TimeSpan? duration)
        {
            CheckPoint(from); CheckPoint(to);
            input.Drag(from, to, button, null, duration, Access.Value.GetValueOrDefault());
        }
        public void PressKeys(string keys) { CheckKeyboard(); input.PressKeys(keys); }
        public void TypeText(string text, Func<bool>? shouldStop)
        {
            CheckKeyboard();
            nint foreground = GetForegroundWindow();
            input.TypeText(text, () => !CanAct || GetForegroundWindow() != foreground || shouldStop?.Invoke() == true);
        }
        private void CheckKeyboard()
        {
            CheckAccess();
            if (!IsOnPrimary(GetForegroundWindow())) throw new InvalidOperationException("Focus a window on the primary display before using the keyboard.");
        }

        public WindowSnapshot Capture()
        {
            CheckAccess();
            var snapshot = windows.Capture();
            var visible = snapshot.Windows.Where(window => IsOnPrimary(window.Handle)).ToArray();
            var foreground = visible.FirstOrDefault(window => window.Handle == snapshot.ForegroundHandle);
            return new WindowSnapshot { Windows = visible, Foreground = foreground, ForegroundHandle = foreground?.Handle ?? 0 };
        }
        public (nint Handle, uint ProcessId) TopLevelWindowAt(ScreenPoint point) { CheckPoint(point); return windows.TopLevelWindowAt(point); }
        public bool BringToFront(nint handle) { CheckAccess(); return IsOnPrimary(handle) && windows.BringToFront(handle); }

        private static bool IsOnPrimary(nint window)
        {
            nint monitor = MonitorFromWindow(window, 0);
            return monitor != 0 && monitor == MonitorFromPoint(0, 1);
        }
        [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
        [DllImport("user32.dll")] private static extern nint MonitorFromWindow(nint window, uint flags);
        [DllImport("user32.dll")] private static extern nint MonitorFromPoint(long point, uint flags);
    }
}
