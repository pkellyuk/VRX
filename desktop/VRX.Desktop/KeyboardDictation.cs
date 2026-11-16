using System.Globalization;
using System.IO;
using System.Speech.AudioFormat;
using System.Speech.Recognition;
using NAudio.Wave;

namespace VRX.Desktop;

// Uses the installed Windows desktop speech engine. Microphone audio stays in a
// bounded, transient PCM buffer and is never written to a recording file.
internal sealed class KeyboardDictation : IDisposable
{
    internal sealed record Microphone(int Device, string Name);
    private readonly object gate = new();
    private SpeechRecognitionEngine? engine;
    private WaveInEvent? capture;
    private BlockingPcmStream? input;
    private int generation;
    private bool disposed;
    internal event Action<string>? Recognized;
    internal event Action<string>? Hypothesized;
    internal event Action<string>? Failed;
    internal bool Listening => Volatile.Read(ref engine) != null;
    internal static IReadOnlyList<Microphone> Microphones()
    {
        var result = new List<Microphone> { new(-1, Loc.Get("DictationDefaultMic")) };
        for (int device = 0; device < WaveInEvent.DeviceCount; device++)
            result.Add(new(device, WaveInEvent.GetCapabilities(device).ProductName));
        return result;
    }
    internal string Start(Microphone microphone)
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            StopCore();
            var devices = Microphones();
            if (devices.Count < 2) throw new InvalidOperationException(Loc.Get("DictationNoMicrophones"));
            if (microphone.Device >= 0)
                microphone = devices.FirstOrDefault(m => m.Device == microphone.Device && m.Name == microphone.Name)
                    ?? devices.FirstOrDefault(m => m.Device >= 0 && m.Name == microphone.Name)
                    ?? throw new InvalidOperationException(Loc.Get("DictationMicRemoved"));
            var installed = SpeechRecognitionEngine.InstalledRecognizers();
            var recognizer = installed.FirstOrDefault(r => r.Culture.Name == CultureInfo.CurrentUICulture.Name)
                ?? installed.FirstOrDefault(r => r.Culture.TwoLetterISOLanguageName == CultureInfo.CurrentUICulture.TwoLetterISOLanguageName)
                ?? installed.FirstOrDefault();
            if (recognizer == null) throw new InvalidOperationException(Loc.Get("DictationNoRecognizer"));
            int epoch = Interlocked.Increment(ref generation);
            try
            {
                input = new BlockingPcmStream();
                engine = new SpeechRecognitionEngine(recognizer);
                engine.LoadGrammar(new DictationGrammar());
                engine.SetInputToAudioStream(input, new SpeechAudioFormatInfo(16000, AudioBitsPerSample.Sixteen, AudioChannel.Mono));
                engine.SpeechRecognized += (_, e) => { if (epoch == generation && e.Result.Confidence >= .2f) Recognized?.Invoke(e.Result.Text); };
                engine.SpeechHypothesized += (_, e) => { if (epoch == generation) Hypothesized?.Invoke(e.Result.Text); };
                engine.RecognizeCompleted += (_, e) => { if (epoch == generation && e.Error != null) Failed?.Invoke(e.Error.Message); };
                capture = new WaveInEvent { DeviceNumber = microphone.Device, WaveFormat = new WaveFormat(16000, 16, 1), BufferMilliseconds = 50, NumberOfBuffers = 3 };
                var stream = input;
                capture.DataAvailable += (_, e) =>
                {
                    if (epoch == generation && !stream.Append(e.Buffer, e.BytesRecorded)) Failed?.Invoke(Loc.Get("DictationOverrun"));
                };
                capture.RecordingStopped += (_, e) => { if (epoch == generation && e.Exception != null) Failed?.Invoke(e.Exception.Message); };
                engine.RecognizeAsync(RecognizeMode.Multiple);
                capture.StartRecording();
                return recognizer.Culture.DisplayName;
            }
            catch { StopCore(); throw; }
        }
    }
    internal void Stop() { lock (gate) StopCore(); }
    private void StopCore()
    {
        Interlocked.Increment(ref generation);
        var oldCapture = capture; capture = null;
        var oldEngine = engine; engine = null;
        input?.Complete(); // Unblock the recognizer before cancellation/disposal.
        if (oldCapture != null)
        {
            try { oldCapture.StopRecording(); } catch (NAudio.MmException) { }
            try { oldCapture.Dispose(); } catch (NAudio.MmException) { }
        }
        if (oldEngine != null)
        {
            try { oldEngine.RecognizeAsyncCancel(); } catch (InvalidOperationException) { }
            finally { try { oldEngine.Dispose(); } catch (System.Runtime.InteropServices.COMException) { } }
        }
        input?.Dispose(); input = null;
    }
    public void Dispose() { lock (gate) { if (disposed) return; disposed = true; StopCore(); } }

    internal static void SmokeTest(string output)
    {
        using var stream = new BlockingPcmStream();
        byte[] bytes = new byte[16];
        var read = Task.Run(() => stream.Read(bytes, 0, bytes.Length));
        if (read.Wait(30)) throw new Exception("Dictation input must wait for microphone audio rather than returning premature EOF");
        stream.Append(Enumerable.Range(0,8).Select(n => (byte)n).ToArray(),8);
        if (read.Wait(30)) throw new Exception("A partial microphone packet must not become a short SAPI read");
        if (!stream.Append(Enumerable.Range(8,8).Select(n => (byte)n).ToArray(),8) || !read.Wait(1000) || read.Result != 16 || bytes[15] != 15)
            throw new Exception("Dictation PCM stream must preserve captured audio bytes");
        var cancelledRead = Task.Run(() => stream.Read(bytes,0,16)); stream.Complete();
        if (!cancelledRead.Wait(1000) || cancelledRead.Result != 0) throw new Exception("Stopping dictation must unblock the recognizer");
        var english = SpeechRecognitionEngine.InstalledRecognizers().FirstOrDefault(r => r.Culture.TwoLetterISOLanguageName == "en");
        if (english != null)
        {
            using var synth = new System.Speech.Synthesis.SpeechSynthesizer();
            using var spoken = new MemoryStream();
            var format = new SpeechAudioFormatInfo(16000, AudioBitsPerSample.Sixteen, AudioChannel.Mono);
            synth.SetOutputToAudioStream(spoken, format); synth.Speak("hello world");
            using var syntheticInput = new BlockingPcmStream();
            byte[] audio = spoken.ToArray();
            // Deliver microphone-sized packets over time. A preloaded short clip
            // cannot expose SAPI treating an undersized read as end-of-input.
            var producer = Task.Run(async () =>
            {
                for (int at = 0; at < audio.Length; at += 1600)
                {
                    var packet = audio.AsSpan(at, Math.Min(1600, audio.Length - at)).ToArray();
                    if (!syntheticInput.Append(packet, packet.Length)) throw new Exception("Dictation fixture exceeds bounded input");
                    await Task.Delay(25);
                }
                syntheticInput.Complete();
            });
            using var recognition = new SpeechRecognitionEngine(english);
            recognition.LoadGrammar(new DictationGrammar()); recognition.SetInputToAudioStream(syntheticInput,format);
            var result = recognition.Recognize(TimeSpan.FromSeconds(6));
            producer.GetAwaiter().GetResult();
            if (result == null || string.IsNullOrWhiteSpace(result.Text)) throw new Exception("Windows dictation did not recognize the in-memory speech fixture");
            File.WriteAllText(Path.Combine(output,"dictation-pipeline.txt"), "PASS: in-memory Windows speech recognized: " + result.Text);
        }
        File.WriteAllLines(Path.Combine(output,"dictation-devices.txt"), Microphones().Select(m => m.Name)
            .Concat(SpeechRecognitionEngine.InstalledRecognizers().Select(r => r.Culture.DisplayName)));
    }

    internal sealed class BlockingPcmStream : Stream
    {
        private readonly object sync = new();
        private readonly byte[] ring = new byte[96000];
        private int read, length;
        private long consumed;
        private bool complete;
        internal bool Append(byte[] bytes, int count)
        {
            lock (sync)
            {
                if (complete) return true;
                if (count > ring.Length - length) return false;
                for (int i = 0; i < count; i++) ring[(read + length + i) % ring.Length] = bytes[i];
                length += count; Monitor.PulseAll(sync); return true;
            }
        }
        internal void Complete() { lock (sync) { complete = true; Monitor.PulseAll(sync); } }
        public override int Read(byte[] buffer, int offset, int count)
        {
            ArgumentNullException.ThrowIfNull(buffer);
            if (offset < 0 || count < 0 || count > buffer.Length - offset) throw new ArgumentOutOfRangeException();
            if (count == 0) return 0;
            lock (sync)
            {
                int total = 0;
                // SAPI treats short reads as EOF. Keep collecting capture packets
                // until the requested block is full or capture has actually ended.
                while (total < count)
                {
                    while (length == 0 && !complete) Monitor.Wait(sync);
                    if (length == 0) break;
                    int take = Math.Min(count - total, length);
                    for (int i = 0; i < take; i++) buffer[offset + total + i] = ring[(read + i) % ring.Length];
                    read = (read + take) % ring.Length; length -= take; consumed += take; total += take;
                }
                return total;
            }
        }
        protected override void Dispose(bool disposing) { lock (sync) { complete = true; length = 0; Array.Clear(ring); Monitor.PulseAll(sync); } base.Dispose(disposing); }
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        // SAPI queries Length and the current cursor even for a live, non-seekable
        // source. Advertise an open-ended stream; Complete supplies its actual EOF.
        public override long Length => long.MaxValue;
        public override long Position { get { lock (sync) return consumed; } set => Seek(value, SeekOrigin.Begin); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin)
        {
            lock (sync)
            {
                if ((origin == SeekOrigin.Current && offset == 0) || (origin == SeekOrigin.Begin && offset == consumed)) return consumed;
                throw new NotSupportedException("Live microphone audio cannot be rewound");
            }
        }
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
