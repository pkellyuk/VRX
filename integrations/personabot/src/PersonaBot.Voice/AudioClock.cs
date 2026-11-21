using System.Diagnostics;

namespace PersonaBot.Voice;

/// <summary>
/// The one clock the speaker and microphone are both stamped by (seconds), so the echo canceller can line up what
/// was played with what the microphone heard. PortAudio's own stream times are per stream (and some host APIs leave
/// them at zero); only the latencies it reports are used, added to this clock.
/// </summary>
public static class AudioClock
{
    /// <summary>
    /// Held while a stream opens or PortAudio restarts (to see devices plugged in since it started), so a stream
    /// never opens halfway through a restart.
    /// </summary>
    public static readonly object Devices = new();

    /// <summary>
    /// Restarts PortAudio so its device list is current (it reads the devices once, when it starts). Only when no
    /// stream is open (<paramref name="anyOpen"/> false): a restart would close them. True when it restarted.
    /// </summary>
    public static bool RefreshDevices(Func<bool> anyOpen, Action<string> log)
    {
        ArgumentNullException.ThrowIfNull(anyOpen);
        ArgumentNullException.ThrowIfNull(log);
        lock (Devices)
        {
            if (anyOpen())
            {
                log("AudioClock.RefreshDevices: audio is in use; the device list stays as it was");
                return false;
            }
            var watch = Stopwatch.StartNew();
            PortAudioSharp.PortAudio.Terminate();
            PortAudioSharp.PortAudio.Initialize();
            log($"AudioClock.RefreshDevices: re-read the devices in {watch.ElapsedMilliseconds} ms ({PortAudioSharp.PortAudio.DeviceCount} devices)");
            return true;
        }
    }

    /// <summary>Now, in seconds.</summary>
    public static double Now => Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;

    /// <summary>How far apart two of PortAudio's stream times are, when both are real; null when either isn't.</summary>
    public static double? Gap(double later, double earlier)
    {
        if (later <= 0 || earlier <= 0) return null;
        var gap = later - earlier;
        return gap is >= 0 and < 1 ? gap : null;
    }
}
