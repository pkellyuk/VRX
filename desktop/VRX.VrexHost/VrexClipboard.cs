using System.Collections.Specialized;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Threading;
using PersonaBot;

namespace VRX.VrexHost;

internal sealed partial class VrexClipboard : IDisposable
{
    private readonly TaskCompletionSource<Dispatcher> dispatcher = new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal VrexClipboard()
    {
        var thread = new Thread(() =>
        {
            dispatcher.SetResult(Dispatcher.CurrentDispatcher);
            Dispatcher.Run();
        }) { IsBackground = true, Name = "VRX clipboard" };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
    }

    internal async Task<string?> ReadTextAsync(CancellationToken token) => await OnClipboardAsync(() =>
    {
        if (Clipboard.ContainsFileDropList() || !Clipboard.ContainsText()) return null;
        string text = Clipboard.GetText();
        return Encoding.UTF8.GetByteCount(text) <= X11Clipboard.MaximumBytes ? text : null;
    }, token);

    internal Task WriteTextAsync(string text, CancellationToken token) => OnClipboardAsync(() =>
    {
        if (Encoding.UTF8.GetByteCount(text) > X11Clipboard.MaximumBytes) throw new ArgumentException("Clipboard text exceeds 16 KiB.");
        if (text.Length == 0) Clipboard.Clear();
        else Clipboard.SetText(text);
        return true;
    }, token);

    internal Task<string[]> ReadFilesAsync(CancellationToken token) => OnClipboardAsync(() =>
        Clipboard.GetFileDropList().Cast<string>().Take(129).ToArray(), token);

    internal Task WriteSyncedTextAsync(string text, uint original, CancellationToken token) => OnClipboardAsync(() =>
    {
        if (SequenceNumber() != original || Clipboard.ContainsFileDropList() ||
            (!Clipboard.ContainsText() && Clipboard.GetDataObject() is { } current && current.GetFormats(false).Length > 0)) return false;
        if (text.Length == 0) Clipboard.Clear();
        else Clipboard.SetText(text);
        return true;
    }, token);

    internal Task<bool> WriteFilesAsync(string[] paths, uint original, CancellationToken token) => OnClipboardAsync(() =>
    {
        if (SequenceNumber() != original) return false;
        var files = new StringCollection();
        files.AddRange(paths);
        Clipboard.SetFileDropList(files);
        return true;
    }, token);

    [LibraryImport("user32.dll", EntryPoint = "GetClipboardSequenceNumber")]
    internal static partial uint SequenceNumber();

    private async Task<T> OnClipboardAsync<T>(Func<T> action, CancellationToken token)
    {
        var ui = await dispatcher.Task.WaitAsync(token);
        for (int attempt = 0; ; attempt++)
        {
            try { return await ui.InvokeAsync(action, DispatcherPriority.Normal, token); }
            catch (COMException) when (attempt < 2) { await Task.Delay(80, token); }
        }
    }

    public void Dispose()
    {
        if (dispatcher.Task.IsCompletedSuccessfully)
            dispatcher.Task.Result.BeginInvokeShutdown(DispatcherPriority.Normal);
    }
}
