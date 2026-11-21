using PersonaBot;

namespace VRX.VrexHost;

// Waiting for an in-flight action to pause must not block the worker's command
// reader: Resume, Cancel and Stop still need to be processed during that wait.
internal sealed class VrexPause : IDisposable
{
    private readonly object gate=new();
    private int generation;
    private CancellationTokenSource? pending;
    private RunControl? control;
    internal Task Completion {get;private set;}=Task.CompletedTask;
    internal void Request(RunControl target,Action paused,Action<Exception> failed,CancellationToken shutdown)
    {
        lock(gate)
        {
            ResumeCore();control=target;pending=CancellationTokenSource.CreateLinkedTokenSource(shutdown);
            Completion=CompleteAsync(target,paused,failed,pending.Token,generation);
        }
    }
    private async Task CompleteAsync(RunControl target,Action paused,Action<Exception> failed,CancellationToken token,int request)
    {
        try
        {
            await target.PauseAsync(token);
            lock(gate)if(request==generation && !token.IsCancellationRequested && target.IsPaused)paused();
        }
        catch(OperationCanceledException) when(token.IsCancellationRequested){}
        catch(Exception ex){lock(gate)if(request==generation)failed(ex);}
    }
    internal void Resume()
    {lock(gate)ResumeCore();}
    private void ResumeCore()
    {
        generation++;
        pending?.Cancel();pending?.Dispose();pending=null;
        control?.Resume();control=null;
    }
    public void Dispose()=>Resume();
}
