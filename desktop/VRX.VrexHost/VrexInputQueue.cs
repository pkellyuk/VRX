using System.Threading.Channels;
using VRX.VrexBridge;

namespace VRX.VrexHost;

// Guest input may wait for an agent action or QEMU. Keep the command
// reader responsive, coalesce pointer motion and scrolling, and discard stale gestures.
internal sealed class VrexInputQueue : IAsyncDisposable
{
    private readonly object gate=new();
    private sealed class PendingInput(VrexRequest request, CancellationToken token)
    {
        public VrexRequest Request { get; set; } = request;
        public CancellationToken Token { get; } = token;
    }
    private readonly Channel<PendingInput> queue=Channel.CreateBounded<PendingInput>(64);
    private readonly Func<VrexRequest,CancellationToken,Task> execute;
    private readonly Action<Exception> failed;
    private readonly CancellationToken shutdown;
    private CancellationTokenSource epoch;
    private readonly Task pump;
    private bool scrollPending,disposed;
    private PendingInput? pendingMove;
    internal VrexInputQueue(Func<VrexRequest,CancellationToken,Task> execute,Action<Exception> failed,CancellationToken shutdown)
    {
        this.execute=execute;this.failed=failed;this.shutdown=shutdown;
        epoch=CancellationTokenSource.CreateLinkedTokenSource(shutdown);pump=Task.Run(PumpAsync);
    }
    internal bool Submit(VrexRequest request)
    {
        lock(gate)
        {
            if(disposed)return false;
            if(request.Type=="scroll" && scrollPending)return true;
            if(request.Type=="move" && pendingMove is { } move)
            {
                move.Request=request;
                return true;
            }
            var pending = new PendingInput(request, epoch.Token);
            if(!queue.Writer.TryWrite(pending))return false;
            // Keep movement on its own side of a click or another input event.
            pendingMove = request.Type=="move" ? pending : null;
            if(request.Type=="scroll")scrollPending=true;
            return true;
        }
    }
    internal void Cancel(bool releaseButton = false)
    {
        lock(gate)
        {
            if(disposed)return;
            pendingMove=null;
            epoch.Cancel();epoch.Dispose();epoch=CancellationTokenSource.CreateLinkedTokenSource(shutdown);
            if (releaseButton && !queue.Writer.TryWrite(new(new("release-input"), shutdown)))
                _ = QueueReleaseAsync(shutdown);
        }
    }
    private async Task QueueReleaseAsync(CancellationToken token)
    {
        try { await queue.Writer.WriteAsync(new(new("release-input"), token), token); }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (ChannelClosedException) { }
    }
    private async Task PumpAsync()
    {
        await foreach(var item in queue.Reader.ReadAllAsync())
        {
            VrexRequest request;
            lock (gate)
            {
                if (ReferenceEquals(pendingMove, item)) pendingMove = null;
                request = item.Request;
            }
            try {if(!item.Token.IsCancellationRequested)await execute(request,item.Token);}
            catch(OperationCanceledException) when(item.Token.IsCancellationRequested){}
            catch(Exception) when(item.Token.IsCancellationRequested){}
            catch(Exception ex){failed(ex);}
            finally {if(request.Type=="scroll")lock(gate)scrollPending=false;}
        }
    }
    public async ValueTask DisposeAsync()
    {
        lock(gate)
        {
            if(disposed)return;
            disposed=true;epoch.Cancel();queue.Writer.TryComplete();
        }
        await pump;epoch.Dispose();
    }
}
