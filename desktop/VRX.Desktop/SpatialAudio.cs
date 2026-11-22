using System.Runtime.InteropServices;
using CefSharp;
using CefSharp.Enums;
using CefSharp.Handler;
using CefSharp.Structs;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using VRX.Audio;

namespace VRX.Desktop;

internal sealed record AudioDirection(float X, float Y, float Z);

// Render a moving point source for headphones. A short interaural delay and
// head shadow supplement equal-power panning. With no headset pose, play the
// original stereo signal or centered speech.
internal sealed class SpatialSamples(ISampleProvider source, Func<AudioDirection?> direction, bool positionalOnly = false,
    bool spatial = true, Func<float>? readGain = null) : ISampleProvider
{
    public WaveFormat WaveFormat { get; } = WaveFormat.CreateIeeeFloatWaveFormat(source.WaveFormat.SampleRate, 2);
    private readonly PointSourceAudio pointSource = new();
    private float[] input = [];
    private float enabled;

    public int Read(float[] buffer, int offset, int count)
    {
        int channels = source.WaveFormat.Channels;
        int wanted = count / 2 * channels;
        if (input.Length < wanted) input = new float[wanted];
        int frames = source.Read(input, 0, wanted) / channels;
        var location = direction();
        if (positionalOnly && location == null)
        {
            Array.Clear(buffer, offset, frames * 2);
            return frames * 2;
        }
        float distance = location == null ? 0 : MathF.Sqrt(location.X * location.X + location.Y * location.Y + location.Z * location.Z);
        float targetPan = distance > .01f ? location!.X / distance : 0;
        float targetRear = distance > .01f ? MathF.Max(0, location!.Z / distance) : 0;
        float targetEnabled = location == null || !spatial ? 0 : 1;
        float smoothing = 1 - MathF.Exp(-1f / (.025f * WaveFormat.SampleRate));
        float gain = readGain?.Invoke() ?? 1;
        pointSource.SetTarget(WaveFormat.SampleRate, targetPan, targetRear);
        for (int i = 0; i < frames; i++)
        {
            enabled += (targetEnabled - enabled) * smoothing;
            float left = input[i * channels], right = channels == 1 ? left : input[i * channels + 1];
            pointSource.Render((left + right) * .5f, out float l, out float r);
            buffer[offset++] = (left + (l - left) * enabled) * gain;
            buffer[offset++] = (right + (r - right) * enabled) * gain;
        }
        return frames * 2;
    }

    internal static void SmokeTest()
    {
        var tone = new SignalGenerator(48000, 2) { Gain = .2, Frequency = 550 };
        AudioDirection? position = new(1, 0, 0);
        var samples = new SpatialSamples(tone, () => position);
        var buffer = new float[48000];
        void Check(bool right)
        {
            samples.Read(buffer, 0, buffer.Length);
            double leftPower = 0, rightPower = 0;
            for (int i = buffer.Length / 2; i < buffer.Length; i += 2)
            {
                leftPower += buffer[i] * buffer[i];
                rightPower += buffer[i + 1] * buffer[i + 1];
            }
            if (right ? rightPower < 10 * leftPower : leftPower < 10 * rightPower)
                throw new InvalidOperationException("Positional audio did not follow a source across the listener.");
        }
        Check(true);
        position = new(-1, 0, 0);
        Check(false);
        var guest = new SpatialSamples(tone, () => position, positionalOnly: true);
        guest.Read(buffer, 0, buffer.Length);
        position = null;
        int muted = guest.Read(buffer, 0, buffer.Length);
        if (muted != buffer.Length || buffer.Any(value => value != 0))
            throw new InvalidOperationException("Ubuntu audio continued after its floating panel lost its position.");
        tone.Frequency = 1000;
        float gain = 1;
        var volume = new SpatialSamples(tone, () => null, spatial: false, readGain: () => gain);
        volume.Read(buffer, 0, buffer.Length);
        float peak = buffer.Max(MathF.Abs);
        gain = .5f;
        volume.Read(buffer, 0, buffer.Length);
        if (peak <= 0 || MathF.Abs(buffer.Max(MathF.Abs) - peak * .5f) > .0001f)
            throw new InvalidOperationException("Changing playback volume did not scale the running audio stream.");
        gain = 0;
        volume.Read(buffer, 0, buffer.Length);
        if (buffer.Any(value => value != 0))
            throw new InvalidOperationException("Zero playback volume did not mute the audio stream.");
    }
}

// Offscreen browsers and guests send audio on their own threads. Keep a reserve
// so Windows' playback callback can ride out jitter in packet delivery.
internal sealed class SpatialPlaybackBuffer : IWaveProvider
{
    private readonly object gate = new();
    private readonly BufferedWaveProvider buffer;
    private readonly byte[] discard;
    private readonly int capacity;
    private readonly int reserve;
    private bool primed;

    public WaveFormat WaveFormat => buffer.WaveFormat;

    internal SpatialPlaybackBuffer(WaveFormat format)
    {
        capacity = format.SampleRate / 4 * format.BlockAlign;
        reserve = format.SampleRate / 10 * format.BlockAlign;
        discard = new byte[capacity];
        buffer = new BufferedWaveProvider(format) { BufferLength = capacity, ReadFully = false };
    }

    internal void Add(byte[] packet, int count)
    {
        lock (gate)
        {
            int keep = Math.Min(count, capacity);
            int overflow = buffer.BufferedBytes + keep - capacity;
            if (overflow > 0) buffer.Read(discard, 0, overflow);
            buffer.AddSamples(packet, count - keep, keep);
        }
    }

    public int Read(byte[] destination, int offset, int count)
    {
        lock (gate)
        {
            if (!primed) primed = buffer.BufferedBytes >= reserve;
            int copied = primed ? buffer.Read(destination, offset, count) : 0;
            if (copied < count)
            {
                primed = false;
                Array.Clear(destination, offset + copied, count - copied);
            }
            return count;
        }
    }

    internal static void SmokeTest()
    {
        CheckFormat(WaveFormat.CreateIeeeFloatWaveFormat(1000, 2));
        CheckFormat(new WaveFormat(1000, 16, 2));
    }

    private static void CheckFormat(WaveFormat format)
    {
        var audio = new SpatialPlaybackBuffer(format);
        int frame = audio.WaveFormat.BlockAlign;
        var read = new byte[20 * frame];
        void Add(int frames, byte value) => audio.Add(Enumerable.Repeat(value, frames * frame).ToArray(), frames * frame);
        void Check(byte value)
        {
            if (audio.Read(read, 0, read.Length) != read.Length || read.Any(sample => sample != value))
                throw new InvalidOperationException("The spatial audio buffer lost its timing reserve or discarded playable audio.");
        }
        Add(80, 1);
        Check(0);
        Add(20, 1);
        for (int i = 0; i < 5; i++) Check(1);
        Check(0);
        Add(20, 2);
        Check(0);
        Add(80, 2);
        Check(2);
        // A burst discards only the excess oldest frames, leaving the next audio intact.
        Add(150, 3);
        Add(40, 4);
        Check(2);
    }
}

internal enum BrowserAudioBackend { WASAPI, WaveOut }

internal static class SpatialPlayback
{
    internal static IWavePlayer Start(ISampleProvider samples, bool wasapi = true)
    {
        IWavePlayer player = wasapi
            ? new WasapiOut(AudioClientShareMode.Shared, true, 60)
            : new WaveOutEvent { DesiredLatency = 60, NumberOfBuffers = 3 };
        try
        {
            player.Init(wasapi ? new SampleToWaveProvider(samples) : new SampleToWaveProvider16(samples));
            player.Play();
            return player;
        }
        catch
        {
            player.Dispose();
            throw;
        }
    }

    internal static bool IsOutputError(Exception ex) =>
        ex is NAudio.MmException or InvalidOperationException or COMException or NotSupportedException or ArgumentException;
}

internal sealed class BrowserSpatialAudio(BrowserAudioBackend backend, bool spatial, Action<ISampleProvider>? captureSamples = null) : AudioHandler, IDisposable
{
    private readonly object gate = new();
    private SpatialPlaybackBuffer? packets;
    private IWavePlayer? output;
    private bool disposed;
    private float[] left = [], right = [], interleaved = [];
    private byte[] payload = [];
    private AudioDirection? position;

    internal void SetDirection(AudioDirection? value) => Volatile.Write(ref position, value);

    protected override bool GetAudioParameters(IWebBrowser chromiumWebBrowser, IBrowser browser, ref AudioParameters parameters)
    {
        if (captureSamples == null && backend == BrowserAudioBackend.WaveOut && waveOutGetNumDevs() == 0) return false;
        parameters.ChannelLayout = ChannelLayout.LayoutStereo;
        parameters.SampleRate = 48000;
        parameters.FramesPerBuffer = 480;
        return true;
    }

    [DllImport("winmm.dll")]
    private static extern uint waveOutGetNumDevs();

    protected override void OnAudioStreamStarted(IWebBrowser chromiumWebBrowser, IBrowser browser, AudioParameters parameters, int channels)
    {
        lock (gate)
        {
            Reset();
            if (disposed || channels != 2) return;
            packets = new SpatialPlaybackBuffer(WaveFormat.CreateIeeeFloatWaveFormat(parameters.SampleRate, channels));
            var samples = new SpatialSamples(packets.ToSampleProvider(), () => Volatile.Read(ref position),
                spatial: spatial, readGain: FloatingBrowserRuntime.AudioGain);
            if (captureSamples != null)
            {
                captureSamples(samples);
                return;
            }
            try
            {
                output = SpatialPlayback.Start(samples, wasapi: backend == BrowserAudioBackend.WASAPI);
            }
            catch (Exception ex) when (SpatialPlayback.IsOutputError(ex))
            {
                Reset();
                System.Diagnostics.Debug.WriteLine($"[Browser audio] {ex.Message}");
            }
        }
    }

    protected override void OnAudioStreamPacket(IWebBrowser chromiumWebBrowser, IBrowser browser, IntPtr data, int frames, long pts)
    {
        if (frames is <= 0 or > 16384 || data == IntPtr.Zero) return;
        lock (gate)
        {
            if (packets == null) return;
            if (left.Length < frames)
            {
                left = new float[frames]; right = new float[frames];
                interleaved = new float[frames * 2]; payload = new byte[frames * 8];
            }
            Marshal.Copy(Marshal.ReadIntPtr(data), left, 0, frames);
            Marshal.Copy(Marshal.ReadIntPtr(data, IntPtr.Size), right, 0, frames);
            for (int i = 0; i < frames; i++)
            {
                interleaved[2 * i] = left[i]; interleaved[2 * i + 1] = right[i];
            }
            Buffer.BlockCopy(interleaved, 0, payload, 0, frames * 8);
            packets.Add(payload, frames * 8);
        }
    }

    protected override void OnAudioStreamStopped(IWebBrowser chromiumWebBrowser, IBrowser browser)
    { lock (gate) Reset(); }

    protected override void OnAudioStreamError(IWebBrowser chromiumWebBrowser, IBrowser browser, string errorMessage)
    {
        lock (gate) Reset();
        System.Diagnostics.Debug.WriteLine($"[Browser audio] {errorMessage}");
    }

    private void Reset()
    {
        packets = null;
        output?.Dispose();
        output = null;
    }

    public new void Dispose()
    {
        lock (gate) { disposed = true; Reset(); }
        base.Dispose();
    }
}
