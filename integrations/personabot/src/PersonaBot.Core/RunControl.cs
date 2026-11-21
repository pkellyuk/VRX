using System.Diagnostics;

namespace PersonaBot;

/// <summary>
/// Pauses at action boundaries, allowing an in-flight tool to finish safely, and shares the guest's mouse
/// and keyboard between the bot and the person.
/// <para>
/// Two locks: the bot's actions run one at a time under the action lock, which is also what a pause waits
/// for. The guest's input has a lock of its own, taken by the bot only for tools that press keys or move the
/// pointer, and by the person's input. So the person's clicks and keys wait for a bot click or keystroke in
/// progress, never for a bot's shell command, screenshot or file read (which can run for minutes).
/// </para>
/// </summary>
public sealed class RunControl
{
    private readonly object _sync = new();
    private readonly SemaphoreSlim _action = new(1, 1);
    private readonly SemaphoreSlim _input = new(1, 1);
    private TaskCompletionSource? _resume;
    public bool IsPaused { get { lock (_sync) return _resume != null; } }

    public async Task PauseAsync(CancellationToken token)
    {
        Trace.WriteLine($"RunControl.PauseAsync enter: already paused={IsPaused}");
        var waited = Stopwatch.StartNew();
        lock (_sync) _resume ??= new(TaskCreationOptions.RunContinuationsAsynchronously);
        await _action.WaitAsync(token);
        _action.Release();
        Trace.WriteLine($"RunControl.PauseAsync exit: paused after waiting {waited.ElapsedMilliseconds} ms for the action in flight");
    }

    public void Resume()
    {
        bool wasPaused;
        lock (_sync) { var resume = _resume; wasPaused = resume != null; _resume = null; resume?.TrySetResult(); }
        Trace.WriteLine($"RunControl.Resume: {(wasPaused ? "resumed" : "was not paused")}");
    }

    public async Task WaitAsync(CancellationToken token)
    {
        while (true)
        {
            Task? wait;
            lock (_sync) wait = _resume?.Task;
            if (wait == null) { token.ThrowIfCancellationRequested(); return; }
            await wait.WaitAsync(token);
        }
    }

    /// <summary>Runs one bot action. <paramref name="usesInput"/>: it presses keys or moves the pointer, so it also takes the guest's input.</summary>
    public async Task<T> ActAsync<T>(Func<Task<T>> action, CancellationToken token, bool usesInput = false)
    {
        ArgumentNullException.ThrowIfNull(action);
        while (true)
        {
            await WaitAsync(token);
            await _action.WaitAsync(token);
            try
            {
                if (IsPaused)
                {
                    Trace.WriteLine("RunControl.ActAsync: paused after taking the action lock; waiting again");
                    continue;
                }
                if (!usesInput) return await action();
                var waited = Stopwatch.StartNew();
                await _input.WaitAsync(token);
                try
                {
                    if (waited.ElapsedMilliseconds > 250) Trace.WriteLine($"RunControl.ActAsync: the bot's input waited {waited.ElapsedMilliseconds} ms for the person's");
                    return await action();
                }
                finally { _input.Release(); }
            }
            finally { _action.Release(); }
        }
    }

    /// <summary>The person's input: shares the guest's input with the bot's input tools, and works while the bot is paused or running anything else.</summary>
    public async Task OperatorActAsync(Func<Task> action, CancellationToken token)
    {
        ArgumentNullException.ThrowIfNull(action);
        var waited = Stopwatch.StartNew();
        await _input.WaitAsync(token);
        try
        {
            if (waited.ElapsedMilliseconds > 250) Trace.WriteLine($"RunControl.OperatorActAsync: the person's input waited {waited.ElapsedMilliseconds} ms for the bot's");
            token.ThrowIfCancellationRequested();
            await action();
        }
        finally { _input.Release(); }
    }
}
