using System.Diagnostics;
using System.IO;
using System.Windows.Threading;
using ScreenRecorderLib;

namespace VRX.Desktop;

internal sealed class VrViewCapture(Dispatcher dispatcher, Action changed) : IDisposable
{
    private Recorder? active;
    private TaskCompletionSource? completed;
    private bool video, stopping;
    public bool CanStop => video && active != null && !stopping;
    public bool Busy => active != null;
    public string Status { get; private set; } = "";

    internal void Start(string folder, bool recordVideo)
    {
        if (Busy) return;
        try
        {
            var monitorIds = Process.GetProcessesByName("vrmonitor")
                .Select(process => { using (process) return process.Id; }).ToHashSet();
            var mirror = Recorder.GetWindows().FirstOrDefault(window =>
                window.Pid is int pid && monitorIds.Contains(pid) &&
                (window.Title.Equals("VR View", StringComparison.OrdinalIgnoreCase) ||
                 window.Title.Equals("SteamVR View", StringComparison.OrdinalIgnoreCase)) &&
                window.IsValidWindow() && !window.IsMinmimized());
            if (mirror == null)
            {
                Status = Loc.Get("CaptureOpenVrView");
                changed();
                return;
            }
            Directory.CreateDirectory(folder);
            string name = recordVideo ? "VR Video" : "VR Screenshot";
            string path = Path.Combine(folder, $"{name} {DateTime.Now:yyyy-MM-dd HH-mm-ss-fff}.{(recordVideo ? "mp4" : "png")}");
            var source = new WindowRecordingSource(mirror.Handle)
            {
                IsCursorCaptureEnabled = false
            };
            var options = RecorderOptions.Default;
            options.SourceOptions.RecordingSources = [source];
            options.OutputOptions.RecorderMode = recordVideo ? RecorderMode.Video : RecorderMode.Screenshot;
            options.SnapshotOptions.SnapshotFormat = ImageFormat.PNG;
            options.AudioOptions.IsAudioEnabled = false;
            options.VideoEncoderOptions.Framerate = 30;
            var recorder = Recorder.CreateRecorder(options);
            active = recorder;
            completed = new(TaskCreationOptions.RunContinuationsAsynchronously);
            video = recordVideo;
            stopping = false;
            recorder.OnRecordingComplete += (_, e) => dispatcher.InvokeAsync(() => Finish(recorder, Loc.Format("CaptureSaved", Path.GetFileName(e.FilePath))));
            recorder.OnRecordingFailed += (_, e) => dispatcher.InvokeAsync(() => Finish(recorder, Loc.Format("CaptureFailed", e.Error)));
            recorder.OnStatusChanged += (_, e) =>
            {
                if (recordVideo && e.Status == RecorderStatus.Recording)
                    dispatcher.InvokeAsync(() =>
                    {
                        if (active == recorder && !stopping) { Status = Loc.Get("CaptureRecordingVideo"); changed(); }
                    });
            };
            Status = recordVideo ? Loc.Get("CaptureStarting") : Loc.Get("CaptureScreenshotting");
            recorder.Record(path);
        }
        catch (Exception ex) when (Recoverable(ex))
        {
            if (active is { } failed) Finish(failed, Loc.Format("CaptureStartFailed", ex.Message));
            else Status = Loc.Format("CaptureStartFailed", ex.Message);
        }
        changed();
    }

    internal async Task StopAsync()
    {
        if (completed is not { } pending || active == null) return;
        Stop();
        await pending.Task.WaitAsync(TimeSpan.FromSeconds(8));
    }

    internal void Stop()
    {
        if (active == null || stopping) return;
        stopping = true;
        Status = video ? Loc.Get("CaptureSaving") : Loc.Get("CaptureFinishingScreenshot");
        try { active.Stop(); }
        catch (Exception ex) when (Recoverable(ex)) { Finish(active, Loc.Format("CaptureStopFailed", ex.Message)); }
        changed();
    }

    private void Finish(Recorder recorder, string status)
    {
        if (active != recorder) return;
        active = null;
        recorder.Dispose();
        completed?.TrySetResult();
        completed = null;
        Status = status;
        changed();
    }

    public void Dispose()
    {
        // Stop finalizes the MP4. Its completion callback disposes the recorder.
        if (active != null) Stop();
    }

    private static bool Recoverable(Exception ex) => ex is IOException or UnauthorizedAccessException or InvalidOperationException
        or System.Runtime.InteropServices.ExternalException or TypeInitializationException or TypeLoadException or BadImageFormatException;
}
