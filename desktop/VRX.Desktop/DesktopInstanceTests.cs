using System.Diagnostics;
using System.IO;

namespace VRX.Desktop;

internal static class DesktopInstanceTests
{
    public static int Fixture(string name,string output,bool hold)
    {
        using var instance=new DesktopInstance(name);
        if(instance.IsOwner) instance.Listen(()=>File.WriteAllText(output+".activated","focused"));
        using var release=new EventWaitHandle(false,EventResetMode.ManualReset,name+".Release");
        File.WriteAllText(output,instance.IsOwner?"owner":"duplicate");
        return !hold || !instance.IsOwner || release.WaitOne(15000)?0:1;
    }
    public static async Task Run(string output)
    {
        string root=Path.Combine(output,"desktop-instance-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        string name=@"Local\VRX.Desktop.Test."+Guid.NewGuid().ToString("N");
        Process Start(string file,bool hold) {
            var start=new ProcessStartInfo(Environment.ProcessPath!) {UseShellExecute=false,CreateNoWindow=true};
            foreach(string arg in new[]{"--desktop-instance-fixture",name,file,hold?"hold":"exit"}) start.ArgumentList.Add(arg);
            return Process.Start(start)!;
        }
        async Task WaitFile(string file) {
            long until=Environment.TickCount64+5000;
            while(!File.Exists(file) && Environment.TickCount64<until) await Task.Delay(30);
            if(!File.Exists(file)) throw new Exception("Desktop instance fixture did not produce "+Path.GetFileName(file));
        }
        string first=Path.Combine(root,"first.txt"),second=Path.Combine(root,"second.txt"),third=Path.Combine(root,"third.txt");
        using var owner=Start(first,true);
        try {
            await WaitFile(first);
            using var duplicate=Start(second,false);await duplicate.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            await WaitFile(first+".activated");
            if(File.ReadAllText(first)!="owner" || File.ReadAllText(second)!="duplicate" || duplicate.ExitCode!=0)
                throw new Exception("A duplicate desktop must exit and signal the existing UI");
            using(var release=EventWaitHandle.OpenExisting(name+".Release")) release.Set();
            await owner.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            using var next=Start(third,false);await next.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            if(File.ReadAllText(third)!="owner") throw new Exception("Closing the desktop must release its mutex for the next launch");
        } finally {
            if(!owner.HasExited) {
                using var release=EventWaitHandle.OpenExisting(name+".Release");release.Set();
                await owner.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            }
        }
        File.WriteAllText(Path.Combine(output,"desktop-instance-pass.txt"),"PASS: named mutex ownership across processes; duplicate exits and signals existing UI; normal exit permits a later launch. Steam wrappers and smoke tests bypass the interactive desktop gate.");
    }
}
