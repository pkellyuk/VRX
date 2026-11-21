using Automaton.Core.Services.ComputerUse;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed class VrexGridPreview(Action<VrexEvent> emit)
{
    private readonly object gate = new();
    private string? surface;
    private GridView? view;
    private string kind = "";
    private VrexEvent? marker;
    private bool open;

    internal void SetOpen(bool value)
    {
        lock (gate)
        {
            open = value;
            if (!open || view == null) return;
            Publish();
            if (marker != null) emit(marker);
        }
    }

    internal void Show(string target, GridView grid, string description)
    {
        if (grid.ImageBytes.Length is 0 or > VrexProtocol.MaxPcImageBytes) return;
        lock (gate)
        {
            surface = target;
            view = grid;
            kind = description;
            marker = null;
            if (open) Publish();
        }
    }

    private void Publish() => emit(new("grid-view", surface, Grid: new(kind, view!.ImageBytes)));

    internal void Indicate(string target, double x, double y, bool clicked)
    {
        lock (gate)
        {
            if (target != surface || view == null) return;
            var area = view.Region;
            VrexMarkPoint? at = x >= area.X && x <= area.Right - 1 && y >= area.Y && y <= area.Bottom - 1
                ? new((x - area.X) / Math.Max(1, area.Width - 1), (y - area.Y) / Math.Max(1, area.Height - 1))
                : null;
            marker = new("grid-marker", target, X: clicked ? 1 : 0, Point: at);
            if (open) emit(marker);
        }
    }

    internal void Clear(string? target = null)
    {
        lock (gate)
        {
            if (target != null && target != surface) return;
            view = null;
            surface = null;
            marker = null;
            if (open) emit(new("grid-view", target));
        }
    }
}
