namespace Automaton.Core.Streaming;

/// <summary>
/// Per-stream ordered record of the raw SSE lines as they are read off the
/// owner\u0027s socket. Captured during <c>ReadStreamingResponseAsync</c> for
/// the XCHAT raw-stream replay path: a future remote client receives these
/// same bytes and runs them through the shared <see cref="OpenAIStreamDecoder"/>
/// + <see cref="LlmStreamApplier"/>, arriving at byte-identical state with
/// the owner (XCHAT.md \u00A78, invariant #4).
///
/// Lifecycle: the caller (<c>RemoteSessionService</c> in M3) constructs one
/// per active streaming response and hands it to the provider. The provider
/// is the only writer; the caller reads <see cref="Lines"/> after the
/// streaming call returns. The recorder is not thread-safe \u2014 it is
/// owned by a single streaming call and only that call appends to it.
///
/// Null lines (stream-closed signal) are not recorded; only actual SSE bytes
/// (including blank separators and the <c>data: [DONE]</c> line) are appended.
/// </summary>
public sealed class RawStreamRecorder
{
    private readonly List<string> _lines = new();
    private readonly int? _maximumCharacters;
    private readonly int? _maximumLines;
    private long _characters;
    private ModelResponseLimitException? _failure;

    public RawStreamRecorder() : this(null, null) { }

    public RawStreamRecorder(int? maximumCharacters, int? maximumLines)
    {
        if (maximumCharacters is <= 0) throw new ArgumentOutOfRangeException(nameof(maximumCharacters));
        if (maximumLines is <= 0) throw new ArgumentOutOfRangeException(nameof(maximumLines));
        _maximumCharacters = maximumCharacters;
        _maximumLines = maximumLines;
    }

    /// <summary>
    /// The raw SSE lines in the order they were read off the socket.
    /// </summary>
    public IReadOnlyList<string> Lines => _lines;

    /// <summary>
    /// Append a single line. <paramref name="line"/> may be the empty string
    /// (SSE blank separator) or a <c>data:</c> line; a <c>null</c> line is
    /// the stream-close signal and is ignored.
    /// </summary>
    public void RecordLine(string? line)
    {
        if (line == null)
        {
            return;
        }

        if (_failure != null) throw _failure;
        if ((_maximumLines.HasValue && _lines.Count >= _maximumLines.Value) ||
            (_maximumCharacters.HasValue && _characters + line.Length > _maximumCharacters.Value))
            throw _failure = new ModelResponseLimitException("Raw model stream recording limit exhausted.");
        _characters += line.Length;
        _lines.Add(line);
    }
}
