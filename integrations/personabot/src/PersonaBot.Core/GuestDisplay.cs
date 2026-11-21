namespace PersonaBot;

/// <summary>A BGRA, opaque desktop image. Pixels remain valid until the stream advances again.</summary>
public sealed record GuestDisplayFrame(int Width, int Height, ReadOnlyMemory<byte> Pixels);

/// <summary>Optional live display, independent of PNG screenshots used by tools and attachments.</summary>
public interface IGuestDisplayStream
{
    /// <summary>
    /// A full image followed by changed images. Awaiting the consumer bounds memory and provides
    /// backpressure; idle desktops wait for changes. Cancellation closes this viewer's connection.
    /// </summary>
    IAsyncEnumerable<GuestDisplayFrame> StreamDisplayAsync(CancellationToken token);
}

/// <summary>A bot's own display inside the guest (XMEETING.md §3): a screen, and the X display number its programs open windows on.</summary>
public interface IBotScreen : IGuestScreen
{
    int DisplayNumber { get; }
}

/// <summary>A machine that can give each bot a display of its own, beside its main display (XMEETING.md §3).</summary>
public interface IBotDisplayHost
{
    /// <summary>
    /// The bot's own display as a screen, started if it isn't running (and its packages installed first, the
    /// first time on a machine, which <paramref name="status"/> reports). The same screen each time while it runs.
    /// </summary>
    Task<IBotScreen> OpenBotDisplayAsync(GuestAccount account, Action<string> status, CancellationToken token);
}
