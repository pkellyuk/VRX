using VRX.VrexBridge;

namespace VRX.VrexHost;

internal static class VrexInputQueueTests
{
    internal static async Task RunAsync()
    {
        static void Check(bool condition,string message){if(!condition)throw new Exception("Input queue: "+message);}
        var entered=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var fresh=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int scrolls=0,oldText=0,errors=0;
        await using(var queue=new VrexInputQueue(async(request,token)=>
        {
            if(request.Type=="scroll")
            {Interlocked.Increment(ref scrolls);entered.TrySetResult();await Task.Delay(Timeout.Infinite,token);}
            if(request.Text=="old")Interlocked.Increment(ref oldText);
            if(request.Text=="fresh")fresh.TrySetResult();
        },_=>Interlocked.Increment(ref errors),CancellationToken.None))
        {
            Check(queue.Submit(new("scroll")),"first scroll rejected");
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(2));
            for(int i=0;i<200;i++)Check(queue.Submit(new("scroll")),"coalesced scroll rejected");
            Check(queue.Submit(new("text","old")),"old input rejected");
            queue.Cancel();Check(queue.Submit(new("text","fresh")),"new focus input rejected");
            await fresh.Task.WaitAsync(TimeSpan.FromSeconds(2));
            Check(scrolls==1 && oldText==0 && errors==0,"focus loss must cancel an in-flight gesture and discard stale queued input");
        }
        var blocked=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var released = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var full = new VrexInputQueue(async (request, token) =>
        {
            if (request.Type == "release-input") { released.TrySetResult(); return; }
            blocked.TrySetResult();
            await Task.Delay(Timeout.Infinite, token);
        }, _ => Interlocked.Increment(ref errors), CancellationToken.None);
        Check(full.Submit(new("text","blocking")),"blocking action rejected");
        await blocked.Task.WaitAsync(TimeSpan.FromSeconds(2));
        for(int i=0;i<64;i++)Check(full.Submit(new("text",i.ToString())),"bounded capacity changed");
        Check(!full.Submit(new("text","overflow")),"overflow must reject rather than block the command reader");
        full.Cancel(releaseButton: true);
        full.Cancel();
        await released.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await full.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        Check(errors==0,"shutdown must cancel blocked guest input without reporting a failure");
    }
}
