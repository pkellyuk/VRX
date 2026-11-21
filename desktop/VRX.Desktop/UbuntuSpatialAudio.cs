using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using VRX.VrexBridge;

namespace VRX.Desktop;

// The Ubuntu display emits playback PCM; only play it while a panel has a headset position.
internal sealed class UbuntuSpatialAudio
{
    private readonly object gate = new();
    private AudioDirection? direction;
    private BufferedWaveProvider? input;
    private WaveOutEvent? output;
    private long nextDeviceAttempt;

    internal void SetDirection(AudioDirection? value) => Volatile.Write(ref direction, value);

    internal void Push(string encoded)
    {
        lock (gate)
        {
            if (Volatile.Read(ref direction) == null) { StopCore(); return; }
            Span<byte> scratch = stackalloc byte[VrexProtocol.GuestAudioPacketBytes];
            if (!Convert.TryFromBase64String(encoded, scratch, out int bytes) || bytes == 0 || bytes % 4 != 0) return;
            if (input == null)
            {
                long now = Environment.TickCount64;
                if (now < nextDeviceAttempt || !scratch[..bytes].ContainsAnyExcept((byte)0)) return;
                nextDeviceAttempt = now + 3000;
                try
                {
                    input = new BufferedWaveProvider(new WaveFormat(VrexProtocol.GuestAudioRate, 16, 2))
                    { BufferDuration = TimeSpan.FromMilliseconds(500), DiscardOnBufferOverflow = true };
                    output = new WaveOutEvent { DesiredLatency = 60, NumberOfBuffers = 3 };
                    output.Init(new SampleToWaveProvider16(new SpatialSamples(input.ToSampleProvider(), () => Volatile.Read(ref direction), positionalOnly: true)));
                }
                catch (Exception ex) when (ex is NAudio.MmException or InvalidOperationException)
                {
                    StopCore();
                    System.Diagnostics.Debug.WriteLine($"[Ubuntu playback] {ex.Message}");
                    return;
                }
            }
            if (input.BufferedDuration > TimeSpan.FromMilliseconds(200)) input.ClearBuffer();
            input.AddSamples(scratch[..bytes].ToArray(), 0, bytes);
            try { if (output!.PlaybackState == PlaybackState.Stopped) output.Play(); }
            catch (Exception ex) when (ex is NAudio.MmException or InvalidOperationException)
            {
                StopCore();
                System.Diagnostics.Debug.WriteLine($"[Ubuntu playback] {ex.Message}");
            }
        }
    }

    internal void Stop() { lock (gate) StopCore(); }

    private void StopCore()
    {
        output?.Dispose();
        output = null;
        input = null;
    }
}
