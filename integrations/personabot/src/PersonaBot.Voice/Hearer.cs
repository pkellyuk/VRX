using System.Diagnostics;
using SherpaOnnx;

namespace PersonaBot.Voice;

/// <summary>
/// Moonshine speech to text through sherpa-onnx: turns a recording into words. The model folder is a sherpa-onnx
/// Moonshine release: v2 (encoder_model.ort and decoder_model_merged.ort) or v1 (preprocess, encode,
/// uncached_decode and cached_decode), with tokens.txt.
/// </summary>
public sealed class Hearer : IDisposable
{
    private readonly string _folder;
    private readonly Action<string> _log;
    private OfflineRecognizer? _recognizer;

    public Hearer(string folder, Action<string> log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        ArgumentNullException.ThrowIfNull(log);
        _folder = folder;
        _log = log;
    }

    /// <summary>Whether <paramref name="folder"/> holds a Moonshine model this can load.</summary>
    public static bool IsModel(string? folder) =>
        !string.IsNullOrWhiteSpace(folder) && File.Exists(Path.Combine(folder, "tokens.txt")) &&
        (Find(folder, "encoder_model") != null && Find(folder, "decoder_model_merged") != null || Find(folder, "preprocess") != null);

    private static string? Find(string folder, string name) =>
        new[] { ".ort", ".int8.onnx", ".onnx" }.Select(ext => Path.Combine(folder, name + ext)).FirstOrDefault(File.Exists);

    /// <summary>Loads the model now, so the first words aren't slow.</summary>
    public void Warm() => Recognizer();

    /// <summary>The words in <paramref name="samples"/> (mono, at <paramref name="sampleRate"/>), or "" for none.</summary>
    public string Transcribe(float[] samples, int sampleRate)
    {
        ArgumentNullException.ThrowIfNull(samples);
        if (samples.Length == 0 || sampleRate <= 0) return "";
        var recognizer = Recognizer();
        var watch = Stopwatch.StartNew();
        _log($"Hearer.Transcribe enter: {samples.Length / (double)sampleRate:0.0}s of audio");
        using var stream = recognizer.CreateStream();
        stream.AcceptWaveform(sampleRate, samples);
        recognizer.Decode(stream);
        var text = (stream.Result.Text ?? "").Trim();
        _log($"Hearer.Transcribe exit: {text.Length} characters in {watch.ElapsedMilliseconds} ms");
        return text;
    }

    private OfflineRecognizer Recognizer()
    {
        if (_recognizer != null) return _recognizer;
        var watch = Stopwatch.StartNew();
        _log($"Hearer.Recognizer: loading Moonshine from {_folder}");
        var config = new OfflineRecognizerConfig();
        if (Find(_folder, "decoder_model_merged") is { } merged)
        {
            config.ModelConfig.Moonshine.Encoder = Find(_folder, "encoder_model")!;
            config.ModelConfig.Moonshine.MergedDecoder = merged;
        }
        else
        {
            config.ModelConfig.Moonshine.Preprocessor = Find(_folder, "preprocess") ?? "";
            config.ModelConfig.Moonshine.Encoder = Find(_folder, "encode") ?? "";
            config.ModelConfig.Moonshine.UncachedDecoder = Find(_folder, "uncached_decode") ?? "";
            config.ModelConfig.Moonshine.CachedDecoder = Find(_folder, "cached_decode") ?? "";
        }
        config.ModelConfig.Tokens = Path.Combine(_folder, "tokens.txt");
        config.ModelConfig.NumThreads = Math.Clamp(Environment.ProcessorCount / 2, 1, 4);
        config.ModelConfig.Provider = "cpu";
        config.ModelConfig.Debug = 0;
        _recognizer = new OfflineRecognizer(config);
        _log($"Hearer.Recognizer: loaded in {watch.ElapsedMilliseconds} ms");
        return _recognizer;
    }

    public void Dispose()
    {
        _recognizer?.Dispose();
        _recognizer = null;
    }
}
