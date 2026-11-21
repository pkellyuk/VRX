using System.Diagnostics;
using System.Runtime.InteropServices;
using SherpaOnnx;

namespace PersonaBot.Voice;

/// <summary>
/// Kokoro text to speech through sherpa-onnx. The model folder is a sherpa-onnx Kokoro release (model.onnx,
/// voices.bin, tokens.txt and the English lexicons). The words are turned into phonemes here (<see cref="SpeechText"/>
/// then <see cref="Phonemizer"/>, British or American from that accent's lexicon) and given to Kokoro as phonemes,
/// so no word is skipped. One engine serves both accents; each accent's lexicon loads the first time it's wanted.
/// </summary>
public sealed class Speaker : IDisposable
{
    private readonly string _folder;
    private readonly Action<string> _log;
    private readonly object _gate = new();
    private readonly Dictionary<string, (SpeechText Text, Phonemizer Phonemes)> _languages = [];
    private OfflineTts? _engine;

    public Speaker(string folder, Action<string> log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        ArgumentNullException.ThrowIfNull(log);
        _folder = folder;
        _log = log;
    }

    /// <summary>Whether <paramref name="folder"/> holds a Kokoro model this can load.</summary>
    public static bool IsModel(string? folder) =>
        !string.IsNullOrWhiteSpace(folder) && File.Exists(ModelFile(folder)) && File.Exists(Path.Combine(folder, "voices.bin")) &&
        File.Exists(Path.Combine(folder, "tokens.txt")) && Directory.EnumerateFiles(folder, "lexicon-*-en.txt").Any();

    /// <summary>The full-precision model when it's there: on x86 CPUs it's about four times faster than the int8 one.</summary>
    private static string ModelFile(string folder) =>
        File.Exists(Path.Combine(folder, "model.onnx")) ? Path.Combine(folder, "model.onnx") : Path.Combine(folder, "model.int8.onnx");

    /// <summary>The samples per second of the speech it makes (Kokoro: 24000).</summary>
    public int SampleRate => Engine().SampleRate;

    /// <summary>Loads the engine and <paramref name="accent"/>'s lexicon now, so the first words aren't slow.</summary>
    public void Warm(string? accent)
    {
        Engine();
        Language(accent);
    }

    /// <summary>The words Kokoro will be given for <paramref name="text"/> (see <see cref="SpeechText"/>).</summary>
    public string Prepare(string text, string? accent)
    {
        ArgumentNullException.ThrowIfNull(text);
        return Language(accent).Text.Prepare(text);
    }

    /// <summary>The phonemes Kokoro will be given for <paramref name="text"/>, a chunk at a time.</summary>
    public List<string> Phonemes(string text, string? accent)
    {
        ArgumentNullException.ThrowIfNull(text);
        var language = Language(accent);
        return language.Phonemes.Chunks(language.Text.Prepare(text));
    }

    /// <summary>
    /// Says <paramref name="text"/> in speaker <paramref name="speaker"/>, handing each piece of audio (a sentence or
    /// so) to <paramref name="samples"/> as it's made. Returns false if <paramref name="token"/> stopped it.
    /// </summary>
    public bool Say(string text, int speaker, string? accent, float speed, Action<float[]> samples, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentNullException.ThrowIfNull(samples);
        if (string.IsNullOrWhiteSpace(text)) return true;
        var watch = Stopwatch.StartNew();
        var engine = Engine();
        var chunks = Phonemes(text, accent);
        _log($"Speaker.Say enter: {text.Length} characters as {chunks.Count} chunk(s), speaker {speaker}, accent {Accent(accent)}, speed {speed:0.00}, prepared in {watch.ElapsedMilliseconds} ms");
        if (chunks.Count == 0) return true;
        var first = -1L;
        var config = new OfflineTtsGenerationConfig
        {
            Sid = Math.Clamp(speaker, 0, Math.Max(0, engine.NumSpeakers - 1)),
            Speed = Math.Clamp(speed, 0.5f, 2f),
            SilenceScale = 0.2f,
            PhonemeCodepoints = [.. chunks.Select(c => c.EnumerateRunes().Select(r => r.Value).ToArray())]
        };
        OfflineTtsCallbackProgressWithArg callback = (pointer, count, progress, arg) =>
        {
            if (token.IsCancellationRequested) return 0;
            if (count <= 0 || pointer == IntPtr.Zero) return 1;
            var block = new float[count];
            Marshal.Copy(pointer, block, 0, count);
            if (first < 0) first = watch.ElapsedMilliseconds;
            samples(block);
            return token.IsCancellationRequested ? 0 : 1;
        };
        var audio = engine.GenerateWithConfig(text, config, callback);
        try
        {
            GC.KeepAlive(callback);
        }
        finally
        {
            audio.Dispose();
        }
        _log($"Speaker.Say exit: first audio after {first} ms, all made in {watch.ElapsedMilliseconds} ms, stopped={token.IsCancellationRequested}");
        return !token.IsCancellationRequested;
    }

    private static string Accent(string? accent) => string.Equals(accent, "us", StringComparison.OrdinalIgnoreCase) ? "us" : "gb";

    /// <summary>An accent's text preparer and phonemizer, from its lexicon.</summary>
    private (SpeechText Text, Phonemizer Phonemes) Language(string? accent)
    {
        var key = Accent(accent);
        lock (_gate)
        {
            if (_languages.TryGetValue(key, out var language)) return language;
            var watch = Stopwatch.StartNew();
            var file = Path.Combine(_folder, $"lexicon-{key}-en.txt");
            if (!File.Exists(file)) file = Directory.EnumerateFiles(_folder, "lexicon-*-en.txt").First();
            var lexicon = Phonemizer.ReadLexicon(file);
            var phonemizer = new Phonemizer(lexicon, Phonemizer.ReadTokens(Path.Combine(_folder, "tokens.txt")), key);
            language = (new SpeechText(phonemizer.Knows), phonemizer);
            _languages[key] = language;
            _log($"Speaker.Language: {lexicon.Count} words from {Path.GetFileName(file)} in {watch.ElapsedMilliseconds} ms");
            return language;
        }
    }

    private OfflineTts Engine()
    {
        lock (_gate)
        {
            if (_engine != null) return _engine;
            var watch = Stopwatch.StartNew();
            _log($"Speaker.Engine: loading Kokoro from {_folder}");
            var config = new OfflineTtsConfig();
            config.Model.Kokoro.Model = ModelFile(_folder);
            config.Model.Kokoro.Voices = Path.Combine(_folder, "voices.bin");
            config.Model.Kokoro.Tokens = Path.Combine(_folder, "tokens.txt");
            config.Model.NumThreads = int.TryParse(Environment.GetEnvironmentVariable("PB_VOICE_THREADS"), out var threads) ? threads : Math.Clamp(Environment.ProcessorCount / 2, 1, 4);
            config.Model.Provider = "cpu";
            config.Model.Debug = Environment.GetEnvironmentVariable("PB_VOICE_DEBUG") == "1" ? 1 : 0;
            config.MaxNumSentences = 1;
            _engine = new OfflineTts(config);
            _log($"Speaker.Engine: loaded {Path.GetFileName(ModelFile(_folder))} in {watch.ElapsedMilliseconds} ms ({_engine.NumSpeakers} speakers, {_engine.SampleRate} Hz, {config.Model.NumThreads} threads)");
            return _engine;
        }
    }

    public void Dispose()
    {
        _engine?.Dispose();
        _engine = null;
    }
}
