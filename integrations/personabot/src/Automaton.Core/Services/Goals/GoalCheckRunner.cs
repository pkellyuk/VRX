using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Automaton.Core.Services.Goals;

/// <summary>What happened when a check command was run. Says nothing about the criterion yet.</summary>
public sealed class GoalCheckExecution
{
    public int? ExitCode { get; set; }
    public bool TimedOut { get; set; }
    public bool Cancelled { get; set; }

    /// <summary>Set when the command could not be started at all.</summary>
    public string? StartError { get; set; }

    /// <summary>The last lines of standard output and standard error, in the order they arrived.</summary>
    public string OutputTail { get; set; } = string.Empty;

    public TimeSpan Duration { get; set; }
}

/// <summary>Runs one check command. A seam, so tests need no real processes.</summary>
public interface IGoalCheckRunner
{
    Task<GoalCheckExecution> RunAsync(string command, string workingFolder, TimeSpan timeout, CancellationToken cancellationToken);
}

/// <summary>
/// Runs a check command the way the shell tool runs a command: <c>cmd.exe /c</c> on Windows,
/// <c>/bin/bash -c</c> elsewhere, in the goal's working folder. Only the tail of the output is
/// kept, as it arrives, so a test run that prints a hundred thousand lines costs no memory.
/// A timeout or a cancellation kills the whole process tree.
/// </summary>
public sealed class ShellGoalCheckRunner : IGoalCheckRunner
{
    public const int TailLines = 40;
    public const int TailChars = 4000;

    public async Task<GoalCheckExecution> RunAsync(string command, string workingFolder, TimeSpan timeout, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(command))
        {
            throw new ArgumentException("command is required", nameof(command));
        }

        AutomatonDebug.Log($"ShellGoalCheckRunner.RunAsync: Entry - folder={workingFolder}, timeout={timeout.TotalSeconds:0}s, command={command}");
        var execution = new GoalCheckExecution();
        var stopwatch = Stopwatch.StartNew();

        if (string.IsNullOrWhiteSpace(workingFolder) || !Directory.Exists(workingFolder))
        {
            execution.StartError = $"the working folder does not exist: {workingFolder}";
            AutomatonDebug.LogWarning($"ShellGoalCheckRunner.RunAsync: Exit - {execution.StartError}");
            return execution;
        }

        var tail = new Queue<string>();
        var tailLock = new object();
        void Keep(string? line)
        {
            if (line == null)
            {
                return;
            }

            lock (tailLock)
            {
                tail.Enqueue(line.Length > 500 ? line.Substring(0, 500) + "…" : line);
                while (tail.Count > TailLines)
                {
                    tail.Dequeue();
                }
            }
        }

        using var process = new Process();
        process.StartInfo = new ProcessStartInfo
        {
            FileName = OperatingSystem.IsWindows() ? "cmd.exe" : "/bin/bash",
            Arguments = OperatingSystem.IsWindows() ? $"/c {command}" : $"-c \"{command.Replace("\"", "\\\"")}\"",
            WorkingDirectory = workingFolder,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        process.OutputDataReceived += (_, args) => Keep(args.Data);
        process.ErrorDataReceived += (_, args) => Keep(args.Data);

        try
        {
            process.Start();
        }
        catch (Exception ex)
        {
            execution.StartError = $"the command could not be started: {ex.Message}";
            AutomatonDebug.LogError($"ShellGoalCheckRunner.RunAsync: Exit - {execution.StartError}");
            return execution;
        }

        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(timeout);

        try
        {
            await process.WaitForExitAsync(timeoutSource.Token).ConfigureAwait(false);

            // The parameterless wait flushes the output handlers; the async one does not promise to.
            process.WaitForExit();
            execution.ExitCode = process.ExitCode;
        }
        catch (OperationCanceledException)
        {
            execution.Cancelled = cancellationToken.IsCancellationRequested;
            execution.TimedOut = !execution.Cancelled;
            Kill(process);
        }

        stopwatch.Stop();
        execution.Duration = stopwatch.Elapsed;

        lock (tailLock)
        {
            var text = string.Join("\n", tail);
            execution.OutputTail = text.Length <= TailChars ? text : "…" + text.Substring(text.Length - TailChars);
        }

        AutomatonDebug.Log($"ShellGoalCheckRunner.RunAsync: Exit - exitCode={execution.ExitCode?.ToString() ?? "-"}, timedOut={execution.TimedOut}, cancelled={execution.Cancelled}, seconds={execution.Duration.TotalSeconds:0.0}, tailChars={execution.OutputTail.Length}");
        return execution;
    }

    private static void Kill(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"ShellGoalCheckRunner.Kill: could not kill the check's process tree - {ex.Message}");
        }
    }
}
