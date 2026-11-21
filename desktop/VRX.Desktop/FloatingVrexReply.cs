using System.Globalization;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace VRX.Desktop;

// A brief preview above the chat panel, with controller-clickable choices when needed.
internal sealed class FloatingVrexReply : IFloatingPanel
{
    private string text="";
    private long expires;
    private bool dirty=true;
    private int appearance=-1;
    private IReadOnlyList<(string Label, Action Run)> actions = [];
    private readonly List<Rect> actionBounds = [];
    private int heldAction = -1;
    private int hoveredAction = -1;
    public bool Closed=>Environment.TickCount64>expires;
    public bool Dirty=>dirty || appearance!=Appearance.Revision;
    internal bool Interactive => actions.Count > 0;
    internal bool WaitingForChoice => expires == long.MaxValue;
    internal void Show(string value, IReadOnlyList<(string Label, Action Run)>? choices = null, bool waitForChoice = false)
    {
        actions=choices ?? [];
        text = PreviewText(value, Interactive ? 650 : 180);
        heldAction=hoveredAction=-1;
        expires=waitForChoice ? long.MaxValue : Environment.TickCount64 + (Interactive ? 30000 : 6000);
        dirty=true;
    }
    private static string PreviewText(string value, int limit)
    {
        if (value.Length <= limit) return value;
        int end = value.LastIndexOfAny([' ', '\n', '\r', '\t'], limit - 1);
        if (end < limit / 2) end = char.IsHighSurrogate(value[limit - 2]) ? limit - 2 : limit - 1;
        return value[..end].TrimEnd() + "…";
    }
    internal void Dismiss() { expires=0; heldAction=-1; dirty=true; }
    public byte[] Render()
    {
        var visual=new DrawingVisual();
        actionBounds.Clear();
        using(var drawing=visual.RenderOpen())
        {
            var theme=Appearance.FloatingPalette;
            var accent = Appearance.Brush(theme.Accent);
            var caption = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                new Typeface("Segoe UI"), 56, Appearance.Brush(theme.Ink), 2)
                { MaxTextWidth = 1690, MaxTextHeight = Interactive ? 550 : 765, Trimming = TextTrimming.CharacterEllipsis, LineHeight = 73 };
            var label = new FormattedText("Vrex", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal),
                43, accent, 2);
            double width = Math.Clamp(caption.Width + 128, Interactive ? 1000 : 640, 1850);
            double height = 180 + caption.Height + actions.Count * 122;
            var card = new Rect((ControllerMenu.PixelWidth - width) / 2,
                ControllerMenu.PixelHeight - 112 - height, width, height);
            var fill = Appearance.Brush(theme.Card, Interactive ? .96 : .84);
            drawing.DrawRoundedRectangle(new SolidColorBrush(Color.FromArgb(70, 0, 0, 0)), null,
                new Rect(card.X + 12, card.Y + 18, card.Width, card.Height), 58, 58);
            drawing.DrawRoundedRectangle(fill, new Pen(Appearance.Brush(theme.Accent, .35), 3), card, 56, 56);
            drawing.DrawEllipse(accent, null, new Point(card.X + 70, card.Y + 74), 12, 12);
            drawing.DrawText(label, new Point(card.X + 98, card.Y + 44));
            drawing.DrawText(caption, new Point(card.X + 64, card.Y + 122));
            for (int i = 0; i < actions.Count; i++)
            {
                var bounds = new Rect(card.X + 64, card.Y + 141 + caption.Height + i * 122, card.Width - 128, 98);
                actionBounds.Add(bounds);
                var highlight = hoveredAction == i ? new Pen(accent, 3) : null;
                drawing.DrawRoundedRectangle(Appearance.Brush(heldAction == i ? theme.Accent :
                    hoveredAction == i ? theme.Card : theme.Page), highlight, bounds, 25, 25);
                var action = new FormattedText(actions[i].Label, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                    new Typeface("Segoe UI"), 44, Appearance.Brush(heldAction == i ? theme.Card : theme.Ink), 2)
                    { MaxTextWidth = bounds.Width - 44, MaxTextHeight = 64, Trimming = TextTrimming.CharacterEllipsis };
                drawing.DrawText(action, new Point(bounds.X + 22, bounds.Y + 18));
            }
        }
        var bitmap=new RenderTargetBitmap(ControllerMenu.PixelWidth,ControllerMenu.PixelHeight,96,96,PixelFormats.Pbgra32);
        bitmap.Render(visual);byte[] result=new byte[ControllerMenu.PixelBytes];bitmap.CopyPixels(result,ControllerMenu.PixelWidth*4,0);return result;
    }
    public void Published(){dirty=false;appearance=Appearance.Revision;}
    public void Pointer(int kind,float x,float y,float value=0)
    {
        if (!Interactive || Closed) return;
        var point = new Point(x * ControllerMenu.PixelWidth, y * ControllerMenu.PixelHeight);
        int hit = actionBounds.FindIndex(bounds => bounds.Contains(point));
        if (kind is 0 or 4 or PanelFeedback.Leave)
        {
            int hover = kind == 0 ? hit : -1;
            if (hoveredAction != hover) { hoveredAction = hover; dirty = true; }
        }
        if (kind == 1) { heldAction = hit; dirty = true; }
        if (kind is not (2 or 4)) return;
        int previous = heldAction;
        heldAction = -1;
        dirty = true;
        if (kind == 2 && hit >= 0 && hit == previous)
        {
            var run = actions[hit].Run;
            Dismiss();
            run();
        }
    }
    public void Dispose(){}

    internal static void SmokeChoices()
    {
        using var bubble = new FloatingVrexReply();
        bubble.Show(string.Concat(Enumerable.Repeat("A useful hint. ", 40)));
        if (bubble.text.Length > 180 || !bubble.text.EndsWith('…') || bubble.expires - Environment.TickCount64 > 6000)
            throw new InvalidOperationException("An ordinary reply should show a brief preview.");
        int selected = 0;
        bubble.Show("I found a clue. Would you like help?", [("Guide Me", () => selected++)], waitForChoice: true);
        if (!bubble.WaitingForChoice) throw new InvalidOperationException("A choice should stay open until answered or dismissed.");
        bubble.Render();
        var button = bubble.actionBounds[0];
        float x = (float)((button.X + button.Width / 2) / ControllerMenu.PixelWidth);
        float y = (float)((button.Y + button.Height / 2) / ControllerMenu.PixelHeight);
        bubble.Pointer(1, x, y);
        bubble.Pointer(4, -1, -1);
        bubble.Pointer(2, x, y);
        if (selected != 0) throw new InvalidOperationException("A cancelled Vrex suggestion was selected.");
        bubble.Pointer(1, x, y);
        bubble.Pointer(2, x, y);
        if (selected != 1 || !bubble.Closed) throw new InvalidOperationException("The Vrex suggestion was not selected and dismissed.");
    }
}
