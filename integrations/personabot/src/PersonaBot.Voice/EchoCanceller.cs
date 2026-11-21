using System.Numerics;
using System.Runtime.InteropServices;

namespace PersonaBot.Voice;

/// <summary>
/// Acoustic echo cancellation, so the microphone can listen while the bot speaks (barge-in) without hearing the bot.
/// It knows exactly what's playing (<see cref="AddReference"/>, stamped with when it reaches the speaker) and what
/// the microphone recorded (<see cref="Process"/>, stamped with when it was recorded), both on one clock and both at
/// <see cref="Rate"/>. From those it finds how much later the bot's voice reaches the microphone (the delay, by
/// GCC-PHAT cross-correlation, refined every half second while the bot talks), then an adaptive filter (NLMS,
/// <see cref="Taps"/> long, so 64 ms of the room's echoes after the delay) learns the path from speaker to microphone
/// and takes the bot's voice out, leaving the person's (the residual). With headphones there's no echo to take out
/// and the residual is just the microphone. The filter and the delay are kept between utterances, so it starts
/// already tuned to the room after the first sentence.
/// </summary>
public sealed class EchoCanceller
{
    /// <summary>The sample rate of both streams.</summary>
    public const int Rate = 16000;

    /// <summary>The filter's length: how much of the room's echo it covers after the delay (64 ms).</summary>
    public const int Taps = 1024;

    /// <summary>How far before the found delay the filter starts (10 ms), for timing jitter.</summary>
    public const int Lead = 160;

    /// <summary>The longest delay looked for (400 ms) and the most the reference may lag behind the microphone (100 ms).</summary>
    public const int MaxDelay = 6400, MaxAdvance = 1600;

    private const int Capacity = Rate * 6;
    private const int FindWindow = 8192;
    private const int FindSize = 16384;
    private const float Step = 0.5f;

    private readonly float[] _reference = new float[Capacity * 2];
    private readonly float[] _weights = new float[Taps];
    private readonly float[] _findMic = new float[FindWindow];
    private long _base;
    private int _count;
    private double? _origin;
    private double _micPosition = double.NaN;
    private int _findFill;
    private long _findStart;
    private int _pendingDelay = -1_000_000;

    /// <summary>The delay (samples) from the reference's timestamps to its echo in the microphone.</summary>
    public int Delay { get; private set; }

    /// <summary>Whether the delay has been found from the sound itself (rather than assumed from the timestamps).</summary>
    public bool Aligned { get; private set; }

    /// <summary>How many times the delay has been found or changed (for the log).</summary>
    public int DelayUpdates { get; private set; }

    /// <summary>How many standard deviations a delay's correlation peak must stand above the rest to be believed.</summary>
    public const double Confidence = 9;

    /// <summary>How clearly the last delay search found a peak (standard deviations above the rest), for the log.</summary>
    public double LastConfidence { get; private set; }

    /// <summary>The power of the reference (what's playing) lined up with the last processed block, its echo tail included.</summary>
    public float ReferencePower { get; private set; }

    /// <summary>
    /// Forgets the timeline (not what it learnt of the room): a new utterance's streams start afresh, but the
    /// filter and delay carry on.
    /// </summary>
    public void Restart()
    {
        _base = 0;
        _count = 0;
        _origin = null;
        _micPosition = double.NaN;
        _findFill = 0;
        _pendingDelay = -1_000_000;
        ReferencePower = 0;
    }

    /// <summary>
    /// What's playing: <paramref name="samples"/> at <see cref="Rate"/>, the first reaching the speaker at
    /// <paramref name="time"/> (seconds, the shared clock). Blocks follow on from each other; a gap is silence.
    /// </summary>
    public void AddReference(ReadOnlySpan<float> samples, double time)
    {
        if (samples.Length == 0) return;
        if (_origin == null)
        {
            _origin = time;
            _base = 0;
            _count = 0;
        }
        var at = (long)Math.Round((time - _origin.Value) * Rate);
        var end = _base + _count;
        if (at > end + Lead)
        {
            // A gap (nothing was playing): silence, up to what the buffer holds.
            var gap = (int)Math.Min(at - end, Capacity);
            Append(new float[gap]);
        }
        else if (at < end - Rate / 2)
        {
            // Far behind where the timeline expects: the clocks have jumped; carry on as if continuous.
            _origin += (end - at) / (double)Rate;
        }
        // Small differences are timing jitter: the blocks are treated as continuous.
        Append(samples);
    }

    private void Append(ReadOnlySpan<float> samples)
    {
        if (samples.Length >= Capacity) samples = samples[^(Capacity - 1)..];
        if (_count + samples.Length > _reference.Length)
        {
            // Keep the newest half: the filter only looks a little way back.
            var keep = Math.Min(_count, Capacity);
            Array.Copy(_reference, _count - keep, _reference, 0, keep);
            _base += _count - keep;
            _count = keep;
        }
        samples.CopyTo(_reference.AsSpan(_count));
        _count += samples.Length;
    }

    private float ReferenceAt(long index)
    {
        var i = index - _base;
        return i >= 0 && i < _count ? _reference[i] : 0f;
    }

    /// <summary>
    /// Takes the bot's voice out of <paramref name="mic"/> (recorded from <paramref name="time"/>) and returns the
    /// residual. <paramref name="adapt"/> false (the person is talking too) slows the filter's learning right down,
    /// so their voice doesn't teach it wrong.
    /// </summary>
    public float[] Process(ReadOnlySpan<float> mic, double time, bool adapt)
    {
        var residual = new float[mic.Length];
        if (mic.Length == 0) return residual;
        if (_origin == null)
        {
            // Nothing has played yet: nothing to take out.
            mic.CopyTo(residual);
            ReferencePower = 0;
            return residual;
        }
        var expected = (time - _origin.Value) * Rate;
        // Follow on from the last block unless the timestamps say it's clearly elsewhere.
        if (double.IsNaN(_micPosition) || Math.Abs(expected - _micPosition) > Lead * 2) _micPosition = expected;
        var start = (long)Math.Round(_micPosition) - Delay;
        _micPosition += mic.Length;

        var step = adapt ? Step : Step * 0.05f;
        var window = new float[Taps];
        double power = 0;
        for (var n = 0; n < mic.Length; n++)
        {
            // The reference that can be in this sample's echo: from Lead after the delay back Taps samples (oldest first).
            var newest = start + n + Lead;
            var from = newest - Taps + 1;
            if (!Slice(from, window)) Gather(from, window);
            var echo = Dot(_weights, window);
            var error = mic[n] - echo;
            residual[n] = error;
            var energy = Dot(window, window);
            power += energy / Taps;
            if (energy < 1e-6f) continue;
            Update(_weights, window, step * error / (energy + 1e-3f));
        }
        ReferencePower = (float)(power / mic.Length);
        Find(mic, start);
        return residual;
    }

    /// <summary>Copies the reference [from, from + Taps) when it's all in the buffer; false when part of it isn't.</summary>
    private bool Slice(long from, float[] window)
    {
        var i = from - _base;
        if (i < 0 || i + Taps > _count) return false;
        _reference.AsSpan((int)i, Taps).CopyTo(window);
        return true;
    }

    private void Gather(long from, float[] window)
    {
        for (var k = 0; k < Taps; k++) window[k] = ReferenceAt(from + k);
    }

    private static float Dot(float[] a, float[] b)
    {
        var va = MemoryMarshal.Cast<float, Vector<float>>(a);
        var vb = MemoryMarshal.Cast<float, Vector<float>>(b);
        var sum = Vector<float>.Zero;
        for (var i = 0; i < va.Length; i++) sum += va[i] * vb[i];
        var total = Vector.Dot(sum, Vector<float>.One);
        for (var i = va.Length * Vector<float>.Count; i < a.Length; i++) total += a[i] * b[i];
        return total;
    }

    private static void Update(float[] weights, float[] window, float scale)
    {
        var vw = MemoryMarshal.Cast<float, Vector<float>>(weights.AsSpan());
        var vx = MemoryMarshal.Cast<float, Vector<float>>(window);
        var s = new Vector<float>(scale);
        for (var i = 0; i < vw.Length; i++) vw[i] += s * vx[i];
        for (var i = vw.Length * Vector<float>.Count; i < weights.Length; i++) weights[i] += scale * window[i];
    }

    /// <summary>Collects the microphone for the delay finder and, every half second of it, looks for the delay.</summary>
    private void Find(ReadOnlySpan<float> mic, long start)
    {
        // The first sample collected sits at reference index _findStart + Delay (start already has Delay taken off).
        if (_findFill == 0) _findStart = start + Delay;
        var take = Math.Min(mic.Length, FindWindow - _findFill);
        mic[..take].CopyTo(_findMic.AsSpan(_findFill));
        _findFill += take;
        if (_findFill < FindWindow) return;
        _findFill = 0;
        if (FindDelay(_findMic, _findStart) is not { } found) return;
        // The same answer twice running (or the first one ever) before the filter is moved to it.
        if (Aligned && Math.Abs(found - _pendingDelay) > 16 && Math.Abs(found - Delay) > 32)
        {
            _pendingDelay = found;
            return;
        }
        _pendingDelay = found;
        if (Aligned && Math.Abs(found - Delay) <= 32) return;
        ShiftWeights(found - Delay);
        Delay = found;
        Aligned = true;
        DelayUpdates++;
    }

    /// <summary>
    /// Where the bot's voice is in <paramref name="mic"/> (recorded at reference index <paramref name="at"/> as the
    /// timestamps have it): the delay in samples from GCC-PHAT, or null when there's too little playing or no clear
    /// peak (the person talking over it, silence, music with no echo).
    /// </summary>
    public int? FindDelay(float[] mic, long at)
    {
        ArgumentNullException.ThrowIfNull(mic);
        double micEnergy = 0, refEnergy = 0;
        var re1 = new double[FindSize];
        var im1 = new double[FindSize];
        var re2 = new double[FindSize];
        var im2 = new double[FindSize];
        for (var i = 0; i < Math.Min(mic.Length, FindWindow); i++)
        {
            re1[i] = mic[i];
            micEnergy += mic[i] * mic[i];
        }
        // The reference from MaxDelay before the microphone's window to MaxAdvance after its end.
        var length = Math.Min(FindSize, FindWindow + MaxDelay + MaxAdvance);
        for (var k = 0; k < length; k++)
        {
            var value = ReferenceAt(at - MaxDelay + k);
            re2[k] = value;
            refEnergy += value * value;
        }
        if (micEnergy / FindWindow < 1e-7 || refEnergy / length < 1e-6) return null;
        Fft.Transform(re1, im1, inverse: false);
        Fft.Transform(re2, im2, inverse: false);
        for (var i = 0; i < FindSize; i++)
        {
            // conj(Mic) × Ref, whitened (PHAT) so the peak is sharp whatever the voice's spectrum.
            var r = re1[i] * re2[i] + im1[i] * im2[i];
            var m = re1[i] * im2[i] - im1[i] * re2[i];
            var magnitude = Math.Sqrt(r * r + m * m) + 1e-12;
            re1[i] = r / magnitude;
            im1[i] = m / magnitude;
        }
        Fft.Transform(re1, im1, inverse: true);
        // re1[s] = Σ mic[n] · ref[at − MaxDelay + n + s]: the echo is s = MaxDelay − delay.
        var span = MaxDelay + MaxAdvance;
        var best = 0;
        double peak = double.MinValue, sum = 0, squares = 0;
        for (var s = 0; s <= span; s++)
        {
            var value = re1[s];
            sum += value;
            squares += value * value;
            if (value > peak)
            {
                peak = value;
                best = s;
            }
        }
        var mean = sum / (span + 1);
        var spread = Math.Sqrt(Math.Max(squares / (span + 1) - mean * mean, 1e-18));
        // How far the peak stands above the rest, in standard deviations: with no echo (headphones) the best of
        // ~8000 lags of noise is about 4; a real echo stands well clear.
        LastConfidence = (peak - mean) / spread;
        if (LastConfidence < Confidence) return null;
        return MaxDelay - best;
    }

    /// <summary>Moves the filter by <paramref name="by"/> samples when the delay changes, keeping what it learnt.</summary>
    private void ShiftWeights(int by)
    {
        if (by == 0) return;
        // The window is oldest first; a later echo (bigger delay) means the same tap sits further back in it.
        var shifted = new float[Taps];
        for (var i = 0; i < Taps; i++)
        {
            var from = i - by;
            if (from >= 0 && from < Taps) shifted[i] = _weights[from];
        }
        // Moving more than the filter covers is a new room: start learning afresh.
        if (Math.Abs(by) >= Taps / 2) Array.Clear(shifted);
        shifted.CopyTo(_weights, 0);
    }
}
