using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

/// <summary>Opt-in async backpressure for providers serving a remote turn.</summary>
public static class OutboundFlowScope
{
    private static readonly AsyncLocal<Func<CancellationToken, Task>?> Active = new();
    public static IDisposable Enter(IHostedWork? host, string peer)
    {
        var previous = Active.Value;
        Active.Value = host == null ? null : token => host.WaitForOutboundCapacityAsync(peer, token);
        return new Restore(previous);
    }
    public static Task WaitAsync(CancellationToken token)
    { token.ThrowIfCancellationRequested(); return Active.Value?.Invoke(token) ?? Task.CompletedTask; }
    private sealed class Restore(Func<CancellationToken, Task>? previous) : IDisposable
    { public void Dispose() => Active.Value = previous; }
}
