using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using VRX.Audio;
using PortAudioSharp;

namespace PersonaBot.Voice;

/// <summary>
/// Plays float samples on the default output device through PortAudio. Samples are queued as they're made
/// (Kokoro makes a sentence at a time), so speech starts after the first sentence rather than the whole text.
/// <see cref="Level"/> is the loudness of what's playing now, for the bot's mouth.
/// </summary>
public sealed class AudioOut : IDisposable
{
    private readonly Action<string> _log;
    private readonly ConcurrentQueue<float[]> _queue = new();
    private readonly PortAudioSharp.Stream.Callback _callback;
    private PortAudioSharp.Stream? _stream;
    private float[]? _current;
    private int _offset;
    private int _sampleRate;
    private volatile bool _ending;
    private volatile bool _drained = true;
    private volatile float _level;
    private double _latency;

    public AudioOut(Action<string> log)
    {
        ArgumentNullException.ThrowIfNull(log);
        _log = log;
        _callback = Play;
    }

    /// <summary>
    /// A block has been handed to the speaker, silence included, with when its first sample will be heard
    /// (<see cref="AudioClock"/> seconds) and its sample rate: the echo canceller's copy of what's playing. Called on
    /// PortAudio's thread: keep it quick.
    /// </summary>
    public event Action<float[], double, int>? Played;

    /// <summary>The loudness (0–1) of the samples playing now.</summary>
    public float Level => _drained ? 0 : _level;

    /// <summary>How loud it plays, 0 (silent) to 1 (as made): the app's volume and mute.</summary>
    public float Gain
    {
        get => _gain;
        set => _gain = Math.Clamp(value, 0f, 1f);
    }

    private volatile float _gain = 1f;
    private volatile float _pan;
    private volatile float _rear;
    private readonly PointSourceAudio _spatial = new();
    private bool _stereo;
    public void SetDirection(float pan, float rear)
    {
        _pan = pan;
        _rear = rear;
    }

    /// <summary>Whether the output device is open.</summary>
    public bool IsOpen => _stream != null;

    /// <summary>Whether everything queued has been played (after <see cref="End"/>), or nothing was.</summary>
    public bool Drained => _drained;

    /// <summary>Starts a new utterance at <paramref name="sampleRate"/>: anything still playing is dropped.</summary>
    public void Begin(int sampleRate)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        _log($"AudioOut.Begin: {sampleRate} Hz (open stream at {_sampleRate} Hz)");
        Clear();
        _ending = false;
        _drained = false;
        if (_stream != null && _sampleRate == sampleRate) return;
        Close();
        lock (AudioClock.Devices)
        {
            Open(sampleRate);
        }
    }

    private void Open(int sampleRate)
    {
        var device = PortAudio.DefaultOutputDevice;
        if (device == PortAudio.NoDevice) throw new IOException("There's no speaker or headphones to play speech on.");
        var info = PortAudio.GetDeviceInfo(device);
        _stereo = info.maxOutputChannels >= 2;
        var parameters = new StreamParameters
        {
            device = device,
            channelCount = _stereo ? 2 : 1,
            sampleFormat = SampleFormat.Float32,
            suggestedLatency = info.defaultHighOutputLatency,
            hostApiSpecificStreamInfo = IntPtr.Zero
        };
        _stream = new PortAudioSharp.Stream(inParams: null, outParams: parameters, sampleRate: sampleRate, framesPerBuffer: 0,
            streamFlags: StreamFlags.ClipOff, callback: _callback, userData: IntPtr.Zero);
        _latency = info.defaultHighOutputLatency;
        _sampleRate = sampleRate;
        _stream.Start();
        _log($"AudioOut.Begin: playing on device {device} ({info.name}) at {sampleRate} Hz, latency {info.defaultHighOutputLatency * 1000:0} ms");
    }

    /// <summary>Queues samples to play after those already queued.</summary>
    public void Add(float[] samples)
    {
        ArgumentNullException.ThrowIfNull(samples);
        if (samples.Length == 0) return;
        _queue.Enqueue(samples);
    }

    /// <summary>No more samples for this utterance: <see cref="Drained"/> turns true once the queue has played.</summary>
    public void End() => _ending = true;

    /// <summary>Drops everything queued and stops at once.</summary>
    public void Clear()
    {
        while (_queue.TryDequeue(out _)) { }
        _current = null;
        _offset = 0;
        _ending = true;
        _drained = true;
        _level = 0;
    }

    private StreamCallbackResult Play(IntPtr input, IntPtr output, uint frameCount, ref StreamCallbackTimeInfo timeInfo, StreamCallbackFlags statusFlags, IntPtr userData)
    {
        var wanted = (int)frameCount;
        var buffer = new float[wanted];
        var filled = 0;
        while (filled < wanted)
        {
            if (_current == null || _offset >= _current.Length)
            {
                if (!_queue.TryDequeue(out _current)) { _current = null; break; }
                _offset = 0;
            }
            var take = Math.Min(wanted - filled, _current.Length - _offset);
            Array.Copy(_current, _offset, buffer, filled, take);
            _offset += take;
            filled += take;
        }
        double sum = 0;
        for (var i = 0; i < filled; i++) sum += buffer[i] * buffer[i];
        // The mouth follows the voice as made; what reaches the speaker (and the echo canceller) is at the volume set.
        _level = filled == 0 ? 0 : Math.Clamp((float)Math.Sqrt(sum / filled) * 5f, 0f, 1f);
        var gain = _gain;
        if (gain < 0.999f) for (var i = 0; i < filled; i++) buffer[i] *= gain;
        if (filled == 0 && _ending && _queue.IsEmpty) _drained = true;
        if (_stereo)
        {
            var stereo = new float[wanted * 2];
            _spatial.SetTarget(_sampleRate, _pan, _rear);
            for (int i = 0; i < wanted; i++)
            {
                _spatial.Render(buffer[i], out stereo[i * 2], out stereo[i * 2 + 1]);
                buffer[i] = (stereo[i * 2] + stereo[i * 2 + 1]) * .5f;
            }
            Marshal.Copy(stereo, 0, output, stereo.Length);
        }
        else Marshal.Copy(buffer, 0, output, wanted);
        // When this block will be heard: PortAudio's callback-to-speaker gap when it gives one, else the device's latency.
        var ahead = AudioClock.Gap(timeInfo.outputBufferDacTime, timeInfo.currentTime) ?? _latency;
        Played?.Invoke(buffer, AudioClock.Now + ahead, _sampleRate);
        return StreamCallbackResult.Continue;
    }

    /// <summary>Closes the output device (it reopens on the next <see cref="Begin"/>), so an idle helper holds no audio device.</summary>
    public void Close()
    {
        if (_stream == null) return;
        _log("AudioOut.Close");
        try
        {
            _stream.Stop();
        }
        catch (PortAudioException ex)
        {
            _log("AudioOut.Close: stop failed: " + ex.Message);
        }
        _stream.Dispose();
        _stream = null;
        _sampleRate = 0;
    }

    public void Dispose()
    {
        Clear();
        Close();
    }
}
