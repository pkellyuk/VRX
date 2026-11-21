namespace Automaton.Core.Models;

/// <summary>Opt-in shared lifetime limits for outbound session, tool and memory RPCs. Stream limits apply to session queries.</summary>
public sealed record RemoteQueryResponseLimits(int Queries, int QueriesPerPeer, long BytesPerQuery, long TotalBytes,
    int EventsPerQuery, int PendingChunks, int SequenceGap, long DecodedCharacters)
{
    public void Validate()
    {
        if (Queries <= 0 || QueriesPerPeer <= 0 || BytesPerQuery <= 0 || TotalBytes <= 0 ||
            EventsPerQuery <= 0 || PendingChunks <= 0 || SequenceGap <= 0 || DecodedCharacters <= 0)
            throw new ArgumentOutOfRangeException(nameof(RemoteQueryResponseLimits));
    }
}

/// <summary>A permanent resource/protocol refusal; callers must not automatically replay the request.</summary>
public sealed class RemoteQueryResponseException(string message) : InvalidOperationException(message);
