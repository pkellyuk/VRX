using System.Runtime.InteropServices;
using PortAudioSharp;

namespace PersonaBot.Voice;

/// <summary>
/// Records mono float samples at <see cref="SampleRate"/> from the default microphone through PortAudio, handing
/// each block to <see cref="Samples"/> on PortAudio's thread.
/// </summary>
public sealed class AudioIn : IDisposable
{
    public const int SampleRate = 16000;

    private readonly Action<string> _log;
    private readonly PortAudioSharp.Stream.Callback _callback;
    private PortAudioSharp.Stream? _stream;
    private double _latency;

    public AudioIn(Action<string> log)
    {
        ArgumentNullException.ThrowIfNull(log);
        _log = log;
        _callback = Record;
    }

    /// <summary>
    /// A block of samples has been recorded, with when its first sample was (<see cref="AudioClock"/> seconds).
    /// Called on PortAudio's thread: keep it quick.
    /// </summary>
    public event Action<float[], double>? Samples;

    /// <summary>Whether the microphone is open.</summary>
    public bool IsOpen => _stream != null;

    /// <summary>Whether there's a microphone to listen with.</summary>
    public static bool HasMicrophone => PortAudio.DefaultInputDevice != PortAudio.NoDevice;

    /// <summary>The microphone the person picked (by name), or null for the system's default.</summary>
    public string? Preferred { get; set; }

    /// <summary>
    /// The microphones to choose from: the input devices of the system default microphone's audio API (Windows
    /// lists each microphone once per API, so only one API's are offered), the default first, without Windows'
    /// "Sound Mapper" stand-ins. Name is what <see cref="Preferred"/> takes; Label is the full name to show (the
    /// default API, MME, cuts names at 31 characters, so the same device's full name is borrowed from another API).
    /// </summary>
    public static IReadOnlyList<(string Name, string Label)> Microphones()
    {
        var fallback = PortAudio.DefaultInputDevice;
        if (fallback == PortAudio.NoDevice) return [];
        var api = PortAudio.GetDeviceInfo(fallback).hostApi;
        var others = new List<string>();
        var ours = new List<string>();
        for (var i = 0; i < PortAudio.DeviceCount; i++)
        {
            var info = PortAudio.GetDeviceInfo(i);
            if (info.maxInputChannels < 1 || string.IsNullOrWhiteSpace(info.name)) continue;
            if (info.hostApi != api) others.Add(info.name);
            else if (!IsStandIn(info.name) && !ours.Contains(info.name)) ours.Add(info.name);
        }
        // The default first.
        var first = PortAudio.GetDeviceInfo(fallback).name;
        if (ours.Remove(first)) ours.Insert(0, first);
        return ours.Select(name => (name, FullName(name, others))).ToList();
    }

    /// <summary>Windows' pseudo-devices that only stand in for the default ("Microsoft Sound Mapper - Input", "Primary Sound Capture Driver").</summary>
    private static bool IsStandIn(string name) =>
        name.Contains("Sound Mapper", StringComparison.OrdinalIgnoreCase) || name.Contains("Primary Sound Capture", StringComparison.OrdinalIgnoreCase);

    /// <summary>The longest name another API gives the same device (one that starts with this one), or the name as it is.</summary>
    private static string FullName(string name, IEnumerable<string> others) =>
        others.Where(other => other.Length > name.Length && other.StartsWith(name, StringComparison.Ordinal)).OrderByDescending(other => other.Length).FirstOrDefault() ?? name;

    /// <summary>The device to record from: the preferred microphone when it's there, else the default.</summary>
    private int Device()
    {
        var fallback = PortAudio.DefaultInputDevice;
        if (string.IsNullOrWhiteSpace(Preferred) || fallback == PortAudio.NoDevice) return fallback;
        var api = PortAudio.GetDeviceInfo(fallback).hostApi;
        for (var i = 0; i < PortAudio.DeviceCount; i++)
        {
            var info = PortAudio.GetDeviceInfo(i);
            if (info.hostApi == api && info.maxInputChannels > 0 && info.name == Preferred) return i;
        }
        _log($"AudioIn.Device: \"{Preferred}\" isn't connected; using the default microphone");
        return fallback;
    }

    /// <summary>Opens the default microphone and starts recording.</summary>
    public void Start()
    {
        _log("AudioIn.Start enter");
        Stop();
        lock (AudioClock.Devices)
        {
            Open();
        }
    }

    private void Open()
    {
        var device = Device();
        if (device == PortAudio.NoDevice) throw new IOException("There's no microphone to listen with.");
        var info = PortAudio.GetDeviceInfo(device);
        var parameters = new StreamParameters
        {
            device = device,
            channelCount = 1,
            sampleFormat = SampleFormat.Float32,
            suggestedLatency = info.defaultLowInputLatency,
            hostApiSpecificStreamInfo = IntPtr.Zero
        };
        _stream = new PortAudioSharp.Stream(inParams: parameters, outParams: null, sampleRate: SampleRate, framesPerBuffer: 480,
            streamFlags: StreamFlags.ClipOff, callback: _callback, userData: IntPtr.Zero);
        _latency = info.defaultLowInputLatency;
        _stream.Start();
        _log($"AudioIn.Start exit: recording from device {device} ({info.name}) at {SampleRate} Hz, latency {info.defaultLowInputLatency * 1000:0} ms");
    }

    /// <summary>Stops recording and closes the microphone.</summary>
    public void Stop()
    {
        if (_stream == null) return;
        _log("AudioIn.Stop");
        try
        {
            _stream.Stop();
        }
        catch (PortAudioException ex)
        {
            _log("AudioIn.Stop: stop failed: " + ex.Message);
        }
        _stream.Dispose();
        _stream = null;
    }

    private StreamCallbackResult Record(IntPtr input, IntPtr output, uint frameCount, ref StreamCallbackTimeInfo timeInfo, StreamCallbackFlags statusFlags, IntPtr userData)
    {
        if (input == IntPtr.Zero || frameCount == 0) return StreamCallbackResult.Continue;
        var now = AudioClock.Now;
        var block = new float[frameCount];
        Marshal.Copy(input, block, 0, (int)frameCount);
        // When the block's first sample was recorded: PortAudio's own capture-to-callback gap when it gives one,
        // else the block's length and the device's latency.
        var age = AudioClock.Gap(timeInfo.currentTime, timeInfo.inputBufferAdcTime) ?? frameCount / (double)SampleRate + _latency;
        Samples?.Invoke(block, now - age);
        return StreamCallbackResult.Continue;
    }

    public void Dispose() => Stop();
}
