using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using PersonaBot.Voice;

namespace PersonaBot;

/// <summary>
/// PersonaBot's voice on Windows: runs the personabot-voice helper (shipped beside the app in "voice") with the
/// models downloaded into the data folder's "voice" (<see cref="VoiceModels"/>), and talks to it in JSON lines
/// (VoiceProtocol.cs). The helper starts on first use and again after it stops or the models change; requests are
/// numbered, and each speak or listen is a task completed by the helper's matching event.
/// </summary>
public sealed class VoiceClient : IVoice, IAsyncDisposable
{
    private readonly string _helper;
    private readonly SemaphoreSlim _startGate = new(1);
    private readonly object _writeLock = new();
    private readonly ConcurrentDictionary<long, TaskCompletionSource<VoiceEvent>> _pending = new();
    private Process? _process;
    private Task? _reader;
    private bool _canSpeakNow;
    private bool _canHearNow;
    private bool _canBargeInNow;
    private TalkedOver? _talkedOver;
    private string? _microphone;
    private float _volume = 1f;
    private float _pan;
    private long _nextId;
    private long _speaking;
    private long _listening;
    private IProgress<float>? _listenLevel;
    public event Action<string>? PartialWords;
    private bool _disposed;

    private VoiceClient(string helper)
    {
        _helper = helper;
        Trace.WriteLine($"VoiceClient: created for {helper}");
    }

    /// <summary>The voice when the helper is installed with the app; otherwise null (the models are downloaded later).</summary>
    public static VoiceClient? TryCreate()
    {
        var helper = Path.Combine(AppContext.BaseDirectory, "voice", "personabot-voice.exe");
        var present = File.Exists(helper);
        Trace.WriteLine($"VoiceClient.TryCreate: {(present ? "found" : "missing")} {helper}");
        return present ? new VoiceClient(helper) : null;
    }

    public bool CanSpeak => VoiceModels.IsInstalled(VoiceModels.Speech);

    public bool CanHear => VoiceModels.IsInstalled(VoiceModels.Hearing);

    public bool IsSpeaking => Interlocked.Read(ref _speaking) != 0;

    public bool CanBargeIn => CanHear && VoiceModels.IsInstalled(VoiceModels.Detector) && (_process is not { HasExited: false } || _canBargeInNow);

    public event Action<float>? SpeechLevel;

    public event Action<TalkedOver>? TalkedOver;

    public Task<bool> SpeakAsync(string text, VoiceChoice voice, double speed, CancellationToken token, bool listen = false) =>
        SpeakAsync(text, voice, speed, token, listen, partial: false);

    public async Task<bool> SpeakAsync(string text, VoiceChoice voice, double speed, CancellationToken token, bool listen, bool partial)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentNullException.ThrowIfNull(voice);
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (string.IsNullOrWhiteSpace(text)) return true;
        if (!CanSpeak)
        {
            Trace.WriteLine("VoiceClient.SpeakAsync: no voice model installed");
            return false;
        }
        await EnsureStartedAsync(voice.Accent, token);
        if (!_canSpeakNow)
        {
            Trace.TraceWarning("VoiceClient.SpeakAsync: the helper can't speak (see voice.log)");
            return false;
        }
        var id = Interlocked.Increment(ref _nextId);
        var done = Pending(id);
        Interlocked.Exchange(ref _speaking, id);
        listen &= _canBargeInNow;
        Trace.WriteLine($"VoiceClient.SpeakAsync enter: #{id}, {text.Length} characters, voice {voice.Id}, speed {speed:0.00}, may be talked over={listen}");
        Send(new VoiceRequest("speak", id, text, voice.Speaker, voice.Accent, (float)Math.Clamp(speed, 0.5, 2), Listen: listen, Partial: listen && partial));
        await using var stop = token.Register(() => Send(new VoiceRequest("stop")));
        try
        {
            var result = await done.Task.WaitAsync(token);
            Trace.WriteLine($"VoiceClient.SpeakAsync exit: #{id} stopped={result.Stopped}, error={result.Error ?? "none"}, {result.Ms:0} ms");
            return !result.Stopped && result.Error == null;
        }
        finally
        {
            _pending.TryRemove(id, out _);
            Interlocked.CompareExchange(ref _speaking, 0, id);
            SpeechLevel?.Invoke(0);
        }
    }

    public void StopSpeaking()
    {
        if (_process is not { HasExited: false }) return;
        Trace.WriteLine("VoiceClient.StopSpeaking");
        Send(new VoiceRequest("stop"));
    }

    public Task<string> ListenAsync(IProgress<float>? level, CancellationToken token) =>
        ListenAsync(level, token, continuous: false);

    public async Task<string> ListenAsync(IProgress<float>? level, CancellationToken token, bool continuous, bool partial = false)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!CanHear) throw new InvalidOperationException("Speech recognition isn't installed yet.");
        await EnsureStartedAsync(null, token);
        if (!_canHearNow) throw new IOException("There's no microphone to listen with (or it couldn't be opened).");
        var id = Interlocked.Increment(ref _nextId);
        var done = Pending(id);
        _listenLevel = level;
        Interlocked.Exchange(ref _listening, id);
        Trace.WriteLine($"VoiceClient.ListenAsync enter: #{id}");
        Send(new VoiceRequest("listen", id, MaxSeconds: 45, SilenceSeconds: 1.3f, NoSpeechSeconds: continuous ? 45 : 8, Partial: partial));
        await using var cancel = token.Register(() => Send(new VoiceRequest("cancel")));
        try
        {
            var result = await done.Task.WaitAsync(token);
            Trace.WriteLine($"VoiceClient.ListenAsync exit: #{id} cancelled={result.Cancelled}, {result.Text?.Length ?? 0} characters, error={result.Error ?? "none"}, {result.Ms:0} ms");
            if (result.Cancelled) throw new OperationCanceledException(token);
            if (result.Error != null) throw new IOException(result.Error);
            return result.Text ?? "";
        }
        finally
        {
            _pending.TryRemove(id, out _);
            Interlocked.CompareExchange(ref _listening, 0, id);
            _listenLevel = null;
            level?.Report(0);
        }
    }

    public async Task<IReadOnlyList<(string Name, string Label)>> MicrophonesAsync(CancellationToken token)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await EnsureStartedAsync(null, token);
        if (_process is not { HasExited: false })
        {
            Trace.TraceWarning("VoiceClient.MicrophonesAsync: the helper isn't running");
            return [];
        }
        var id = Interlocked.Increment(ref _nextId);
        var done = Pending(id);
        Send(new VoiceRequest("devices", id));
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        var result = await done.Task.WaitAsync(timeout.Token);
        var microphones = (result.Text ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(line => line.Split('\t') is [var name, var label] ? (Name: name.Trim(), Label: label.Trim()) : (Name: line.Trim(), Label: line.Trim()))
            .Where(m => m.Name.Length > 0).ToList();
        Trace.WriteLine($"VoiceClient.MicrophonesAsync: {microphones.Count} microphone(s)");
        return microphones;
    }

    public void UseMicrophone(string? name)
    {
        _microphone = string.IsNullOrWhiteSpace(name) ? null : name;
        Trace.WriteLine($"VoiceClient.UseMicrophone: {_microphone ?? "the system default"}");
        Send(new VoiceRequest("mic", Text: _microphone));
    }

    public void SetVolume(float volume)
    {
        _volume = Math.Clamp(volume, 0f, 1f);
        Trace.WriteLine($"VoiceClient.SetVolume: {_volume:P0}");
        Send(new VoiceRequest("volume", Volume: _volume));
    }

    public void FinishListening()
    {
        if (Interlocked.Read(ref _listening) == 0) return;
        Trace.WriteLine("VoiceClient.FinishListening");
        Send(new VoiceRequest("finish"));
    }

    public void CancelListening() => Send(new VoiceRequest("cancel"));

    private float _rear;
    public void SetDirection(float pan, float rear)
    {
        _pan = float.IsFinite(pan) ? Math.Clamp(pan, -1f, 1f) : 0;
        _rear = float.IsFinite(rear) ? Math.Clamp(rear, 0f, 1f) : 0;
        Send(new VoiceRequest("pan", Pan: _pan, Rear: _rear));
    }

    public async Task ReloadAsync()
    {
        Trace.WriteLine("VoiceClient.ReloadAsync: the models changed; the helper restarts on next use");
        await _startGate.WaitAsync();
        try
        {
            StopHelper();
        }
        finally
        {
            _startGate.Release();
        }
    }

    private TaskCompletionSource<VoiceEvent> Pending(long id)
    {
        var done = new TaskCompletionSource<VoiceEvent>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = done;
        return done;
    }

    private async Task EnsureStartedAsync(string? accent, CancellationToken token)
    {
        await _startGate.WaitAsync(token);
        try
        {
            if (_process is { HasExited: false }) return;
            StopHelper();
            var watch = Stopwatch.StartNew();
            Trace.WriteLine($"VoiceClient.EnsureStartedAsync enter: speak model={CanSpeak}, hear model={CanHear}");
            var info = new ProcessStartInfo(_helper)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardInputEncoding = new UTF8Encoding(false), WorkingDirectory = Path.GetDirectoryName(_helper)!
            };
            info.ArgumentList.Add("--serve");
            if (CanSpeak)
            {
                info.ArgumentList.Add("--tts");
                info.ArgumentList.Add(VoiceModels.FolderOf(VoiceModels.Speech));
            }
            if (CanHear)
            {
                info.ArgumentList.Add("--stt");
                info.ArgumentList.Add(VoiceModels.FolderOf(VoiceModels.Hearing));
            }
            if (VoiceModels.IsInstalled(VoiceModels.Detector))
            {
                info.ArgumentList.Add("--vad");
                info.ArgumentList.Add(VoiceModels.DetectorFile);
            }
            var process = Process.Start(info) ?? throw new IOException("The voice helper couldn't start.");
            _process = process;
            Trace.WriteLine($"VoiceClient.EnsureStartedAsync: helper pid {process.Id} started");
            var log = Path.Combine(Settings.Root, "voice.log");
            _ = Task.Run(async () =>
            {
                // The helper's own log: models loading, devices, timings, failures.
                try { while (await process.StandardError.ReadLineAsync() is { } line) await File.AppendAllTextAsync(log, line + Environment.NewLine); }
                catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
                {
                    Trace.WriteLine($"VoiceClient: stopped copying the helper's log: {ex.GetType().Name}: {ex.Message}");
                }
            });
            var ready = Pending(0);
            _reader = Task.Run(() => ReadEventsAsync(process));
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            var first = await ready.Task.WaitAsync(timeout.Token);
            _canSpeakNow = first.Speak;
            _canHearNow = first.Hear;
            _canBargeInNow = first.BargeIn;
            // The microphone picked (a restarted helper starts with the default).
            if (_microphone != null) Send(new VoiceRequest("mic", Text: _microphone));
            // And the volume (a restarted helper plays at full).
            if (_volume < 1f) Send(new VoiceRequest("volume", Volume: _volume));
            Send(new VoiceRequest("pan", Pan: _pan, Rear: _rear));
            // Load the models now, in the helper's background, so the first words aren't slow.
            Send(new VoiceRequest("warm", Accent: accent ?? VoiceCatalog.Find(null).Accent));
            Trace.WriteLine($"VoiceClient.EnsureStartedAsync exit: ready in {watch.ElapsedMilliseconds} ms (speak={_canSpeakNow}, hear={_canHearNow}, barge-in={_canBargeInNow})");
        }
        catch
        {
            StopHelper();
            throw;
        }
        finally
        {
            _startGate.Release();
        }
    }

    private async Task ReadEventsAsync(Process process)
    {
        try
        {
            while (await process.StandardOutput.ReadLineAsync() is { } line)
            {
                VoiceEvent? message;
                try
                {
                    message = JsonSerializer.Deserialize(line, VoiceJson.Default.VoiceEvent);
                }
                catch (JsonException ex)
                {
                    Trace.TraceWarning("VoiceClient.ReadEventsAsync: not an event: " + ex.Message);
                    continue;
                }
                if (message != null) Dispatch(message);
            }
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            Trace.WriteLine($"VoiceClient.ReadEventsAsync: {ex.GetType().Name}: {ex.Message}");
        }
        Trace.WriteLine("VoiceClient.ReadEventsAsync: the helper closed its output; failing anything still waiting");
        foreach (var id in _pending.Keys.ToList())
        {
            if (_pending.TryRemove(id, out var waiting)) waiting.TrySetResult(new VoiceEvent("gone", id, Error: "The voice helper stopped (see voice.log)."));
        }
    }

    private void Dispatch(VoiceEvent message)
    {
        switch (message.Event)
        {
            case "ready":
                if (_pending.TryRemove(0, out var ready)) ready.TrySetResult(message);
                return;
            case "level":
                if (message.Id == Interlocked.Read(ref _speaking)) SpeechLevel?.Invoke(message.Level);
                else if (message.Id == Interlocked.Read(ref _listening))
                {
                    if (message.Id < 0) _talkedOver?.ReportLevel(message.Level);
                    else _listenLevel?.Report(message.Level);
                }
                return;
            case "barge":
                OnBarge(message.Id);
                return;
            case "partial":
                if (message.Text is { Length: > 0 } partial &&
                    message.Id == Interlocked.Read(ref _listening) && _pending.ContainsKey(message.Id))
                    PartialWords?.Invoke(partial);
                return;
            case "spoken":
            case "heard":
            case "devices":
                if (_pending.TryRemove(message.Id, out var done)) done.TrySetResult(message);
                return;
            default:
                return;
        }
    }

    /// <summary>
    /// The person talked over speak request <paramref name="id"/>: their words will come as heard −id. Listening is
    /// marked as on (so FinishListening works), and <see cref="TalkedOver"/> is raised with the words to come.
    /// </summary>
    private void OnBarge(long id)
    {
        var listenId = -id;
        Trace.WriteLine($"VoiceClient.OnBarge: the person talked over #{id}; listening as #{listenId}");
        var heard = Pending(listenId);
        Interlocked.Exchange(ref _listening, listenId);
        var words = heard.Task.ContinueWith(task =>
        {
            Interlocked.CompareExchange(ref _listening, 0, listenId);
            _talkedOver = null;
            var result = task.Result;
            Trace.WriteLine($"VoiceClient.OnBarge: #{listenId} heard {result.Text?.Length ?? 0} characters, cancelled={result.Cancelled}, error={result.Error ?? "none"}, {result.Ms:0} ms");
            if (result.Cancelled) throw new OperationCanceledException("Listening was cancelled.");
            if (result.Error != null) throw new IOException(result.Error);
            return result.Text ?? "";
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        var talkedOver = new TalkedOver(words);
        _talkedOver = talkedOver;
        TalkedOver?.Invoke(talkedOver);
    }

    private void Send(VoiceRequest request)
    {
        var process = _process;
        if (process is not { HasExited: false }) return;
        var line = JsonSerializer.Serialize(request, VoiceJson.Default.VoiceRequest);
        lock (_writeLock)
        {
            try
            {
                process.StandardInput.WriteLine(line);
                process.StandardInput.Flush();
            }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
            {
                Trace.TraceWarning($"VoiceClient.Send: {request.Op} not sent: {ex.Message}");
            }
        }
    }

    private void StopHelper()
    {
        var process = _process;
        if (process == null) return;
        _process = null;
        Trace.WriteLine($"VoiceClient.StopHelper enter: exited={process.HasExited}");
        try
        {
            if (!process.HasExited)
            {
                process.StandardInput.Close();
                if (!process.WaitForExit(3000))
                {
                    Trace.TraceWarning("VoiceClient.StopHelper: the helper didn't exit within 3s of closing its input; killing it");
                    process.Kill(entireProcessTree: true);
                }
            }
        }
        catch (Exception ex) when (ex is InvalidOperationException or IOException or System.ComponentModel.Win32Exception)
        {
            Trace.WriteLine($"VoiceClient.StopHelper: {ex.Message}");
        }
        process.Dispose();
        Trace.WriteLine("VoiceClient.StopHelper exit");
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        Trace.WriteLine("VoiceClient.DisposeAsync enter");
        await _startGate.WaitAsync();
        try
        {
            StopHelper();
            if (_reader != null) await _reader.WaitAsync(TimeSpan.FromSeconds(2)).ContinueWith(_ => { }, TaskScheduler.Default);
        }
        finally
        {
            _startGate.Release();
        }
        Trace.WriteLine("VoiceClient.DisposeAsync exit");
    }
}
