using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Automaton.Core.Interfaces;
using Automaton.Core.Models;

namespace Automaton.Core.Services.Workers;

/// <summary>The background worker tool's id and the rules for who is offered it.</summary>
public static class BackgroundWorkerInfo
{
    public const string ToolId = "background_worker";

    /// <summary>Worker chats are saved under this prefix, which is also how a worker is recognised.</summary>
    public const string WorkerStatePrefix = "workers/";

    /// <summary>The most workers one chat may have running at once.</summary>
    public const int MaxRunningPerChat = 3;

    /// <summary>
    /// The part of a chat's id that is the same however it is written. A turn names its chat by
    /// saved path ("projects\{project}\{thread}.json") while the chat view uses the bare thread id,
    /// and on 22 Oct 2026 that mismatch kept the workers panel empty for a worker that ran fine.
    /// </summary>
    public static string ChatKey(string? stateId)
    {
        if (string.IsNullOrWhiteSpace(stateId))
        {
            return string.Empty;
        }

        var normalized = stateId.Trim().Replace('\\', '/');
        if (IsWorkerChat(normalized))
        {
            return normalized.ToLowerInvariant();
        }

        var last = normalized[(normalized.LastIndexOf('/') + 1)..];
        if (last.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
        {
            last = last[..^".json".Length];
        }

        return last.ToLowerInvariant();
    }

    public static bool IsWorkerChat(string? stateId)
    {
        return !string.IsNullOrWhiteSpace(stateId) &&
               stateId.Replace('\\', '/').StartsWith(WorkerStatePrefix, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Offered to a saved chat on a cloud model that is not itself a worker. A local model is one
    /// server slot, and workers would only queue behind each other on it.
    /// </summary>
    public static bool IsOffered(ToolContext? context)
    {
        return context != null &&
               context.IsCloudModel &&
               !string.IsNullOrWhiteSpace(context.ConversationStateId) &&
               !IsWorkerChat(context.ConversationStateId);
    }
}

public enum BackgroundWorkerStatus
{
    Running,
    Succeeded,
    Failed,
    Stopped
}

/// <summary>One background worker: a separate chat doing a piece of its parent chat's work.</summary>
public sealed class BackgroundWorker
{
    public string Id { get; init; } = string.Empty;

    public string ParentStateId { get; init; } = string.Empty;

    public string WorkerStateId { get; init; } = string.Empty;

    public string Title { get; init; } = string.Empty;

    public string Brief { get; init; } = string.Empty;

    public DateTimeOffset StartedAt { get; init; } = DateTimeOffset.UtcNow;

    public BackgroundWorkerStatus Status { get; internal set; } = BackgroundWorkerStatus.Running;

    /// <summary>What it is doing right now, for the panel.</summary>
    public string Activity { get; internal set; } = "Starting…";

    public DateTimeOffset? FinishedAt { get; internal set; }

    /// <summary>Its closing report - the last thing it said - once it has finished.</summary>
    public string? Summary { get; internal set; }

    /// <summary>Whether the parent chat has been sent the result.</summary>
    public bool Reported { get; internal set; }

    internal CancellationTokenSource Cancellation { get; } = new();

    public bool IsRunning => Status == BackgroundWorkerStatus.Running;

    public string Describe()
    {
        var elapsed = (FinishedAt ?? DateTimeOffset.UtcNow) - StartedAt;
        return $"{Id} \"{Title}\" - {Status}{(IsRunning ? $" ({Activity})" : string.Empty)}, {elapsed.TotalMinutes:0.#} min";
    }
}

/// <summary>
/// Starts background workers for a chat and brings their results back to it.
///
/// A worker is a new chat (saved as "workers/{id}") seeded with its parent's system prompt,
/// model and folder, given a brief, and run through the same conversation service a scheduled
/// run uses - so it gets the full toolset, compaction and the rest. The parent's tool call returns
/// at once. When the worker finishes, its closing report is sent into the parent chat as a message,
/// which starts a turn there; if the parent is mid-turn the message simply waits its turn.
///
/// Held in memory: a worker belongs to the run of the app that started it. The worker chats
/// themselves are saved like any other and can be opened afterwards.
/// </summary>
public sealed class BackgroundWorkerService
{
    private static readonly Regex ThinkBlock = new("<think>.*?(</think>|$)", RegexOptions.Singleline | RegexOptions.IgnoreCase);

    private readonly Func<IRemoteConversationService?> _runner;
    private readonly object _lock = new();
    private readonly List<BackgroundWorker> _workers = new();

    public BackgroundWorkerService(Func<IRemoteConversationService?> runner)
    {
        _runner = runner ?? throw new ArgumentNullException(nameof(runner));
    }

    /// <summary>
    /// Raised whenever a worker starts, changes what it is doing, or finishes - and with null
    /// when finished workers are cleared from a chat's list.
    /// </summary>
    public event Action<BackgroundWorker?>? Changed;

    /// <summary>The workers a chat started, newest first.</summary>
    public IReadOnlyList<BackgroundWorker> ForChat(string? parentStateId)
    {
        if (string.IsNullOrWhiteSpace(parentStateId))
        {
            return Array.Empty<BackgroundWorker>();
        }

        var key = BackgroundWorkerInfo.ChatKey(parentStateId);
        lock (_lock)
        {
            return _workers.Where(worker => BackgroundWorkerInfo.ChatKey(worker.ParentStateId) == key)
                .OrderByDescending(worker => worker.StartedAt)
                .ToList();
        }
    }

    /// <summary>The worker whose chat is saved as <paramref name="workerStateId"/>, whichever chat started it.</summary>
    public BackgroundWorker? ForChatOrAny(string? workerStateId)
    {
        var key = BackgroundWorkerInfo.ChatKey(workerStateId);
        if (key.Length == 0)
        {
            return null;
        }

        lock (_lock)
        {
            return _workers.FirstOrDefault(worker => BackgroundWorkerInfo.ChatKey(worker.WorkerStateId) == key);
        }
    }

    public BackgroundWorker? Find(string? workerId)
    {
        if (string.IsNullOrWhiteSpace(workerId))
        {
            return null;
        }

        lock (_lock)
        {
            return _workers.FirstOrDefault(worker => string.Equals(worker.Id, workerId.Trim(), StringComparison.OrdinalIgnoreCase));
        }
    }

    /// <summary>
    /// Seeds the worker's chat and starts it running. Returns as soon as it has started; the
    /// result reaches the parent chat later as a message.
    /// </summary>
    public async Task<BackgroundWorker> StartAsync(ToolContext parent, string title, string brief)
    {
        if (parent == null)
        {
            throw new ArgumentNullException(nameof(parent));
        }

        if (string.IsNullOrWhiteSpace(title))
        {
            throw new ArgumentException("A short title is required.", nameof(title));
        }

        if (string.IsNullOrWhiteSpace(brief))
        {
            throw new ArgumentException("A brief is required: the worker sees nothing of this chat but the brief.", nameof(brief));
        }

        AutomatonDebug.Log($"BackgroundWorkerService.StartAsync: Entry - parent={parent.ConversationStateId}, title='{title}', briefChars={brief.Length}");
        if (!BackgroundWorkerInfo.IsOffered(parent))
        {
            AutomatonDebug.LogWarning($"BackgroundWorkerService.StartAsync: Exit - not offered here (cloud={parent.IsCloudModel}, state={parent.ConversationStateId})");
            throw new InvalidOperationException("Background workers are only available to chats on a cloud model, and a worker cannot start workers of its own.");
        }

        var runner = _runner();
        if (runner == null)
        {
            AutomatonDebug.LogError("BackgroundWorkerService.StartAsync: Exit - no conversation service");
            throw new InvalidOperationException("Background workers are unavailable: this app has no conversation service.");
        }

        var parentStateId = parent.ConversationStateId!;
        var running = ForChat(parentStateId).Count(worker => worker.IsRunning);
        if (running >= BackgroundWorkerInfo.MaxRunningPerChat)
        {
            AutomatonDebug.LogWarning($"BackgroundWorkerService.StartAsync: Exit - {running} already running for {parentStateId}");
            throw new InvalidOperationException($"This chat already has {running} workers running, the most it may have. Wait for one to finish, or stop one.");
        }

        var parentState = await new ConversationPersistenceService(parentStateId).LoadStateAsync().ConfigureAwait(false);
        var id = Guid.NewGuid().ToString("N")[..8];
        var worker = new BackgroundWorker
        {
            Id = id,
            ParentStateId = parentStateId,
            WorkerStateId = BackgroundWorkerInfo.WorkerStatePrefix + id,
            Title = title.Trim(),
            Brief = brief.Trim()
        };

        var directory = !string.IsNullOrWhiteSpace(parentState?.Directory) ? parentState!.Directory : parent.Directory;
        await SeedWorkerChatAsync(worker, parentState, parent, directory).ConfigureAwait(false);

        lock (_lock)
        {
            _workers.Add(worker);
        }

        Changed?.Invoke(worker);
        var parentName = string.IsNullOrWhiteSpace(parentState?.DisplayName) ? "Chat" : parentState!.DisplayName!;
        _ = Task.Run(() => RunAsync(runner, worker, directory, parentName));

        AutomatonDebug.Log($"BackgroundWorkerService.StartAsync: Exit - worker {worker.Id} started in {worker.WorkerStateId}, folder={directory}");
        return worker;
    }

    /// <summary>
    /// Removes a chat's finished workers from its list (the panel, and action=list). Running workers
    /// stay. Their chats are kept. Returns how many were removed.
    /// </summary>
    public int ClearFinished(string? parentStateId)
    {
        var key = BackgroundWorkerInfo.ChatKey(parentStateId);
        if (key.Length == 0)
        {
            return 0;
        }

        int removed;
        lock (_lock)
        {
            removed = _workers.RemoveAll(worker => !worker.IsRunning && BackgroundWorkerInfo.ChatKey(worker.ParentStateId) == key);
        }

        AutomatonDebug.Log($"BackgroundWorkerService.ClearFinished: {parentStateId} - removed {removed} finished worker(s)");
        if (removed > 0)
        {
            Changed?.Invoke(null);
        }

        return removed;
    }

    /// <summary>Stops a running worker. False when there is no such worker or it has already finished.</summary>
    public bool Stop(string? workerId)
    {
        var worker = Find(workerId);
        if (worker == null || !worker.IsRunning)
        {
            AutomatonDebug.Log($"BackgroundWorkerService.Stop: {workerId} - {(worker == null ? "not found" : "already " + worker.Status)}");
            return false;
        }

        AutomatonDebug.Log($"BackgroundWorkerService.Stop: stopping {worker.Id} '{worker.Title}'");
        worker.Activity = "Stopping…";
        worker.Cancellation.Cancel();
        Changed?.Invoke(worker);
        return true;
    }

    private static async Task SeedWorkerChatAsync(BackgroundWorker worker, ConversationState? parentState, ToolContext parent, string directory)
    {
        // The parent's own system prompt, so the worker knows the project and its tools the same
        // way, followed by what being a worker means.
        var parentPrompt = parentState?.Messages?.FirstOrDefault(message => message.Role == "system")?.Content;
        var systemPrompt = (string.IsNullOrWhiteSpace(parentPrompt) ? string.Empty : parentPrompt.Trim() + "\n\n") +
            "## You are a background worker\n" +
            "Another chat started you to do one piece of its work while it carries on. You see nothing of that chat " +
            "except the brief you are given. Do the whole task yourself, without stopping to check in - nobody is " +
            "watching this chat and there is no one to answer a question, so where you would ask, make the sensible " +
            "choice and say so. When you are done, end with a short report for the chat that started you: what you " +
            "did, what changed (files, commands, results), and anything left unresolved.";

        var state = new ConversationState
        {
            SessionId = Guid.NewGuid().ToString(),
            Agent = "background-worker",
            Directory = directory,
            Worktree = directory,
            ProjectId = parentState?.ProjectId ?? parent.ProjectId,
            DisplayName = $"Worker: {worker.Title}",
            SelectedProviderKey = parentState?.SelectedProviderKey,
            SelectedModelId = parentState?.SelectedModelId,
            ReasoningEffort = parentState?.ReasoningEffort,
            // Nobody is there to approve anything.
            FullPermissionMode = true,
            Messages = new List<Message>
            {
                new() { Id = Guid.NewGuid().ToString(), Role = "system", Content = systemPrompt }
            }
        };

        await new ConversationPersistenceService(worker.WorkerStateId).SaveStateAsync(state).ConfigureAwait(false);
        AutomatonDebug.Log($"BackgroundWorkerService.SeedWorkerChatAsync: {worker.WorkerStateId} seeded (provider={state.SelectedProviderKey ?? "default"}, model={state.SelectedModelId ?? "default"}, parentPrompt={(parentPrompt == null ? "none" : parentPrompt.Length + " chars")})");
    }

    private async Task RunAsync(IRemoteConversationService runner, BackgroundWorker worker, string directory, string parentName)
    {
        AutomatonDebug.Log($"BackgroundWorkerService.RunAsync: Entry - worker {worker.Id}");
        string? lastActivity = null;
        void Report(string activity)
        {
            if (activity == lastActivity)
            {
                return;
            }

            lastActivity = activity;
            worker.Activity = activity;
            Changed?.Invoke(worker);
        }

        try
        {
            var transcript = await runner.SendMessageWithStreamingAsync(
                worker.WorkerStateId,
                $"Worker: {worker.Title}",
                directory,
                worker.Brief,
                onTextDelta: _ => Report("Writing…"),
                onStreamEvent: streamEvent => Report(StreamEventDescriber.Describe(streamEvent)),
                askPermission: request =>
                {
                    AutomatonDebug.Log($"BackgroundWorkerService.RunAsync: worker {worker.Id} allowed '{request?.Permission}' without asking");
                    return Task.CompletedTask;
                },
                askQuestion: request =>
                {
                    AutomatonDebug.Log($"BackgroundWorkerService.RunAsync: worker {worker.Id} asked a question nobody can answer");
                    return Task.FromResult(QuestionResponse.Unanswered());
                },
                cancellationToken: worker.Cancellation.Token).ConfigureAwait(false);

            worker.Summary = ClosingReport(transcript);
            worker.Status = BackgroundWorkerStatus.Succeeded;
        }
        catch (OperationCanceledException) when (worker.Cancellation.IsCancellationRequested)
        {
            worker.Status = BackgroundWorkerStatus.Stopped;
            worker.Summary = "Stopped before it finished.";
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"BackgroundWorkerService.RunAsync: worker {worker.Id} failed - {ex.GetType().Name}: {ex.Message}");
            worker.Status = BackgroundWorkerStatus.Failed;
            worker.Summary = $"It failed: {ex.Message}";
        }

        worker.FinishedAt = DateTimeOffset.UtcNow;
        worker.Activity = worker.Status.ToString();
        Changed?.Invoke(worker);
        AutomatonDebug.Log($"BackgroundWorkerService.RunAsync: worker {worker.Id} {worker.Status} after {(worker.FinishedAt.Value - worker.StartedAt).TotalSeconds:0}s");

        await ReportToParentAsync(runner, worker, directory, parentName).ConfigureAwait(false);
        AutomatonDebug.Log($"BackgroundWorkerService.RunAsync: Exit - worker {worker.Id}");
    }

    /// <summary>
    /// Sends the result into the parent chat as a message, which starts a turn there so the chat can
    /// carry on with it. Mid-turn, the message waits for the turn to end.
    /// </summary>
    private async Task ReportToParentAsync(IRemoteConversationService runner, BackgroundWorker worker, string directory, string parentName)
    {
        var message =
            $"[Background worker {worker.Id} \"{worker.Title}\" {worker.Status.ToString().ToLowerInvariant()}]\n" +
            $"{worker.Summary}\n\n" +
            $"(This is the worker's report, not a message from the user. Its full chat is saved as {worker.WorkerStateId}.)";

        AutomatonDebug.Log($"BackgroundWorkerService.ReportToParentAsync: Entry - worker {worker.Id} -> {worker.ParentStateId} ({message.Length} chars)");

        // A message sent here, not typed, has to say for itself that the chat is busy and pass on
        // its activity, as a scheduled run does; otherwise the chat looks idle while it works.
        SystemEvents.RaiseConversationGenerationStatusChanged(new ConversationGenerationStatusEventInfo(worker.ParentStateId, true, "LLM busy"));
        try
        {
            await runner.SendMessageWithStreamingAsync(
                worker.ParentStateId,
                parentName,
                directory,
                message,
                onStreamEvent: streamEvent => SystemEvents.RaiseConversationStreamEvent(new ConversationStreamEventInfo(worker.ParentStateId, streamEvent)),
                askPermission: request =>
                {
                    AutomatonDebug.Log($"BackgroundWorkerService.ReportToParentAsync: parent turn allowed '{request?.Permission}' without asking");
                    return Task.CompletedTask;
                },
                askQuestion: _ => Task.FromResult(QuestionResponse.Unanswered())).ConfigureAwait(false);

            worker.Reported = true;
            AutomatonDebug.Log($"BackgroundWorkerService.ReportToParentAsync: Exit - worker {worker.Id} reported");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogError($"BackgroundWorkerService.ReportToParentAsync: worker {worker.Id} could not report to {worker.ParentStateId} - {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            SystemEvents.RaiseConversationGenerationStatusChanged(new ConversationGenerationStatusEventInfo(worker.ParentStateId, false, "Ready"));
            Changed?.Invoke(worker);
        }
    }

    /// <summary>The worker's last words with any text in them, without its thinking, capped.</summary>
    internal static string ClosingReport(RemoteConversationTranscript? transcript)
    {
        var last = transcript?.Messages?
            .Where(message => message.Role == "assistant")
            .Select(message => ThinkBlock.Replace(message.Content ?? string.Empty, string.Empty).Trim())
            .LastOrDefault(text => text.Length > 0);

        if (string.IsNullOrWhiteSpace(last))
        {
            return "It finished without writing a report.";
        }

        return last.Length > 6000 ? last[..6000] + "…" : last;
    }
}
