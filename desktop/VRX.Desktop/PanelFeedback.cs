using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace VRX.Desktop;

// VR pointers do not generate WPF mouse input. Give every projected control an
// explicit, short-lived pressed state rather than invoking its action invisibly.
internal static class PanelFeedback
{
    private sealed class State
    {
        internal double Opacity;
        internal bool Active;
        internal DispatcherTimer? Timer;
    }
    private static readonly ConditionalWeakTable<FrameworkElement, State> states = new();
    internal static void Down(FrameworkElement? control, Action invalidate)
    {
        if (control == null) return;
        var state = states.GetOrCreateValue(control);
        state.Timer?.Stop();
        if (!state.Active) { state.Opacity = control.Opacity; }
        state.Active = true;
        control.Opacity = state.Opacity * .55;
        invalidate();
    }
    internal static void Up(FrameworkElement? control, Action invalidate)
    {
        if (control == null) return;
        var state = states.GetOrCreateValue(control);
        if (!state.Active) Down(control, invalidate);
        state.Timer?.Stop();
        var timer = new DispatcherTimer(DispatcherPriority.Render, control.Dispatcher) { Interval = TimeSpan.FromMilliseconds(150) };
        state.Timer = timer;
        timer.Tick += (_, _) => { timer.Stop(); Cancel(control, invalidate); };
        timer.Start(); invalidate();
    }
    internal static async Task SmokeTest()
    {
        var button = new System.Windows.Controls.Button { Opacity = .8 };
        int rendered = 0; Down(button, () => rendered++);
        if (button.Opacity >= .8) throw new Exception("VR button press must visibly change its appearance");
        Up(button, () => rendered++); await Task.Delay(190);
        if (Math.Abs(button.Opacity - .8) > .001 || rendered < 3) throw new Exception("VR click feedback must restore after the release pulse");
        Down(button, () => rendered++); Cancel(button, () => rendered++);
        if (Math.Abs(button.Opacity - .8) > .001) throw new Exception("Lost pointer focus must clear the pressed appearance");
    }
    internal static void Cancel(FrameworkElement? control, Action invalidate)
    {
        if (control == null || !states.TryGetValue(control, out var state) || !state.Active) return;
        state.Timer?.Stop(); state.Timer = null;
        control.Opacity = state.Opacity; state.Active = false;
        invalidate();
    }
}
