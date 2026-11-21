namespace Automaton.Core.Models;

/// <summary>Optional, versioned attachment support; absent on older peers.</summary>
public sealed class RemoteAttachmentCapabilities
{
    public int Version { get; set; } = 1;
    public int MaxFiles { get; set; } = 8;
    public long MaxTotalBytes { get; set; } = 32L * 1024 * 1024;
    public bool Images { get; set; }
}
