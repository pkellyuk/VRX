namespace Automaton.Core.Interfaces;

/// <summary>
/// Optional host ownership for work started by callbacks. Admission must be
/// registered before invoking the delegate, and shutdown must await admitted work.
/// An accepted delegate always runs, even with an already cancelled token, so it
/// can release resources acquired before admission. A refused delegate never runs.
/// </summary>
public interface IHostedWork
{
    bool TryRun(string name, Func<CancellationToken, Task> operation);

    /// <summary>
    /// Optional bounded cleanup lane for cancellation controls. A host may keep
    /// this lane alive while admitted work unwinds, then seal and drain it.
    /// Hosts without a cleanup lane retain ordinary admission behavior.
    /// </summary>
    bool TryRunCleanup(string name, Func<CancellationToken, Task> operation) => TryRun(name, operation);

    Task WaitForOutboundCapacityAsync(string peer, CancellationToken token) => Task.CompletedTask;

    Streaming.RawStreamRecorder CreateRawStreamRecorder() => new();

    /// <summary>Optional admission for the full lifetime of an incoming expensive request.</summary>
    RemoteRequestAdmission TryRunRemoteRequest(string name, Models.ConnectionEnvelope request, Func<CancellationToken, Task> operation)
        => TryRun(name, operation) ? RemoteRequestAdmission.Accepted : RemoteRequestAdmission.Refused;

    bool CancelRemoteRequest(string peer, string requestId) => false;
    void CancelRemoteRequests(string? peer) { }

    Models.RemoteRetentionLimits? RemoteRetention => null;
    void FailRemotePeer(string peer, string reason) { }
    Action<string>? CaptureRemotePeerFailure(string peer) => null;
    Models.RemoteQueryResponseLimits? RemoteQueryResponses => null;
}

public enum RemoteRequestAdmission { Accepted, Duplicate, Refused }
