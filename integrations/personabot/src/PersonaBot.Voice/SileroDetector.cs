using SherpaOnnx;

namespace PersonaBot.Voice;

/// <summary>
/// Silero VAD (MIT, 644 KB) through sherpa-onnx: whether a 32 ms frame at 16 kHz is speech, so barge-in tells the
/// person talking from a cough, a door or the keyboard.
/// </summary>
public sealed class SileroDetector : ISpeechDetector, IDisposable
{
    private readonly VoiceActivityDetector _vad;
    private readonly Action<string> _log;

    public SileroDetector(string model, Action<string> log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(model);
        ArgumentNullException.ThrowIfNull(log);
        if (!File.Exists(model)) throw new FileNotFoundException("The speech detector model isn't there.", model);
        _log = log;
        var config = new VadModelConfig();
        config.SileroVad.Model = model;
        config.SileroVad.Threshold = 0.5f;
        // Short on purpose: the barge-in decision does its own counting over about 300 ms.
        config.SileroVad.MinSpeechDuration = 0.064f;
        config.SileroVad.MinSilenceDuration = 0.1f;
        config.SileroVad.MaxSpeechDuration = 30f;
        config.SileroVad.WindowSize = BargeInDetector.Frame;
        config.SampleRate = EchoCanceller.Rate;
        config.NumThreads = 1;
        config.Provider = "cpu";
        config.Debug = 0;
        _vad = new VoiceActivityDetector(config, 5f);
        _log($"SileroDetector: loaded {model}");
    }

    /// <summary>Whether <paramref name="model"/> is a speech detector file this can load.</summary>
    public static bool IsModel(string? model) => !string.IsNullOrWhiteSpace(model) && File.Exists(model);

    public bool IsSpeech(float[] frame)
    {
        ArgumentNullException.ThrowIfNull(frame);
        _vad.AcceptWaveform(SpeechLevels.ForRecognition(frame));
        var speech = _vad.IsSpeechDetected();
        // Finished segments aren't wanted (only the yes or no): drop them so its buffer doesn't grow.
        while (!_vad.IsEmpty()) _vad.Pop();
        return speech;
    }

    public void Reset()
    {
        _vad.Reset();
    }

    public void Dispose() => _vad.Dispose();
}
