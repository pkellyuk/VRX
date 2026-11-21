namespace PersonaBot.Voice;

/// <summary>
/// Decides, from the microphone's loudness, when the person has started and finished talking. It learns the room's
/// background noise from the quietest recent frames; a frame well above that is speech. After speech, a stretch of
/// quiet as long as the silence setting ends it. Without any speech, it gives up after <see cref="NoSpeechSeconds"/>.
/// No model: plain loudness is enough for "talk, then pause", and the person can always press to finish.
/// </summary>
public sealed class Endpointer
{
    private readonly float _silenceSeconds;
    private readonly float _maxSeconds;
    private float _noise = 0.004f;
    private float _quietFor;
    private float _speechFor;

    public Endpointer(float silenceSeconds, float maxSeconds, float noSpeechSeconds = 8f)
    {
        _silenceSeconds = Math.Clamp(silenceSeconds, 0.3f, 10f);
        _maxSeconds = Math.Clamp(maxSeconds, 1f, 120f);
        NoSpeechSeconds = Math.Clamp(noSpeechSeconds, 1f, _maxSeconds);
    }

    /// <summary>How long it waits for the person to start talking before giving up.</summary>
    public float NoSpeechSeconds { get; }

    /// <summary>Seconds of audio seen so far.</summary>
    public float Elapsed { get; private set; }

    /// <summary>Whether the person has said something (enough loud frames to be more than a click or a cough).</summary>
    public bool HeardSpeech { get; private set; }
    public float SpeechStartedAt { get; private set; }

    /// <summary>Whether listening should stop now.</summary>
    public bool Done { get; private set; }

    /// <summary>Why it stopped: "pause", "no speech" or "time"; null while listening.</summary>
    public string? Reason { get; private set; }

    /// <summary>The loudness (0–1) to show for the last frame, scaled so normal speech fills most of the meter.</summary>
    public float Level { get; private set; }

    /// <summary>The person is already talking (they talked over the bot): it waits for them to pause, not to start.</summary>
    public void MarkSpeech()
    {
        HeardSpeech = true;
        _speechFor = Math.Max(_speechFor, 0.15f);
    }

    /// <summary>Takes one frame of mono samples (any length, at <paramref name="sampleRate"/>) and updates the state.</summary>
    public void Add(ReadOnlySpan<float> samples, int sampleRate, bool? speechDetected = null)
    {
        if (samples.Length == 0 || sampleRate <= 0 || Done) return;
        double sum = 0;
        foreach (var s in samples) sum += s * s;
        var rms = (float)Math.Sqrt(sum / samples.Length);
        var seconds = samples.Length / (float)sampleRate;
        Elapsed += seconds;
        Level = Math.Clamp(rms * 8f, 0f, 1f);

        // Background noise: falls quickly to quiet frames and creeps up slowly, so speech doesn't become "noise".
        _noise = rms < _noise ? _noise * 0.7f + rms * 0.3f : _noise * 0.995f + rms * 0.005f;
        _noise = Math.Clamp(_noise, 0.0005f, 0.05f);
        var speaking = speechDetected ?? (rms > Math.Max(0.012f, _noise * 3.5f));

        if (speaking)
        {
            if (!HeardSpeech && _speechFor == 0) SpeechStartedAt = Elapsed - seconds;
            _speechFor += seconds;
            _quietFor = 0;
            if (_speechFor >= 0.15f) HeardSpeech = true;
        }
        else
        {
            _quietFor += seconds;
            if (!HeardSpeech && _quietFor > 0.3f) _speechFor = 0;
        }

        if (HeardSpeech && _quietFor >= _silenceSeconds) Stop("pause");
        else if (!HeardSpeech && Elapsed >= NoSpeechSeconds) Stop("no speech");
        else if (Elapsed >= _maxSeconds) Stop("time");
    }

    private void Stop(string reason)
    {
        Done = true;
        Reason = reason;
    }
}
