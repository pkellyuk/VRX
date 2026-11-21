using System.Diagnostics;
using System.Formats.Tar;
using System.Globalization;
using ICSharpCode.SharpZipLib.BZip2;

namespace PersonaBot;

/// <summary>One of the bots' voices: a Kokoro speaker, with its accent (gb or us).</summary>
public sealed record VoiceChoice(string Id, string Name, string Accent, int Speaker, string Gender)
{
    /// <summary>"Emma · British woman", for the voice picker.</summary>
    public string Label => $"{Name} · {(Accent == "gb" ? "British" : "American")} {(Gender == "f" ? "woman" : "man")}";

    public override string ToString() => Label;
}

/// <summary>
/// The English voices offered (Kokoro v1.0's speakers, by their ids in its voices.bin: checked against the model's
/// own speaker2id table, 16 Nov 2026). The best-rated voices of each accent are first.
/// </summary>
public static class VoiceCatalog
{
    public static IReadOnlyList<VoiceChoice> All { get; } =
    [
        new("bf_emma", "Emma", "gb", 21, "f"),
        new("bm_george", "George", "gb", 26, "m"),
        new("bf_isabella", "Isabella", "gb", 22, "f"),
        new("bm_fable", "Fable", "gb", 25, "m"),
        new("bf_alice", "Alice", "gb", 20, "f"),
        new("bm_lewis", "Lewis", "gb", 27, "m"),
        new("bf_lily", "Lily", "gb", 23, "f"),
        new("bm_daniel", "Daniel", "gb", 24, "m"),
        new("af_heart", "Heart", "us", 3, "f"),
        new("af_bella", "Bella", "us", 2, "f"),
        new("am_michael", "Michael", "us", 16, "m"),
        new("am_fenrir", "Fenrir", "us", 14, "m"),
        new("af_nicole", "Nicole", "us", 6, "f"),
        new("am_puck", "Puck", "us", 18, "m"),
        new("af_sarah", "Sarah", "us", 9, "f"),
        new("am_adam", "Adam", "us", 11, "m"),
        new("af_sky", "Sky", "us", 10, "f"),
        new("am_eric", "Eric", "us", 13, "m")
    ];

    /// <summary>The voice for <paramref name="id"/>, or the default for this computer's region when it's unknown.</summary>
    public static VoiceChoice Find(string? id) =>
        All.FirstOrDefault(v => string.Equals(v.Id, id, StringComparison.OrdinalIgnoreCase)) ?? Default(CultureInfo.CurrentCulture);

    /// <summary>British English for British (and most non-American) regions, American for the Americas.</summary>
    public static VoiceChoice Default(CultureInfo culture)
    {
        ArgumentNullException.ThrowIfNull(culture);
        var american = culture.Name is "en-US" or "en-CA" or "es-US" or "en-PH" || culture.Name.EndsWith("-US", StringComparison.Ordinal);
        return All.First(v => v.Accent == (american ? "us" : "gb"));
    }
}

/// <summary>The voice settings: which voice, how fast, and when the bots speak.</summary>
public sealed class VoiceSettings
{
    /// <summary>The Kokoro voice id (see <see cref="VoiceCatalog"/>); null picks one for the region.</summary>
    public string? Name { get; set; }

    /// <summary>0.7–1.4: how fast the bots speak.</summary>
    public double Speed { get; set; } = 1.0;

    /// <summary>Guide Me's bot says what's in its bubble.</summary>
    public bool SpeakGuide { get; set; } = true;

    /// <summary>The bot's chat replies are read aloud as they arrive.</summary>
    public bool ReadReplies { get; set; }

    /// <summary>After the microphone hears you in Guide Me, your words are sent straight away (rather than left to check).</summary>
    public bool SendWhatIsHeard { get; set; } = true;

    /// <summary>
    /// Barge-in: while a bot speaks, the microphone listens, and talking over it stops it and is heard (needs the
    /// speech recognition and the speech detector).
    /// </summary>
    public bool TalkOver { get; set; } = true;

    /// <summary>The microphone to listen with (its name), or null for the system's default.</summary>
    public string? Microphone { get; set; }
}

/// <summary>What the person said to a bot, read for what it means.</summary>
public static partial class SpokenWords
{
    /// <summary>Whether <paramref name="words"/>, said over a bot, only mean "stop talking": it stops, and nothing is sent.</summary>
    public static bool IsJustStop(string words)
    {
        ArgumentNullException.ThrowIfNull(words);
        return JustStop().IsMatch(words.Trim());
    }

    [System.Text.RegularExpressions.GeneratedRegex(@"^\W*(ok(ay)?[, ]+)?(stop|wait|hang on|hold on|shush|sh+|quiet|be quiet|shut up|pause|enough|that'?s enough|no+)(( it)?( please| thanks?( you)?)?)?\W*$",
        System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant)]
    private static partial System.Text.RegularExpressions.Regex JustStop();
}

/// <summary>
/// The person talked over the bot (barge-in): the bot has stopped, and <see cref="Words"/> completes with what they
/// said ("" for nothing) once they pause. <see cref="Level"/> reports the microphone's loudness meanwhile.
/// </summary>
public sealed class TalkedOver
{
    public TalkedOver(Task<string> words)
    {
        ArgumentNullException.ThrowIfNull(words);
        Words = words;
    }

    /// <summary>What they said, once they pause.</summary>
    public Task<string> Words { get; }

    /// <summary>How loud (0–1) the microphone is as they talk.</summary>
    public event Action<float>? Level;

    /// <summary>Reports the microphone's loudness (the voice's side).</summary>
    public void ReportLevel(float level) => Level?.Invoke(level);
}

/// <summary>
/// PersonaBot's voice, when the host has one (Windows: the personabot-voice helper). Speaking and listening each
/// need their model downloaded first (<see cref="VoiceModels"/>).
/// </summary>
public interface IVoice
{
    /// <summary>Whether the voice model is installed, so the bots can speak.</summary>
    bool CanSpeak { get; }

    /// <summary>Whether the speech recognition model is installed, so the bots can listen.</summary>
    bool CanHear { get; }

    /// <summary>Whether something is being said right now.</summary>
    bool IsSpeaking { get; }

    /// <summary>Whether the bots can be talked over (barge-in): speech recognition, the speech detector and a microphone.</summary>
    bool CanBargeIn { get; }

    /// <summary>The person has talked over something said with listening on (raised on the voice's thread).</summary>
    event Action<TalkedOver>? TalkedOver;

    /// <summary>How loud (0–1) the speech playing now is, about 20 times a second while speaking; 0 when it ends.</summary>
    event Action<float>? SpeechLevel;

    /// <summary>
    /// Says <paramref name="text"/>, cutting off anything being said; true when it was said in full. With
    /// <paramref name="listen"/> (and <see cref="CanBargeIn"/>) the microphone listens as it speaks: talking over it
    /// stops it (false) and raises <see cref="TalkedOver"/>.
    /// </summary>
    Task<bool> SpeakAsync(string text, VoiceChoice voice, double speed, CancellationToken token, bool listen = false);

    /// <summary>Stops speaking at once.</summary>
    void StopSpeaking();

    /// <summary>
    /// Listens on the microphone until the person stops talking, and returns their words ("" when nothing was said).
    /// <paramref name="level"/> gets the microphone's loudness (0–1) as it listens. Cancelling stops listening and
    /// throws <see cref="OperationCanceledException"/>.
    /// </summary>
    Task<string> ListenAsync(IProgress<float>? level, CancellationToken token);

    /// <summary>Stops listening now and returns what was heard so far from <see cref="ListenAsync"/>.</summary>
    void FinishListening();

    /// <summary>Starts again with whatever models are installed now (after one is downloaded or removed).</summary>
    Task ReloadAsync();

    /// <summary>
    /// The microphones to choose from, the system's default first (none when the voice can't start): Name to pick it
    /// by (<see cref="UseMicrophone"/>), Label to show.
    /// </summary>
    Task<IReadOnlyList<(string Name, string Label)>> MicrophonesAsync(CancellationToken token);

    /// <summary>Listens with the microphone named <paramref name="name"/> from now on (null: the system's default).</summary>
    void UseMicrophone(string? name);

    /// <summary>Plays speech at <paramref name="volume"/> (0 silent, 1 full) from now on.</summary>
    void SetVolume(float volume);
}

/// <summary>A downloadable voice model: where it comes from and which of the archive's files are kept.</summary>
public sealed record VoiceModel(string Kind, string Title, string Folder, string Url, long DownloadBytes, string[] Keep, string Licence);

/// <summary>
/// The two voice models, from sherpa-onnx's GitHub releases (sizes checked 16 Nov 2026): Kokoro v1.0 (Apache-2.0),
/// full precision, as the int8 one is about four times slower on x86 CPUs; and Moonshine base English, quantized
/// (MIT; the other languages' Moonshine models are non-commercial, so only English is used). They're downloaded into
/// <c>voice</c> under PersonaBot's data folder when the person turns voice on, keeping only the files used.
/// </summary>
public static class VoiceModels
{
    public static VoiceModel Speech { get; } = new(
        "speech", "Voice (Kokoro)", "kokoro-multi-lang-v1_0",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2",
        366_000_000L,
        ["model.onnx", "voices.bin", "tokens.txt", "lexicon-gb-en.txt", "lexicon-us-en.txt", "LICENSE", "README.md"],
        "Apache-2.0");

    public static VoiceModel Hearing { get; } = new(
        "hearing", "Speech recognition (Moonshine)", "sherpa-onnx-moonshine-base-en-quantized-2026-02-27",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-base-en-quantized-2026-02-27.tar.bz2",
        111_300_000L,
        ["encoder_model.ort", "decoder_model_merged.ort", "tokens.txt", "LICENSE"],
        "MIT");

    /// <summary>
    /// Silero VAD (MIT, a single 644 KB file, checked 17 Nov 2026), for barge-in: it tells the person talking over a bot
    /// from a cough or the keyboard.
    /// </summary>
    public static VoiceModel Detector { get; } = new(
        "detector", "Talking over the bot (Silero speech detector)", "silero-vad",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx",
        643_854L,
        ["silero_vad.onnx"],
        "MIT");

    /// <summary>The speech detector's file, for the voice helper.</summary>
    public static string DetectorFile => Path.Combine(FolderOf(Detector), "silero_vad.onnx");

    /// <summary>Where the models live: "voice" in PersonaBot's data folder.</summary>
    public static string Root => Path.Combine(Settings.Root, "voice");

    /// <summary>The folder <paramref name="model"/> is installed in.</summary>
    public static string FolderOf(VoiceModel model)
    {
        ArgumentNullException.ThrowIfNull(model);
        return Path.Combine(Root, model.Folder);
    }

    /// <summary>Whether <paramref name="model"/> is installed: every file it keeps is there.</summary>
    public static bool IsInstalled(VoiceModel model)
    {
        ArgumentNullException.ThrowIfNull(model);
        var folder = FolderOf(model);
        return model.Keep.Where(f => f != "README.md" && f != "LICENSE").All(f => File.Exists(Path.Combine(folder, f)));
    }

    /// <summary>"350 MB", for buttons and hints.</summary>
    public static string Size(long bytes) => bytes >= 1_000_000_000 ? $"{bytes / 1e9:0.0} GB" : $"{bytes / 1e6:0} MB";

    /// <summary>
    /// Downloads <paramref name="model"/> and unpacks the files it keeps, reporting progress (0–1) and a line of
    /// status. The archive streams straight through bzip2 and tar to disk, so nothing extra is stored; a half-done
    /// install is in a ".partial" folder that's only renamed into place when complete.
    /// </summary>
    public static async Task InstallAsync(VoiceModel model, HttpClient client, IProgress<(double Fraction, string Status)>? progress, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(client);
        var watch = Stopwatch.StartNew();
        Trace.WriteLine($"VoiceModels.InstallAsync enter: {model.Kind} from {model.Url}");
        var folder = FolderOf(model);
        var partial = folder + ".partial";
        if (Directory.Exists(partial)) Directory.Delete(partial, recursive: true);
        Directory.CreateDirectory(partial);
        try
        {
            progress?.Report((0, $"Connecting to download {model.Title}…"));
            using var response = await client.GetAsync(model.Url, HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? model.DownloadBytes;
            Trace.WriteLine($"VoiceModels.InstallAsync: HTTP {(int)response.StatusCode}, {total} bytes");
            await using var network = await response.Content.ReadAsStreamAsync(token);
            await using var counted = new CountingStream(network, read =>
                progress?.Report((Math.Clamp(read / (double)total, 0, 1), $"Downloading {model.Title}: {Size(read)} of {Size(total)}")));
            HashSet<string> kept;
            if (!model.Url.EndsWith(".tar.bz2", StringComparison.OrdinalIgnoreCase))
            {
                // A single file (the speech detector): saved as it is.
                var name = model.Keep[0];
                await using (var file = File.Create(Path.Combine(partial, name))) await counted.CopyToAsync(file, token);
                kept = [name];
            }
            else
            {
                // Bzip2 and tar are read on a worker thread: SharpZipLib's bzip2 reads synchronously.
                kept = await Task.Run(() => Unpack(counted, partial, model.Keep, token), token);
            }
            var missing = model.Keep.Where(f => f != "README.md" && !kept.Contains(f)).ToList();
            if (missing.Count > 0) throw new InvalidDataException($"The download was missing {string.Join(", ", missing)}.");
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
            Directory.Move(partial, folder);
            progress?.Report((1, $"{model.Title} installed"));
            Trace.WriteLine($"VoiceModels.InstallAsync exit: {model.Kind} installed in {folder} ({kept.Count} files) in {watch.Elapsed.TotalSeconds:0}s");
        }
        catch
        {
            Trace.WriteLine($"VoiceModels.InstallAsync: {model.Kind} failed or cancelled after {watch.Elapsed.TotalSeconds:0}s; removing the partial folder");
            TryDelete(partial);
            throw;
        }
    }

    /// <summary>Removes an installed model.</summary>
    public static void Remove(VoiceModel model)
    {
        ArgumentNullException.ThrowIfNull(model);
        Trace.WriteLine($"VoiceModels.Remove: {model.Kind} at {FolderOf(model)}");
        TryDelete(FolderOf(model));
    }

    private static void TryDelete(string folder)
    {
        try
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Trace.TraceWarning($"VoiceModels.TryDelete: couldn't remove {folder}: {ex.Message}");
        }
    }

    /// <summary>Unpacks a .tar.bz2 stream into <paramref name="folder"/>, keeping only the named files (by file name).</summary>
    internal static HashSet<string> Unpack(Stream archive, string folder, IReadOnlyCollection<string> keep, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(archive);
        ArgumentException.ThrowIfNullOrWhiteSpace(folder);
        ArgumentNullException.ThrowIfNull(keep);
        var kept = new HashSet<string>(StringComparer.Ordinal);
        using var bzip = new BZip2InputStream(archive) { IsStreamOwner = false };
        using var tar = new TarReader(bzip);
        while (tar.GetNextEntry() is { } entry)
        {
            token.ThrowIfCancellationRequested();
            if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile)) continue;
            var path = entry.Name.StartsWith("./", StringComparison.Ordinal) ? entry.Name[2..] : entry.Name;
            var name = Path.GetFileName(path);
            // Only top-level files of the model's own folder: nothing nested (test audio), nothing outside.
            if (!keep.Contains(name) || path.Count(c => c == '/') > 1 || entry.DataStream == null) continue;
            var target = Path.Combine(folder, name);
            using (var file = File.Create(target)) entry.DataStream.CopyTo(file);
            kept.Add(name);
            Trace.WriteLine($"VoiceModels.Unpack: {name} ({entry.Length} bytes)");
        }
        return kept;
    }

    /// <summary>A read-only stream that reports how many bytes have been read, at most about ten times a second.</summary>
    private sealed class CountingStream(Stream inner, Action<long> report) : Stream
    {
        private long _read;
        private long _lastReport;

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => _read; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        public override int Read(byte[] buffer, int offset, int count)
        {
            var n = inner.Read(buffer, offset, count);
            Count(n);
            return n;
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            var n = await inner.ReadAsync(buffer, cancellationToken);
            Count(n);
            return n;
        }

        private void Count(int n)
        {
            _read += n;
            var now = Environment.TickCount64;
            if (now - _lastReport < 100 && n > 0) return;
            _lastReport = now;
            report(_read);
        }
    }
}
