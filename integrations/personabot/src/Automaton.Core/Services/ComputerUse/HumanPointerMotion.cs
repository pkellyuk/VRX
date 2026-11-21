namespace Automaton.Core.Services.ComputerUse;

/// <summary>
/// How a hand moves a mouse, reduced to the few properties that make a pointer look
/// driven rather than teleported: travel time that grows with distance, a gentle arc
/// instead of a ruler-straight line, a bell-shaped speed profile, and on long moves a
/// small overshoot that is then corrected.
///
/// Pure maths with no platform dependency, and in Core rather than the UI, because two
/// parties must agree on it: the session waits <see cref="TravelTime"/> before injecting
/// a click, and the ghost cursor animates along <see cref="Plan"/> over that same time.
/// If the two computed it separately, the real click would land while the ghost was
/// still on its way.
/// </summary>
public static class HumanPointerMotion
{
    /// <summary>Fastest a move is allowed to take — anything quicker reads as a jump.</summary>
    public static readonly TimeSpan MinimumTravel = TimeSpan.FromMilliseconds(180);

    /// <summary>
    /// Slowest. A 4K diagonal under Fitts's law alone would take well over a second,
    /// which stops looking deliberate and starts looking hesitant.
    /// </summary>
    public static readonly TimeSpan MaximumTravel = TimeSpan.FromMilliseconds(900);

    /// <summary>Moves shorter than this go straight to the target with no overshoot.</summary>
    internal const double OvershootThreshold = 150;

    /// <summary>Share of the travel time spent on the main stroke when there is an overshoot.</summary>
    internal const double MainStrokeShare = 0.82;

    /// <summary>
    /// Fitts's law: time grows with the log of distance over target size. The constants
    /// are tuned by eye so a short hop takes about a quarter of a second and a
    /// screen-width move about three quarters.
    /// </summary>
    public static TimeSpan TravelTime(ScreenPoint from, ScreenPoint to)
    {
        var distance = Distance(from, to);
        if (distance < 1)
        {
            return TimeSpan.Zero;
        }

        const double interceptMs = 140;
        const double slopeMs = 95;
        const double targetWidth = 40;

        var ms = interceptMs + slopeMs * Math.Log2(1 + distance / targetWidth);
        ms = Math.Clamp(ms, MinimumTravel.TotalMilliseconds, MaximumTravel.TotalMilliseconds);

        return TimeSpan.FromMilliseconds(ms);
    }

    /// <summary>
    /// Plans one move. The same inputs always give the same path, so a caller that
    /// passes the seed along gets an identical path on the other side.
    /// </summary>
    /// <param name="overshoot">
    /// False for a drag. A real drag follows this path with the button held, and
    /// sweeping past the drop target before settling would light up whatever sits
    /// just beyond it.
    /// </param>
    public static PointerPath Plan(ScreenPoint from, ScreenPoint to, int seed, bool overshoot = true)
    {
        return new PointerPath(from, to, seed, overshoot);
    }

    /// <summary>
    /// Minimum-jerk profile: zero velocity and acceleration at both ends, peak speed in
    /// the middle. This is the shape measured hand reaches actually follow, and it is
    /// what separates a hand from an ease-in-out curve that merely looks smooth.
    /// </summary>
    public static double MinimumJerk(double t)
    {
        t = Math.Clamp(t, 0, 1);
        return t * t * t * (10 - 15 * t + 6 * t * t);
    }

    internal static double Distance(ScreenPoint a, ScreenPoint b)
    {
        var dx = (double)b.X - a.X;
        var dy = (double)b.Y - a.Y;
        return Math.Sqrt(dx * dx + dy * dy);
    }
}

/// <summary>
/// One planned pointer move, sampled by normalised time.
/// </summary>
public sealed class PointerPath
{
    private readonly double _fromX;
    private readonly double _fromY;
    private readonly double _toX;
    private readonly double _toY;

    // Main stroke: a cubic Bezier from the start to the overshoot point.
    private readonly double _c1X;
    private readonly double _c1Y;
    private readonly double _c2X;
    private readonly double _c2Y;
    private readonly double _landX;
    private readonly double _landY;

    private readonly bool _hasOvershoot;

    internal PointerPath(ScreenPoint from, ScreenPoint to, int seed, bool overshoot)
    {
        From = from;
        To = to;

        _fromX = from.X;
        _fromY = from.Y;
        _toX = to.X;
        _toY = to.Y;

        var distance = HumanPointerMotion.Distance(from, to);
        var random = new Random(seed);

        if (distance < 1)
        {
            _c1X = _c2X = _landX = _toX;
            _c1Y = _c2Y = _landY = _toY;
            return;
        }

        var dirX = (_toX - _fromX) / distance;
        var dirY = (_toY - _fromY) / distance;

        // Perpendicular, for bowing the path sideways.
        var perpX = -dirY;
        var perpY = dirX;

        // A wrist pivots, so a reach bows slightly to one side. Both control points
        // bow the same way — opposite signs would make an S, which no hand does on a
        // single reach — and the second bows less so the path straightens on approach.
        var bow = (random.NextDouble() * 0.16 - 0.08) * distance;

        _hasOvershoot = overshoot && distance >= HumanPointerMotion.OvershootThreshold;

        if (_hasOvershoot)
        {
            // Overshoot a few percent along the direction of travel, never so far that
            // it looks like a miss, with a touch of sideways error.
            var along = Math.Min(distance * (0.025 + random.NextDouble() * 0.025), 36);
            var aside = (random.NextDouble() * 2 - 1) * Math.Min(distance * 0.01, 8);

            _landX = _toX + dirX * along + perpX * aside;
            _landY = _toY + dirY * along + perpY * aside;
        }
        else
        {
            _landX = _toX;
            _landY = _toY;
        }

        _c1X = _fromX + (_landX - _fromX) / 3 + perpX * bow;
        _c1Y = _fromY + (_landY - _fromY) / 3 + perpY * bow;
        _c2X = _fromX + (_landX - _fromX) * 2 / 3 + perpX * bow * 0.45;
        _c2Y = _fromY + (_landY - _fromY) * 2 / 3 + perpY * bow * 0.45;
    }

    public ScreenPoint From { get; }

    public ScreenPoint To { get; }

    /// <summary>
    /// Where the pointer is at normalised time <paramref name="t"/> (0 at the start,
    /// 1 at arrival). Exactly <see cref="From"/> at 0 and exactly <see cref="To"/> at 1.
    /// </summary>
    public (double X, double Y) Sample(double t)
    {
        t = Math.Clamp(t, 0, 1);

        if (t >= 1)
        {
            return (_toX, _toY);
        }

        if (!_hasOvershoot)
        {
            return Bezier(HumanPointerMotion.MinimumJerk(t));
        }

        const double mainShare = HumanPointerMotion.MainStrokeShare;

        if (t < mainShare)
        {
            return Bezier(HumanPointerMotion.MinimumJerk(t / mainShare));
        }

        // The correction: a short, separate submovement back onto the target, which
        // is what the eye picks up as "it went a bit past and settled".
        var s = HumanPointerMotion.MinimumJerk((t - mainShare) / (1 - mainShare));
        return (_landX + (_toX - _landX) * s, _landY + (_toY - _landY) * s);
    }

    private (double X, double Y) Bezier(double s)
    {
        var u = 1 - s;
        var a = u * u * u;
        var b = 3 * u * u * s;
        var c = 3 * u * s * s;
        var d = s * s * s;

        return (
            a * _fromX + b * _c1X + c * _c2X + d * _landX,
            a * _fromY + b * _c1Y + c * _c2Y + d * _landY);
    }
}
