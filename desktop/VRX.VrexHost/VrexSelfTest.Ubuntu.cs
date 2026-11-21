using PersonaBot;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal static partial class VrexSelfTest
{
    internal static async Task<int> UbuntuAsync(string root)
    {
        using var lease = MachineLease.Acquire(root);
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var audio = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        bool annotated = false;
        var channel = VrexProtocol.ChannelPrefix + Guid.NewGuid().ToString("N");
        await using var session = new VrexSession(channel, root, message =>
        {
            if (message.Type is "status" or "ready" or "error" or "verification-passed")
                Console.WriteLine(message.Text);
            if (message.Type == "ready") ready.TrySetResult();
            if (message.Type == "error") ready.TrySetException(new IOException(message.Text));
            if (message is { Type: "screen-marks", Text: "ubuntu", Marks: [{ Shape: "Box" }] }) annotated = true;
            if (message.Type == "guest-audio" && message.Text != null &&
                Convert.FromBase64String(message.Text).Any(sample => sample != 0)) audio.TrySetResult();
        }, allowDiagnostics: true);
        try
        {
            await session.HandleAsync(new("configure-ubuntu-start", CpuCount: 4, Resolution: "1920x1080"));
            await ready.Task.WaitAsync(TimeSpan.FromMinutes(35));
            await session.HandleAsync(new("verify-vm"));
            if (!annotated) throw new IOException("Ubuntu's annotation did not reach the VRX event stream.");
            await session.HandleAsync(new("verify-display-stream"));
            await session.HandleAsync(new("verify-audio"));
            await audio.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Console.WriteLine("Verified Ubuntu playback reaches the VRX audio stream.");
            await session.HandleAsync(new("ubuntu-stop"));
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }
}
