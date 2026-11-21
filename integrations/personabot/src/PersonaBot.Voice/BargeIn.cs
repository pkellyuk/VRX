namespace PersonaBot.Voice;

/// <summary>Says whether a 32 ms frame (512 samples at 16 kHz) is someone speaking (Silero VAD in the helper).</summary>
public interface ISpeechDetector
{
    /// <summary>Whether <paramref name="frame"/> is speech (the detector keeps its own state across frames).</summary>
    bool IsSpeech(float[] frame);

    /// <summary>Starts afresh (a new utterance).</summary>
    void Reset();
}

/// <summary>
/// Barge-in: while the bot speaks, decides when the person starts talking over it. The microphone goes through the
/// <see cref="EchoCanceller"/> (taking the bot's own voice out) and is judged a frame (32 ms) at a time. A frame is
/// the person when the speech detector hears speech and the sound is clearly more than the bot's echo could be:
/// <see cref="DoubleTalkRatio"/> × the echo's learnt share of what's playing, plus the room's noise. Until the
/// canceller has settled (it takes out at least <see cref="SettledErle"/> of the echo) that's judged on the raw
/// microphone against the raw echo, which doesn't depend on the canceller; once it has, on the residual against
/// what's left of the echo, which is far more sensitive. Each share follows the echo's peaks (up quickly, down
/// slowly) from frames that aren't the person, so loud moments of the bot's voice aren't taken for them. Enough person
/// frames close together (<see cref="FramesNeeded"/> of the last <see cref="FramesWindow"/>, about 300 ms) is a
/// barge-in. What's learnt of the room carries on between utterances. While the person talks the canceller barely
/// learns, so their voice doesn't teach it wrong; after a barge-in the residual carries on for the words to be heard from.
/// </summary>
public sealed class BargeInDetector
{
    /// <summary>A frame: 32 ms, the speech detector's window.</summary>
    public const int Frame = 512;

    /// <summary>How much more than the expected echo (power) the sound must be to be the person: 6 dB.</summary>
    public const float DoubleTalkRatio = 4f;

    /// <summary>How much of the echo (power ratio) the canceller must take out before the residual is trusted: 12 dB.</summary>
    public const float SettledErle = 16f;

    /// <summary>Frames of the person talking needed, out of the last <see cref="FramesWindow"/>.</summary>
    public const int FramesNeeded = 9, FramesWindow = 13;

    /// <summary>Frames the bot must have played (ever) before its echo is known: it can't be talked over until then.</summary>
    public const int WarmFrames = 10;

    /// <summary>How long the bot must have been playing in this utterance before it can be talked over (its own onset).</summary>
    public const float SettleSeconds = 0.3f;

    /// <summary>How much of the residual before the barge-in is kept for the words (the start of what they said).</summary>
    public const float PrerollSeconds = 0.5f;

    private readonly EchoCanceller _echo;
    private readonly ISpeechDetector _speech;
    private readonly List<float> _frame = new(Frame);
    private readonly List<float> _rawFrame = new(Frame);
    private readonly Queue<bool> _recent = new();
    private readonly LinkedList<float[]> _history = new();
    private int _historySamples;
    // What's learnt of the room (kept between utterances).
    private float _rawShare = 1e-6f;
    private float _erle = 1f;
    private int _erleFrames;
    private int _learnt;
    private float _noise = 1e-5f;
    // This utterance.
    private float _referenceFrame;
    private double _played;
    private int _talking;

    public BargeInDetector(EchoCanceller echo, ISpeechDetector speech)
    {
        ArgumentNullException.ThrowIfNull(echo);
        ArgumentNullException.ThrowIfNull(speech);
        _echo = echo;
        _speech = speech;
    }

    /// <summary>The echo canceller (its delay, whether it's aligned), for the log.</summary>
    public EchoCanceller Echo => _echo;

    /// <summary>The echo's share of what's playing as the microphone hears it (power ratio), learnt as the bot talks.</summary>
    public float Coupling => _rawShare;

    /// <summary>How much of the echo the canceller takes out (power ratio), learnt as the bot talks.</summary>
    public float Erle => _erle;

    /// <summary>Whether the canceller takes out enough of the echo for the residual to be judged.</summary>
    public bool Settled => _erleFrames >= SettledFrames && _echo.Aligned && _erle >= SettledErle;

    /// <summary>Frames of the bot talking alone (2 s, ever) the canceller must have been judged over before it's trusted.</summary>
    public const int SettledFrames = 62;

    /// <summary>Each frame's judgement, when set (for tuning: personabot-voice --bargetest with PB_BARGE_TRACE=2).</summary>
    public Action<string>? FrameTrace { get; set; }

    /// <summary>Whether the person has talked over the bot in this utterance.</summary>
    public bool BargedIn { get; private set; }

    /// <summary>Why it barged in, for the log.</summary>
    public string? Reason { get; private set; }

    /// <summary>Frames judged so far in this utterance (for the log).</summary>
    public int Frames { get; private set; }

    /// <summary>Frames judged to be the person talking (for the log).</summary>
    public int PersonFrames { get; private set; }

    /// <summary>A new utterance: the timeline and the counts start afresh; what was learnt of the room carries on.</summary>
    public void BeginUtterance()
    {
        _echo.Restart();
        _speech.Reset();
        _frame.Clear();
        _rawFrame.Clear();
        _recent.Clear();
        _history.Clear();
        _historySamples = 0;
        _played = 0;
        _talking = 0;
        _referenceFrame = 0;
        BargedIn = false;
        Reason = null;
        Frames = 0;
        PersonFrames = 0;
    }

    /// <summary>What's playing, at 16 kHz, reaching the speaker at <paramref name="time"/>.</summary>
    public void AddReference(ReadOnlySpan<float> samples, double time)
    {
        _echo.AddReference(samples, time);
        double sum = 0;
        foreach (var s in samples) sum += s * s;
        if (samples.Length > 0 && sum / samples.Length > 1e-6) _played += samples.Length / (double)EchoCanceller.Rate;
    }

    /// <summary>
    /// A block of the microphone recorded from <paramref name="time"/>: returns the residual (the bot's voice taken
    /// out). <see cref="BargedIn"/> turns true on the block where the person is found talking over the bot; after
    /// that the residual just carries on.
    /// </summary>
    public float[] Process(ReadOnlySpan<float> mic, double time)
    {
        var residual = _echo.Process(mic, time, adapt: _talking == 0 && !BargedIn);
        if (BargedIn) return residual;
        Remember(residual);
        for (var i = 0; i < residual.Length; i++)
        {
            _frame.Add(residual[i]);
            _rawFrame.Add(mic[i]);
            if (_frame.Count < Frame) continue;
            Judge(_frame.ToArray(), Power(_rawFrame), _echo.ReferencePower);
            _frame.Clear();
            _rawFrame.Clear();
            if (BargedIn) break;
        }
        return residual;
    }

    private static float Power(IReadOnlyList<float> samples)
    {
        double sum = 0;
        foreach (var s in samples) sum += s * s;
        return samples.Count == 0 ? 0 : (float)(sum / samples.Count);
    }

    private void Judge(float[] frame, float raw, float referencePower)
    {
        Frames++;
        var residual = Power(frame);
        // The reference that could echo in this frame: held a little, as the echo lingers in the room.
        _referenceFrame = Math.Max(referencePower, _referenceFrame * 0.85f);
        var playing = _referenceFrame > 1e-6f;
        if (!playing)
        {
            // The room's own noise: learnt while nothing plays.
            _noise = raw < _noise ? _noise * 0.8f + raw * 0.2f : _noise * 0.99f + raw * 0.01f;
            _noise = Math.Clamp(_noise, 1e-9f, 1e-2f);
        }
        var settled = Settled;
        var rawLouder = raw > DoubleTalkRatio * _rawShare * _referenceFrame + 4 * _noise;
        // What's left of the echo after the canceller: the raw echo less what it takes out. Derived rather than learnt
        // on its own, so it falls as the canceller settles, and the person's voice can't talk it up.
        var residualShare = _rawShare / _erle;
        var residualLouder = residual > DoubleTalkRatio * residualShare * _referenceFrame + 4 * _noise;
        var louder = settled ? residualLouder : rawLouder;
        // The first third of a second the bot ever plays is learnt from whatever it hears (the shares start at
        // nothing, so its own echo would otherwise look like the person and never be learnt).
        var warming = _learnt < WarmFrames;
        if (playing && (warming || !rawLouder && !residualLouder))
        {
            // Only the bot (and the room): learn its echo. The shares follow the peaks: up fast, down slowly.
            _learnt++;
            _rawShare = Follow(_rawShare, Math.Max(0, raw - _noise) / _referenceFrame);
        }
        if (playing && !rawLouder && residualLouder && settled)
        {
            // Maybe the person (or the room changed, a moved laptop): don't learn from it, but drift down slowly so a
            // real change in the room is re-learnt within a second or two, long after a person would have been heard.
            _erle *= 0.995f;
        }
        else if (playing && !rawLouder && raw > 16 * _noise)
        {
            // How much the canceller takes out, judged pessimistically: down quickly to its worst moments, up slowly.
            // While it's young it cancels well on sounds it has heard and poorly on new ones, and the residual
            // must only be trusted for what it reliably takes out.
            var erle = Math.Clamp(raw / Math.Max(residual, 1e-12f), 1f, 1e5f);
            _erle = erle < _erle ? _erle * 0.7f + erle * 0.3f : _erle * 0.98f + erle * 0.02f;
            _erleFrames++;
        }
        var speech = _speech.IsSpeech(frame);
        var person = speech && louder;
        FrameTrace?.Invoke($"{Frames * Frame / 16000.0:0.00}s raw {Db(raw)} res {Db(residual)} play {Db(_referenceFrame)} rawShare {Db(_rawShare)} erle {Db(_erle)} " +
                           $"settled {settled} rawLouder {rawLouder} resLouder {residualLouder} speech {speech} person {person}");
        _talking = rawLouder || residualLouder ? 3 : Math.Max(0, _talking - 1);
        if (person) PersonFrames++;
        _recent.Enqueue(person);
        while (_recent.Count > FramesWindow) _recent.Dequeue();
        if (_played < SettleSeconds || _learnt < WarmFrames) return;
        var count = _recent.Count(p => p);
        if (count < FramesNeeded) return;
        BargedIn = true;
        Reason = $"{count} of the last {_recent.Count} frames were speech over the echo, judged on the {(settled ? "residual" : "raw microphone")} " +
                 $"(raw {Db(raw)} dB, residual {Db(residual)} dB, playing {Db(_referenceFrame)} dB, echo share raw {Db(_rawShare)} dB / residual {Db(residualShare)} dB, " +
                 $"echo taken out {Db(_erle)} dB, noise {Db(_noise)} dB, delay {_echo.Delay / 16.0:0} ms{(_echo.Aligned ? "" : " (not yet found)")}, after {_played:0.0}s of speech)";
    }

    private static float Follow(float share, float now)
    {
        var next = now > share ? share * 0.7f + now * 0.3f : share * 0.97f + now * 0.03f;
        return Math.Clamp(next, 1e-6f, 16f);
    }

    private static string Db(float power) => (10 * Math.Log10(power + 1e-12)).ToString("0", System.Globalization.CultureInfo.InvariantCulture);

    private void Remember(float[] residual)
    {
        _history.AddLast(residual);
        _historySamples += residual.Length;
        var keep = (int)((PrerollSeconds + FramesWindow * Frame / (double)EchoCanceller.Rate) * EchoCanceller.Rate);
        while (_history.Count > 1 && _historySamples - _history.First!.Value.Length >= keep)
        {
            _historySamples -= _history.First.Value.Length;
            _history.RemoveFirst();
        }
    }

    /// <summary>
    /// The residual from a little before the person started (their first words) up to now, for the recognizer to
    /// carry on from after a barge-in.
    /// </summary>
    public float[] TakePreroll()
    {
        var all = _history.SelectMany(b => b).ToArray();
        _history.Clear();
        _historySamples = 0;
        return all;
    }
}
