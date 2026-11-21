using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace VRX.Desktop;

internal sealed class VrexVmIndicator : DockPanel
{
    private readonly Ellipse dot = new() { Width = 8, Height = 8, Margin = new(0, 0, 7, 0) };
    private readonly TextBlock label = new() { FontWeight = FontWeights.SemiBold };
    private readonly TextBlock detail = new() { TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };

    internal VrexVmIndicator()
    {
        LastChildFill = true;
        Margin = new Thickness(0, 3, 0, 6);
        var badge = new StackPanel { Orientation = Orientation.Horizontal };
        badge.Children.Add(dot);
        badge.Children.Add(label);
        var pill = new Border { Child = badge, CornerRadius = new CornerRadius(10), VerticalAlignment = VerticalAlignment.Center,
            Padding = new Thickness(9, 4, 9, 4), Margin = new Thickness(0, 0, 10, 0) };
        pill.SetResourceReference(Border.BackgroundProperty, "Raised");
        SetDock(pill, Dock.Left);
        Children.Add(pill);
        Children.Add(detail);
    }

    internal void Update(IVrexClient vm)
    {
        var (text, color) = vm.VmPhase switch
        {
            VrexVmPhase.Preparing => ("Preparing", Colors.Goldenrod),
            VrexVmPhase.Booting => ("Booting", Colors.Goldenrod),
            VrexVmPhase.Running => ("Running", Colors.MediumSeaGreen),
            VrexVmPhase.Stopping => ("Shutting down", Colors.Goldenrod),
            VrexVmPhase.Moving => ("Moving storage", Colors.Goldenrod),
            VrexVmPhase.DisplayUnavailable => ("Display disconnected", Colors.IndianRed),
            VrexVmPhase.Failed => ("Needs attention", Colors.IndianRed),
            _ => ("Powered off", Colors.SlateGray)
        };
        label.Text = text;
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        dot.Fill = brush;
        detail.Text = vm.VmDetail;
    }
}
