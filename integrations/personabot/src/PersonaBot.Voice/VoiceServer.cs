using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;

namespace PersonaBot.Voice;

/// <summary>
/// The helper's serve mode: reads <see cref="VoiceRequest"/> lines from stdin and writes <see cref="VoiceEvent"/>
/// lines to stdout (VoiceProtocol.cs). Speech runs on its own thread, one request at a time, a new one cutting off
/// the last. The microphone's blocks go to a worker thread: while listening they're recorded and turned into words;
/// while a speak request that may be talked over plays (barge-in), they go through the echo canceller and barge-in
/// detector, and when the person talks over the bot its speech stops and what they say is heard. Ends when stdin
/// closes.
/// </summary>
public sealed class VoiceServer : IDisposable
{
    private readonly Speaker? _speaker;
    private readonly Hearer? _hearer;
    private readonly BargeInDetector? _bargeIn;
    private readonly ISpeechDetector? _speechDetector;
    private readonly Action<string> _log;
    private readonly TextWriter _out;
    private readonly object _writeLock = new();
    private readonly BlockingCollection<VoiceRequest> _speech = new();
    private readonly BlockingCollection<(float[] Block, double Time)> _mic = new();
    private readonly AudioOut _audioOut;
    private readonly AudioIn _audioIn;
    private readonly object _listenLock = new();
    private readonly object _hearLock = new();
    // The barge-in detector is used by the microphone worker and restarted by the speech thread.
    private readonly object _echoLock = new();
    private CancellationTokenSource _speaking = new();
    private Listening? _listening;
    private Watch? _watch;
    private Resampler? _toMic;
    private int _partialInFlight;

    /// <summary>One turn at the microphone: what's been recorded and when to stop.</summary>
    private sealed class Listening(long id, Endpointer endpointer, bool throughEcho, bool partial)
    {
        public long Id { get; } = id;
        public Endpointer Endpointer { get; } = endpointer;
        public List<float> Samples { get; } = new(AudioIn.SampleRate * 10);
        public Stopwatch Watch { get; } = Stopwatch.StartNew();
        /// <summary>It began by talking over the bot: the microphone still goes through the echo canceller.</summary>
        public bool ThroughEcho { get; } = throughEcho;
        public bool Partial { get; } = partial;
        public float NextPartialAt;
        public string? LastPartial;
        public int Blocks;
        public int Ended;
    }

    /// <summary>A speak request being watched for the person talking over it.</summary>
    private sealed class Watch(long id, bool partial)
    {
        public long Id { get; } = id;
        public bool Partial { get; } = partial;
        public Stopwatch Since { get; } = Stopwatch.StartNew();
        public int Blocks;
        public int BargedIn;
    }

    public VoiceServer(Speaker? speaker, Hearer? hearer, ISpeechDetector? detector, TextWriter output, Action<string> log)
    {
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(log);
        _speaker = speaker;
        _hearer = hearer;
        _speechDetector = detector;
        _bargeIn = detector == null ? null : new BargeInDetector(new EchoCanceller(), detector);
        _out = output;
        _log = log;
        _audioOut = new AudioOut(log);
        _audioIn = new AudioIn(log);
        _audioIn.Samples += OnSamples;
        _audioOut.Played += OnPlayed;
    }

    /// <summary>Whether it can listen while it speaks: a voice, a recognizer, the speech detector and a microphone.</summary>
    private bool CanBargeIn => _speaker != null && _hearer != null && _bargeIn != null && AudioIn.HasMicrophone;

    /// <summary>Serves requests from <paramref name="input"/> until it closes.</summary>
    public void Run(TextReader input)
    {
        ArgumentNullException.ThrowIfNull(input);
        var hear = _hearer != null && AudioIn.HasMicrophone;
        _log($"VoiceServer.Run enter: speak={_speaker != null}, hear={hear} (model={_hearer != null}, microphone={AudioIn.HasMicrophone}), barge-in={CanBargeIn} (detector={_bargeIn != null})");
        var speechThread = new Thread(SpeechLoop) { IsBackground = true, Name = "speech" };
        speechThread.Start();
        var micThread = new Thread(MicLoop) { IsBackground = true, Name = "microphone" };
        micThread.Start();
        Send(new VoiceEvent("ready", Speak: _speaker != null, Hear: hear, BargeIn: CanBargeIn));
        while (input.ReadLine() is { } line)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            VoiceRequest? request;
            try
            {
                request = JsonSerializer.Deserialize(line, VoiceJson.Default.VoiceRequest);
            }
            catch (JsonException ex)
            {
                _log("VoiceServer.Run: not a request: " + ex.Message);
                continue;
            }
            if (request == null) continue;
            Handle(request);
        }
        _log("VoiceServer.Run: input closed; stopping");
        _speech.CompleteAdding();
        _speaking.Cancel();
        _audioOut.Clear();
        EndListening(cancel: true);
        StopWatching("the app closed");
        _mic.CompleteAdding();
        speechThread.Join(2000);
        micThread.Join(2000);
        _log("VoiceServer.Run exit");
    }

    private void Handle(VoiceRequest request)
    {
        _log($"VoiceServer.Handle: {request.Op} #{request.Id}{(request.Listen ? " (may be talked over)" : "")}");
        switch (request.Op)
        {
            case "speak":
                if (_speaker == null)
                {
                    Send(new VoiceEvent("spoken", request.Id, Error: "No voice model is installed."));
                    return;
                }
                StopSpeaking();
                _speech.Add(request);
                return;
            case "stop":
                StopSpeaking();
                return;
            case "warm":
                ThreadPool.QueueUserWorkItem(_ => Warm(request.Accent));
                return;
            case "listen":
                StartListening(request);
                return;
            case "finish":
                EndListening(cancel: false);
                return;
            case "cancel":
                EndListening(cancel: true);
                return;
            case "devices":
                // Microphones plugged in since the helper started only show after PortAudio re-reads its devices.
                AudioClock.RefreshDevices(() => _audioIn.IsOpen || _audioOut.IsOpen || _speech.Count > 0, _log);
                var microphones = AudioIn.Microphones();
                _log($"VoiceServer.Handle: {microphones.Count} microphone(s): {string.Join(" | ", microphones.Select(m => m.Label))}");
                // One per line: the name to pick it by, a tab, and its full name to show.
                Send(new VoiceEvent("devices", request.Id, Text: string.Join("\n", microphones.Select(m => m.Name + "\t" + m.Label))));
                return;
            case "volume":
                _audioOut.Gain = request.Volume ?? 1f;
                _log($"VoiceServer.Handle: volume {request.Volume ?? 1f:P0}");
                return;
            case "pan":
                _audioOut.SetDirection(request.Pan, request.Rear);
                return;
            case "mic":
                _audioIn.Preferred = string.IsNullOrWhiteSpace(request.Text) ? null : request.Text;
                _log($"VoiceServer.Handle: microphone {(_audioIn.Preferred ?? "the system default")} (from the next time it opens)");
                return;
            default:
                _log("VoiceServer.Handle: unknown op " + request.Op);
                return;
        }
    }

    private void Warm(string? accent)
    {
        var watch = Stopwatch.StartNew();
        try
        {
            _speaker?.Warm(accent);
            lock (_hearLock)
            {
                _hearer?.Warm();
            }
            _log($"VoiceServer.Warm: ready in {watch.ElapsedMilliseconds} ms");
        }
        catch (Exception ex)
        {
            _log("VoiceServer.Warm failed: " + ex);
        }
    }

    private void StopSpeaking()
    {
        _speaking.Cancel();
        while (_speech.TryTake(out var dropped)) Send(new VoiceEvent("spoken", dropped.Id, Stopped: true));
        _audioOut.Clear();
    }

    private void SpeechLoop()
    {
        foreach (var request in _speech.GetConsumingEnumerable())
        {
            _speaking = new CancellationTokenSource();
            var token = _speaking.Token;
            var watch = Stopwatch.StartNew();
            string? error = null;
            try
            {
                _audioOut.Begin(_speaker!.SampleRate);
                if (request.Listen) StartWatching(request.Id, request.Partial);
                using var levels = new Timer(_ => Send(new VoiceEvent("level", request.Id, Level: _audioOut.Level)), null, 50, 50);
                _speaker.Say(request.Text ?? "", request.Speaker, request.Accent, request.Speed, _audioOut.Add, token);
                _audioOut.End();
                while (!_audioOut.Drained && !token.IsCancellationRequested) Thread.Sleep(20);
            }
            catch (Exception ex)
            {
                _log("VoiceServer.SpeechLoop: speaking failed: " + ex);
                error = ex.Message;
            }
            var stopped = token.IsCancellationRequested;
            if (stopped) _audioOut.Clear();
            StopWatching(stopped ? "the speech was stopped" : "the speech finished");
            if (_speech.Count == 0) _audioOut.Close();
            Send(new VoiceEvent("level", request.Id));
            Send(new VoiceEvent("spoken", request.Id, Stopped: stopped, Error: error, Ms: watch.Elapsed.TotalMilliseconds));
        }
    }

    /// <summary>Opens the microphone to listen for the person talking over speak request <paramref name="id"/>.</summary>
    private void StartWatching(long id, bool partial)
    {
        if (!CanBargeIn)
        {
            _log($"VoiceServer.StartWatching: #{id} can't be talked over (voice={_speaker != null}, recognizer={_hearer != null}, detector={_bargeIn != null}, microphone={AudioIn.HasMicrophone})");
            return;
        }
        lock (_listenLock)
        {
            if (_listening != null)
            {
                _log($"VoiceServer.StartWatching: #{id} not watched: the microphone is listening for #{_listening.Id}");
                return;
            }
            var watch = new Watch(id, partial);
            lock (_echoLock)
            {
                _bargeIn!.BeginUtterance();
            }
            _toMic = new Resampler(_speaker!.SampleRate, EchoCanceller.Rate);
            _watch = watch;
            try
            {
                if (!_audioIn.IsOpen) _audioIn.Start();
            }
            catch (Exception ex)
            {
                _watch = null;
                _log("VoiceServer.StartWatching: the microphone wouldn't open, so no barge-in this time: " + ex.Message);
                return;
            }
            _log($"VoiceServer.StartWatching: #{id} listening for the person talking over it (echo delay {_bargeIn.Echo.Delay / 16.0:0} ms, " +
                 $"{(_bargeIn.Echo.Aligned ? "found" : "not yet found")}; coupling {10 * Math.Log10(_bargeIn.Coupling):0} dB)");
        }
    }

    /// <summary>Stops watching (the speech ended without being talked over); the microphone closes unless it's listening.</summary>
    private void StopWatching(string why)
    {
        lock (_listenLock)
        {
            var watch = _watch;
            if (watch == null) return;
            _watch = null;
            if (Volatile.Read(ref watch.BargedIn) != 0) return;
            if (_listening == null) _audioIn.Stop();
            _log($"VoiceServer.StopWatching: #{watch.Id} not talked over ({why}) after {watch.Since.ElapsedMilliseconds} ms: {watch.Blocks} blocks, " +
                 $"{_bargeIn!.Frames} frames, {_bargeIn.PersonFrames} sounded like the person; echo delay {_bargeIn.Echo.Delay / 16.0:0} ms " +
                 $"({(_bargeIn.Echo.Aligned ? $"found, {_bargeIn.Echo.DelayUpdates} update(s)" : "not found")}), coupling {10 * Math.Log10(_bargeIn.Coupling):0} dB");
        }
    }

    private void OnPlayed(float[] block, double time, int rate)
    {
        // What's playing, for the echo canceller: only while a speak request is watched.
        if (_watch == null || _bargeIn == null || _toMic is not { } resampler) return;
        if (resampler.FromRate != rate) return;
        var samples = resampler.Process(block);
        _mic.TryAdd((samples, -time - 1));
    }

    private void StartListening(VoiceRequest request)
    {
        if (_hearer == null)
        {
            Send(new VoiceEvent("heard", request.Id, Error: "No speech recognition model is installed."));
            return;
        }
        StopSpeaking();
        StopWatching("the person pressed the microphone");
        EndListening(cancel: true);
        var listening = new Listening(request.Id, new Endpointer(request.SilenceSeconds, request.MaxSeconds, request.NoSpeechSeconds),
            throughEcho: false, partial: request.Partial);
        lock (_echoLock) _speechDetector?.Reset();
        lock (_listenLock)
        {
            _listening = listening;
        }
        try
        {
            _audioIn.Start();
        }
        catch (Exception ex)
        {
            _log("VoiceServer.StartListening: the microphone wouldn't open: " + ex);
            lock (_listenLock)
            {
                _listening = null;
            }
            Send(new VoiceEvent("heard", request.Id, Error: "The microphone wouldn't open: " + ex.Message));
            return;
        }
        Send(new VoiceEvent("listening", request.Id));
    }

    private void OnSamples(float[] block, double time)
    {
        // PortAudio's thread: hand over and return.
        if (!_mic.IsAddingCompleted) _mic.TryAdd((block, time));
    }

    /// <summary>
    /// The microphone worker: what's playing goes to the echo canceller (marked by a negative time), and each
    /// microphone block goes to the barge-in watch or the listening turn.
    /// </summary>
    private void MicLoop()
    {
        foreach (var (block, time) in _mic.GetConsumingEnumerable())
        {
            try
            {
                if (time < 0)
                {
                    if (_watch == null && _listening is not { ThroughEcho: true }) continue;
                    lock (_echoLock)
                    {
                        _bargeIn!.AddReference(block, -time - 1);
                    }
                    continue;
                }
                var watch = _watch;
                if (watch != null && _bargeIn != null && Volatile.Read(ref watch.BargedIn) == 0)
                {
                    watch.Blocks++;
                    bool barged;
                    lock (_echoLock)
                    {
                        _bargeIn.Process(block, time);
                        barged = _bargeIn.BargedIn;
                    }
                    if (barged) BargeIn(watch);
                    continue;
                }
                var listening = _listening;
                if (listening == null || Volatile.Read(ref listening.Ended) != 0) continue;
                if (!listening.ThroughEcho || _bargeIn == null)
                {
                    Hear(listening, block);
                    continue;
                }
                float[] residual;
                lock (_echoLock)
                {
                    residual = _bargeIn.Process(block, time);
                }
                Hear(listening, residual);
            }
            catch (Exception ex)
            {
                // One bad block never stops the microphone.
                _log("VoiceServer.MicLoop: " + ex);
            }
        }
    }

    /// <summary>
    /// The person talked over speak request <paramref name="watch"/>: speech stops, and what they say is listened to
    /// (from a little before they started), through the echo canceller as the bot's last words die away.
    /// </summary>
    private void BargeIn(Watch watch)
    {
        if (Interlocked.Exchange(ref watch.BargedIn, 1) != 0) return;
        _log($"VoiceServer.BargeIn: #{watch.Id} talked over after {watch.Since.ElapsedMilliseconds} ms: {_bargeIn!.Reason}");
        var endpointer = new Endpointer(1.3f, 45f);
        endpointer.MarkSpeech();
        var listening = new Listening(-watch.Id, endpointer, throughEcho: true, partial: watch.Partial);
        float[] preroll;
        lock (_echoLock)
        {
            preroll = _bargeIn.TakePreroll();
        }
        listening.Samples.AddRange(preroll);
        lock (_listenLock)
        {
            _listening = listening;
            _watch = null;
        }
        Send(new VoiceEvent("barge", watch.Id));
        // The bot stops at once; the microphone stays open for the person.
        StopSpeaking();
        _log($"VoiceServer.BargeIn: listening as #{listening.Id}, {preroll.Length / (double)AudioIn.SampleRate:0.00}s of what they said kept from before");
    }

    private void Hear(Listening listening, float[] block)
    {
        bool? voiced;
        lock (_echoLock) voiced = _speechDetector?.IsSpeech(block);
        lock (listening)
        {
            if (Volatile.Read(ref listening.Ended) != 0) return;
            listening.Samples.AddRange(block);
            var endpointer = listening.Endpointer;
            endpointer.Add(block, AudioIn.SampleRate, voiced);
            if (++listening.Blocks % 2 == 0) Send(new VoiceEvent("level", listening.Id, Level: endpointer.Level));
            if (!endpointer.Done)
            {
                if (listening.Partial && endpointer.HeardSpeech && endpointer.Elapsed - endpointer.SpeechStartedAt >= .6f &&
                    endpointer.Elapsed >= listening.NextPartialAt)
                {
                    listening.NextPartialAt = endpointer.Elapsed + .9f;
                    QueuePartial(listening, endpointer.SpeechStartedAt);
                }
                return;
            }
        }
        _log($"VoiceServer.Hear: stopping #{listening.Id} after {listening.Endpointer.Elapsed:0.0}s ({listening.Endpointer.Reason})");
        EndListening(cancel: false, expected: listening);
    }

    private void QueuePartial(Listening listening, float speechStart)
    {
        if (Interlocked.CompareExchange(ref _partialInFlight, 1, 0) != 0) return;
        var samples = listening.Samples.ToArray();
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                if (Volatile.Read(ref listening.Ended) != 0 || !Monitor.TryEnter(_hearLock)) return;
                try
                {
                    if (Volatile.Read(ref listening.Ended) != 0) return;
                    string text = _hearer!.Transcribe(
                        SpeechLevels.PrepareUtterance(samples, AudioIn.SampleRate, speechStart), AudioIn.SampleRate);
                    if (Volatile.Read(ref listening.Ended) == 0 && text.Length > 0 && text != listening.LastPartial)
                    {
                        listening.LastPartial = text;
                        Send(new VoiceEvent("partial", listening.Id, Text: text));
                    }
                }
                finally { Monitor.Exit(_hearLock); }
            }
            catch (Exception ex) { _log("VoiceServer.QueuePartial: " + ex.GetType().Name); }
            finally { Volatile.Write(ref _partialInFlight, 0); }
        });
    }

    private void EndListening(bool cancel, Listening? expected = null)
    {
        Listening? listening;
        lock (_listenLock)
        {
            listening = _listening;
            if (listening == null || (expected != null && !ReferenceEquals(listening, expected)) ||
                Interlocked.Exchange(ref listening.Ended, 1) != 0) return;
            _listening = null;
            if (_watch == null) _audioIn.Stop();
        }
        Send(new VoiceEvent("level", listening.Id));
        if (cancel)
        {
            _log($"VoiceServer.EndListening: #{listening.Id} cancelled after {listening.Watch.ElapsedMilliseconds} ms");
            Send(new VoiceEvent("heard", listening.Id, Cancelled: true));
            return;
        }
        float[] samples;
        bool heardSpeech;
        float speechStart;
        lock (listening)
        {
            samples = listening.Samples.ToArray();
            heardSpeech = listening.Endpointer.HeardSpeech;
            speechStart = listening.Endpointer.SpeechStartedAt;
        }
        ThreadPool.QueueUserWorkItem(_ =>
        {
            var watch = Stopwatch.StartNew();
            try
            {
                string text;
                lock (_hearLock)
                {
                    text = heardSpeech ? _hearer!.Transcribe(
                        SpeechLevels.PrepareUtterance(samples, AudioIn.SampleRate, speechStart), AudioIn.SampleRate) : "";
                }
                _log($"VoiceServer.EndListening: #{listening.Id} heard {text.Length} characters from {samples.Length / (double)AudioIn.SampleRate:0.0}s (speech={heardSpeech})");
                Send(new VoiceEvent("heard", listening.Id, Text: text, Ms: watch.Elapsed.TotalMilliseconds));
            }
            catch (Exception ex)
            {
                _log("VoiceServer.EndListening: transcribing failed: " + ex);
                Send(new VoiceEvent("heard", listening.Id, Error: ex.Message));
            }
        });
    }

    private void Send(VoiceEvent message)
    {
        var line = JsonSerializer.Serialize(message, VoiceJson.Default.VoiceEvent);
        lock (_writeLock)
        {
            try
            {
                _out.WriteLine(line);
                _out.Flush();
            }
            catch (IOException ex)
            {
                _log("VoiceServer.Send: the app has gone: " + ex.Message);
            }
        }
    }

    public void Dispose()
    {
        _audioIn.Samples -= OnSamples;
        _audioOut.Played -= OnPlayed;
        _audioIn.Dispose();
        _audioOut.Dispose();
        _speech.Dispose();
        _mic.Dispose();
        _speaking.Dispose();
    }
}
