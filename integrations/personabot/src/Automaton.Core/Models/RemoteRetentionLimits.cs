namespace Automaton.Core.Models;

/// <summary>Optional host limits for received state and diagnostic previews, not canonical chat storage.</summary>
public sealed record RemoteRetentionLimits(
    int StateUpdates, int StateUpdatesPerPeer, long StateBytes, long StateBytesPerPeer,
    int DiagnosticEntries, int DiagnosticBodyCharacters, int Sessions, int MessagesPerSession,
    int MessageCharacters, int SessionCharacters, int TotalSessionCharacters)
{
    public void Validate()
    {
        if (StateUpdates <= 0 || StateUpdatesPerPeer <= 0 || StateBytes <= 0 || StateBytesPerPeer <= 0 ||
            DiagnosticEntries <= 0 || DiagnosticBodyCharacters < 128 || Sessions <= 0 || MessagesPerSession < 2 ||
            MessageCharacters < 64 || SessionCharacters < MessageCharacters || TotalSessionCharacters < SessionCharacters ||
            TotalSessionCharacters < Sessions * 64L)
            throw new ArgumentOutOfRangeException(nameof(RemoteRetentionLimits));
    }
}
