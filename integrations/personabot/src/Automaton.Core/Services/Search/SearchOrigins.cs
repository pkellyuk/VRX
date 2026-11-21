namespace Automaton.Core.Services.Search;

/// <summary>
/// Where the text of a search hit came from: a message as it was actually sent, or a summary
/// the compressor wrote in place of messages it dropped from the model's context.
/// </summary>
/// <remarks>
/// Both are indexed, and a fact can legitimately appear under each - once in the message that
/// stated it and once in the summary that condensed that message. That duplication is
/// deliberate. A summary is lossy, and a reader given only the summary cannot tell what was
/// lost; a reader shown the raw message beside it can. So every hit says which it is rather
/// than the index trying to pick one on the reader's behalf.
/// </remarks>
public static class SearchOrigins
{
    /// <summary>A message as it was sent. Never altered by compaction.</summary>
    public const string Raw = "raw";

    /// <summary>A summary written by context compaction; the original messages may be indexed separately as <see cref="Raw"/>.</summary>
    public const string Compressed = "compressed";

    /// <summary>Normalises a stored or transmitted value to one of the two constants; anything unrecognised is <see cref="Raw"/>.</summary>
    public static string Normalize(string? origin)
    {
        return string.Equals(origin, Compressed, System.StringComparison.OrdinalIgnoreCase) ? Compressed : Raw;
    }
}
