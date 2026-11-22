using NAudio.Wave;
using VRX.VrexBridge;

namespace VRX.Desktop;

// The Ubuntu display emits playback PCM; only play it while a panel has a headset position.
internal sealed class UbuntuSpatialAudio
{
    private readonly object gate = new();
    private AudioDirection? direction;
    private readonly byte[] packet = new byte[VrexProtocol.GuestAudioPacketBytes];
    private SpatialPlaybackBuffer? input;
    private IWavePlayer? output;
    private long nextDeviceAttempt;
    private bool spatial = true, wasapi = true;
    private float gain = 1;

    internal void Configure(bool enableSpatial, BrowserAudioBackend backend, int volume)
    {
        lock (gate)
        {
            Volatile.Write(ref gain, volume / 100f);
            bool useWasapi = backend == BrowserAudioBackend.WASAPI;
            if (spatial == enableSpatial && wasapi == useWasapi) return;
            spatial = enableSpatial;
            wasapi = useWasapi;
            StopCore();
            nextDeviceAttempt = 0;
        }
    }

    internal void SetDirection(AudioDirection? value) => Volatile.Write(ref direction, value);

    internal void Push(string encoded)
    {
        lock (gate)
        {
            if (Volatile.Read(ref direction) == null) { StopCore(); return; }
            if (!Convert.TryFromBase64String(encoded, packet, out int bytes) || bytes == 0 || bytes % 4 != 0) return;
            if (output?.PlaybackState == PlaybackState.Stopped) StopCore();
            if (input != null)
            {
                input.Add(packet, bytes);
                return;
            }
            long now = Environment.TickCount64;
            if (now < nextDeviceAttempt || !packet.AsSpan(0, bytes).ContainsAnyExcept((byte)0)) return;
            nextDeviceAttempt = now + 3000;
            try
            {
                input = new SpatialPlaybackBuffer(new WaveFormat(VrexProtocol.GuestAudioRate, 16, 2));
                input.Add(packet, bytes);
                var samples = new SpatialSamples(input.ToSampleProvider(), () => Volatile.Read(ref direction),
                    positionalOnly: true, spatial: spatial, readGain: () => Volatile.Read(ref gain));
                output = SpatialPlayback.Start(samples, wasapi);
            }
            catch (Exception ex) when (SpatialPlayback.IsOutputError(ex))
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
