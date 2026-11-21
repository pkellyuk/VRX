using System.Globalization;
using System.Windows;
using System.Windows.Media;
using VRX.VrexBridge;

namespace VRX.Desktop;

internal sealed class VrexMarkCanvas : FrameworkElement
{
    private readonly List<VrexMark> marks = [];
    internal bool HasMarks => marks.Count != 0;
    private Rect imageBounds = Rect.Empty;
    internal Rect ImageBounds
    {
        set
        {
            if (imageBounds == value) return;
            imageBounds = value;
            InvalidateVisual();
        }
    }

    internal VrexMarkCanvas() => IsHitTestVisible = false;

    internal void Update(VrexMark[] items, bool replace)
    {
        if (replace) marks.Clear();
        marks.AddRange(items);
        if (marks.Count > 40) marks.RemoveRange(0, marks.Count - 40);
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawing)
    {
        Rect bounds = imageBounds.IsEmpty ? new Rect(RenderSize) : imageBounds;
        if (bounds.Width <= 0 || bounds.Height <= 0) return;
        drawing.PushClip(new RectangleGeometry(bounds));
        double scale = Math.Clamp(bounds.Height / 600, 0.8, 2.5);
        Point Map(VrexMarkPoint p) => new(bounds.X + p.X * bounds.Width, bounds.Y + p.Y * bounds.Height);

        foreach (var mark in marks)
        {
            var points = mark.Points.Select(Map).ToArray();
            if (points.Length == 0) continue;
            var color = (Color)ColorConverter.ConvertFromString(mark.Colour);
            var brush = new SolidColorBrush(color);
            var stroke = new Pen(brush, 3 * scale) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
            var outline = new Pen(Brushes.Black, 5 * scale);
            Point first = points[0];
            void Line(Point a, Point b)
            {
                drawing.DrawLine(outline, a, b);
                drawing.DrawLine(stroke, a, b);
            }

            if (points.Length >= 2)
            {
                Point second = points[1];
                var box = new Rect(first, second);
                switch (mark.Shape)
                {
                    case "Box":
                        drawing.DrawRoundedRectangle(null, outline, box, 4, 4);
                        drawing.DrawRoundedRectangle(null, stroke, box, 4, 4);
                        break;
                    case "Circle":
                        var center = new Point(box.Left + box.Width / 2, box.Top + box.Height / 2);
                        drawing.DrawEllipse(null, outline, center, box.Width / 2, box.Height / 2);
                        drawing.DrawEllipse(null, stroke, center, box.Width / 2, box.Height / 2);
                        break;
                    case "Highlight":
                        drawing.DrawRectangle(new SolidColorBrush(Color.FromArgb(85, color.R, color.G, color.B)), stroke, box);
                        break;
                    case "Underline":
                        Line(box.BottomLeft, box.BottomRight);
                        break;
                    case "Arrow":
                        Line(first, second);
                        Vector fromTip = first - second;
                        if (fromTip.Length > 1)
                        {
                            fromTip.Normalize();
                            var tail = fromTip * (15 * scale);
                            var side = new Vector(-tail.Y, tail.X) * .55;
                            Line(second, second + tail + side);
                            Line(second, second + tail - side);
                        }
                        break;
                    case "Pen":
                        for (int i = 1; i < points.Length; i++) Line(points[i - 1], points[i]);
                        break;
                }
            }
            if (!string.IsNullOrWhiteSpace(mark.Label))
            {
                var text = new FormattedText(mark.Label, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                    new Typeface("Segoe UI Semibold"), 16 * scale, Brushes.White, VisualTreeHelper.GetDpi(this).PixelsPerDip)
                    { MaxTextWidth = Math.Max(1, bounds.Width - 14 * scale), MaxLineCount = 2, Trimming = TextTrimming.CharacterEllipsis };
                var at = new Point(Math.Clamp(first.X, bounds.Left, Math.Max(bounds.Left, bounds.Right - text.Width - 10 * scale)),
                    Math.Clamp(first.Y, bounds.Top, Math.Max(bounds.Top, bounds.Bottom - text.Height - 6 * scale)));
                drawing.DrawRoundedRectangle(Brushes.Black, new Pen(brush, 1.5 * scale),
                    new Rect(at, new Size(text.Width + 10 * scale, text.Height + 6 * scale)), 5 * scale, 5 * scale);
                drawing.DrawText(text, at + new Vector(5 * scale, 3 * scale));
            }
        }
        drawing.Pop();
    }
}
