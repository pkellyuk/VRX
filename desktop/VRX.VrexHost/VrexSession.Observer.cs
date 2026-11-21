using PersonaBot;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private Task? observation;
    private CancellationTokenSource? observationCancellation;
    private ObserverLook? lastLook;
    private string? lastSurface;
    private DateTime nextRemark;
    private readonly List<string> recentRemarks = [];

    private sealed record PcFrame(byte[] Image, int X, int Y, int Width, int Height, string? Surface)
    {
        private string Source => Surface switch
        {
            "browser" => "This is the floating browser the user pointed at.",
            "settings" => "This is the VRX settings panel the user pointed at.",
            _ => "This is the user's main PC display."
        };
        internal string Focus => Source + " " + (X < 0 || Y < 0
            ? "No pointer was visible. Consider the entire display."
            : FormattableString.Invariant($"The pointer was at ({X}, {Y}) in this {Width} × {Height} image, measured from the top left. Use it as a focus hint and consider the full display."));
    }

    private static PcFrame? DecodePcImage(string? encoded, int x, int y, string? surface = null)
    {
        if (encoded == null) return null;
        if (encoded.Length > VrexProtocol.MaxPcImageBytes * 4 / 3 + 4)
            throw new ArgumentException("The PC screenshot is too large.");
        byte[] bytes;
        try { bytes = Convert.FromBase64String(encoded); }
        catch (FormatException ex) { throw new ArgumentException("The PC screenshot is invalid.", ex); }
        if (bytes.Length > VrexProtocol.MaxPcImageBytes || bytes.Length < 24 ||
            !bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }))
            throw new ArgumentException("The PC screenshot is invalid.");
        int width = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(bytes.AsSpan(16, 4));
        int height = System.Buffers.Binary.BinaryPrimitives.ReadInt32BigEndian(bytes.AsSpan(20, 4));
        if (width is < 1 or > 1280 || height is < 1 or > 720)
            throw new ArgumentException("The PC screenshot is too large.");
        bool inFrame = x >= 0 && y >= 0 && x < width && y < height;
        return new(bytes, inFrame ? x : -1, inFrame ? y : -1, width, height, surface);
    }

    private async Task ObservePcAsync(PcFrame frame, string surface, bool speak, CancellationToken token)
    {
        try
        {
            token.ThrowIfCancellationRequested();
            var look = ObserverEyes.Look(frame.Image, frame.X, frame.Y, wholeFrame: frame.X < 0);
            if (look == null || (surface == lastSurface && !Observer.WorthARemark(lastLook, look))) return;
            lastLook = look;
            lastSurface = surface;
            nextRemark = DateTime.UtcNow + Observer.RemarkGap;
            token.ThrowIfCancellationRequested();
            string context = surface switch
            {
                "settings" => "This image is the VRX settings panel the person is pointing at. If useful, explain the visible control near the pointer by its label. Describe only what the visible controls support.",
                "browser" => "This image is the floating web browser the person is pointing at. Comment on the visible page if useful.",
                _ => "This image is the person's main PC display."
            };
            var answer = await Observer.AskAsync(settings, "Vrex", look, [], recentRemarks, [], token,
                display: frame.Image, displayFocus: context + " " + frame.Focus);
            if (token.IsCancellationRequested || string.IsNullOrWhiteSpace(answer?.Say)) return;
            recentRemarks.Add(answer.Say);
            if (recentRemarks.Count > 6) recentRemarks.RemoveAt(0);
            var choices = answer.Options.Where(choice => !string.IsNullOrWhiteSpace(choice))
                .Select(choice => choice.Length > 100 ? choice[..100] : choice).Take(2).ToArray();
            string remark = string.IsNullOrWhiteSpace(answer.Question) ? answer.Say : $"{answer.Say}\n{answer.Question}";
            emit(new("suggestion", remark, Choices: choices, ObservedSurface: surface));
            if (speak && !Live && !token.IsCancellationRequested) await SpeakRecapAsync(answer.Say, token, announceState:false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex)
        {
            System.Diagnostics.Trace.WriteLine($"Vrex observer: {ex.GetType().Name}");
        }
    }
}
