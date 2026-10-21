namespace VRX.Desktop;

// Only the interactive desktop owns this mutex. Steam wrappers and isolated
// test processes must remain able to run beside it.
internal sealed class DesktopInstance : IDisposable
{
    internal const string Name = @"Local\VRX.Desktop.UI";
    private readonly Mutex mutex;
    private readonly EventWaitHandle activate;
    private RegisteredWaitHandle? listener;
    private volatile bool disposed;
    public bool IsOwner { get; }

    public DesktopInstance(string name = Name)
    {
        mutex=new Mutex(false,name);
        try { IsOwner=mutex.WaitOne(0); }
        catch(AbandonedMutexException) { IsOwner=true; }
        activate=new EventWaitHandle(false,EventResetMode.AutoReset,name+".Activate");
        if(!IsOwner) activate.Set();
    }
    public void Listen(Action focus)
    {
        if(!IsOwner) throw new InvalidOperationException("Only the desktop owner can listen for activation.");
        listener=ThreadPool.RegisterWaitForSingleObject(activate,(_,_)=>{if(!disposed)focus();},null,Timeout.Infinite,false);
    }
    public void Dispose()
    {
        if(disposed) return;
        disposed=true;listener?.Unregister(null);activate.Dispose();
        if(IsOwner) mutex.ReleaseMutex();
        mutex.Dispose();
    }
}
