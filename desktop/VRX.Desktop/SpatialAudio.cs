using System.Runtime.InteropServices;
using CefSharp;
using CefSharp.Enums;
using CefSharp.Handler;
using CefSharp.Structs;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using VRX.Audio;

namespace VRX.Desktop;

internal sealed record AudioDirection(float X, float Y, float Z);

// Render a moving point source for headphones. A short interaural delay and
// head shadow supplement equal-power panning. With no headset pose, play the
// original stereo signal or centered speech.
internal sealed class SpatialSamples(ISampleProvider source, Func<AudioDirection?> direction, bool positionalOnly = false) : ISampleProvider
{
    public WaveFormat WaveFormat { get; } = WaveFormat.CreateIeeeFloatWaveFormat(source.WaveFormat.SampleRate, 2);
    private readonly PointSourceAudio spatial = new();
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
        float targetEnabled = location == null ? 0 : 1;
        float smoothing = 1 - MathF.Exp(-1f / (.025f * WaveFormat.SampleRate));
        spatial.SetTarget(WaveFormat.SampleRate, targetPan, targetRear);
        for (int i = 0; i < frames; i++)
        {
            enabled += (targetEnabled - enabled) * smoothing;
            float left = input[i * channels], right = channels == 1 ? left : input[i * channels + 1];
            spatial.Render((left + right) * .5f, out float l, out float r);
            buffer[offset++] = left + (l - left) * enabled;
            buffer[offset++] = right + (r - right) * enabled;
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
    }
}

internal sealed class BrowserSpatialAudio(Action<ISampleProvider>? captureSamples = null) : AudioHandler, IDisposable
{
    private readonly object gate = new();
    private BufferedWaveProvider? packets;
    private WaveOutEvent? output;
    private bool disposed;
    private float[] left = [], right = [], interleaved = [];
    private byte[] payload = [];
    private AudioDirection? position;

    internal void SetDirection(AudioDirection? value) => Volatile.Write(ref position, value);

    protected override bool GetAudioParameters(IWebBrowser chromiumWebBrowser, IBrowser browser, ref AudioParameters parameters)
    {
        if (captureSamples == null && waveOutGetNumDevs() == 0) return false;
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
            packets = new BufferedWaveProvider(WaveFormat.CreateIeeeFloatWaveFormat(parameters.SampleRate, channels))
            {
                BufferDuration = TimeSpan.FromMilliseconds(250),
                DiscardOnBufferOverflow = true
            };
            var samples = new SpatialSamples(packets.ToSampleProvider(), () => Volatile.Read(ref position));
            if (captureSamples != null)
            {
                captureSamples(samples);
                return;
            }
            try
            {
                output = new WaveOutEvent { DesiredLatency = 60, NumberOfBuffers = 3 };
                output.Init(new SampleToWaveProvider16(samples));
                output.Play();
            }
            catch (Exception ex) when (ex is NAudio.MmException or InvalidOperationException)
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
            if (packets.BufferedDuration > TimeSpan.FromMilliseconds(160)) packets.ClearBuffer();
            packets.AddSamples(payload, 0, frames * 8);
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
