namespace PersonaBot.Voice;

/// <summary>
/// Changes a stream's sample rate by a fixed ratio (Kokoro's 24 kHz speech to the microphone's 16 kHz, for the echo
/// canceller's copy of what's playing), block by block with no gaps or clicks between blocks. Polyphase windowed-sinc:
/// up by L, low-pass below the lower Nyquist, down by M, so nothing above 8 kHz folds back into the speech band.
/// </summary>
public sealed class Resampler
{
    private const int TapsPerPhase = 24;

    private readonly int _up;
    private readonly int _down;
    private readonly float[][] _phases;
    private readonly float[] _history = new float[TapsPerPhase];
    private int _phase;

    public Resampler(int fromRate, int toRate)
    {
        if (fromRate <= 0) throw new ArgumentOutOfRangeException(nameof(fromRate));
        if (toRate <= 0) throw new ArgumentOutOfRangeException(nameof(toRate));
        var divisor = Gcd(fromRate, toRate);
        _up = toRate / divisor;
        _down = fromRate / divisor;
        FromRate = fromRate;
        ToRate = toRate;
        // The prototype low-pass, at the up-sampled rate: cut-off at the lower of the two Nyquists, a little under.
        var length = TapsPerPhase * _up;
        var cutoff = 0.92 * 0.5 / Math.Max(_up, _down);
        var prototype = new double[length];
        for (var i = 0; i < length; i++)
        {
            var t = i - (length - 1) / 2.0;
            var sinc = Math.Abs(t) < 1e-9 ? 2 * cutoff : Math.Sin(2 * Math.PI * cutoff * t) / (Math.PI * t);
            var window = 0.42 - 0.5 * Math.Cos(2 * Math.PI * i / (length - 1)) + 0.08 * Math.Cos(4 * Math.PI * i / (length - 1));
            prototype[i] = sinc * window * _up;
        }
        _phases = new float[_up][];
        for (var p = 0; p < _up; p++)
        {
            _phases[p] = new float[TapsPerPhase];
            for (var k = 0; k < TapsPerPhase; k++) _phases[p][k] = (float)prototype[k * _up + p];
        }
    }

    public int FromRate { get; }

    public int ToRate { get; }

    /// <summary>The block in the new rate (carrying on from the last block).</summary>
    public float[] Process(ReadOnlySpan<float> input)
    {
        if (_up == 1 && _down == 1) return input.ToArray();
        var output = new List<float>(input.Length * _up / _down + 2);
        foreach (var sample in input)
        {
            // Newest first.
            Array.Copy(_history, 0, _history, 1, TapsPerPhase - 1);
            _history[0] = sample;
            // This input sample makes output samples at every up-sampled position it covers that lands on a multiple of M.
            while (_phase < _up)
            {
                var taps = _phases[_phase];
                float sum = 0;
                for (var k = 0; k < TapsPerPhase; k++) sum += taps[k] * _history[k];
                output.Add(sum);
                _phase += _down;
            }
            _phase -= _up;
        }
        return output.ToArray();
    }

    private static int Gcd(int a, int b)
    {
        while (b != 0) (a, b) = (b, a % b);
        return a;
    }
}
