using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Models;
using Automaton.Core.Models.Goals;
using Automaton.Core.Services.Goals;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private const string GoalProject = VrexChatStore.Project;
    private readonly Lazy<GoalTrackingService> goalStore = new(() =>
    {
        var service = new GoalTrackingService { ChecksExecutable = false };
        service.LoopReconcileAtStartup();
        return service;
    });
    private CancellationTokenSource? goalRun;
    private Task? goalWork;
    private bool performingGoalTurn;
    private bool goalTurnFailed;
    private volatile string? currentGoalId;
    private bool goalRunSpeaks;
    private bool goalRunSelectedOnly;
    private string goalStopReason = "Goal work stopped.";
    private readonly object goalReports = new();
    internal const string GoalInstruction =
        "Use the task_list for the checklist of the current turn or job. Use goal for durable, explicitly requested objectives that span multiple turns. " +
        "A goal tracks the outcome and its steps across chats. Read it before continuing, record actual progress, and report blockers honestly. " +
        "If the user asks to resume, list the goals and inspect their blockers, then use resume on the relevant goal once its blockers have been addressed.";

    private void ReportGoals()
    {
        lock (goalReports)
        {
            emit(new("goals-start"));
            foreach (var item in goalStore.Value.ListGoals(GoalCaller.ForHuman(GoalProject)))
                emit(new("goal-item", Goal: Summarize(item.Goal, item.Progress)));
            emit(new("goals-ready"));
        }
    }

    private void ReportGoal(string? id)
    {
        if (string.IsNullOrWhiteSpace(id)) return;
        try
        {
            var goal = goalStore.Value.GetGoal(GoalCaller.ForHuman(GoalProject), id);
            emit(new("goal-detail", Goal: Summarize(goal, GoalProgress.For(goal), detail: true)));
        }
        catch (GoalNotFoundException) { emit(new("goal-status", "That goal is no longer available.")); }
    }

    private void AcceptGoal(string id)
    {
        try
        {
            var service = goalStore.Value;
            var caller = GoalCaller.ForHuman(GoalProject);
            service.AcceptReview(caller, id);
            emit(new("goals-changed"));
        }
        catch (Exception ex) when (ex is GoalRuleException or GoalNotFoundException or GoalConflictException)
        { emit(new("goal-status", ex.Message)); }
    }

    private async Task PauseGoalAsync(string id)
    {
        try
        {
            var service = goalStore.Value;
            var caller = GoalCaller.ForHuman(GoalProject);
            var goal = service.GetGoal(caller, id);
            if (goal.Status != GoalStatus.Active) return;
            if (goalWork is { IsCompleted: false } running && currentGoalId == id)
            {
                StopGoalRun();
                await running;
            }
            if (service.GetGoal(caller, id).Status == GoalStatus.Active)
                service.SetGoalStatus(caller, id, GoalStatus.Backlog, "Paused by the user.");
            ForgetHeldGoal(id);
            emit(new("goal-status", "Goal paused. You can resume it from Goals."));
            emit(new("goals-changed"));
        }
        catch (Exception ex) when (ex is GoalRuleException or GoalNotFoundException or GoalConflictException or IOException)
        { emit(new("goal-status", ex.Message)); }
    }

    private void ResumeSelectedGoal(string id, bool speak)
    {
        if (goalWork is { IsCompleted: false })
        {
            emit(new("goal-status", "Pause the current goal before resuming another."));
            return;
        }
        if (string.IsNullOrWhiteSpace(settings.Model))
        {
            emit(new("goal-status", "Choose a model in Vrex Chat Settings first."));
            return;
        }
        try
        {
            ResumeGoal(goalStore.Value, GoalCaller.ForHuman(GoalProject), id);
            ForgetHeldGoal(id);
            emit(new("goals-changed"));
            StartGoalRun(speak, id, selectedOnly: true);
        }
        catch (Exception ex) when (ex is GoalRuleException or GoalNotFoundException or GoalConflictException or IOException)
        { emit(new("goal-status", ex.Message)); }
    }

    private void ForgetHeldGoal(string id)
    {
        bool changed = false;
        foreach (var entry in heldChatWork.Where(item => item.Value.GoalId == id).ToArray())
            changed |= heldChatWork.TryRemove(entry.Key, out _);
        if (changed) ReportChats(includeTranscript: false);
    }

    private static void ResumeGoal(GoalTrackingService service, GoalCaller caller, string id)
    {
        var goal = service.GetGoal(caller, id);
        if (goal.Status is not (GoalStatus.Active or GoalStatus.Blocked or GoalStatus.Backlog))
            throw new GoalRuleException("Only an active, blocked, or backlog goal can be retried.");
        foreach (var step in goal.Tasks.Where(step => step.Status == GoalTaskStatus.Blocked))
            service.SetTaskStatus(caller, id, step.ShortId, GoalTaskStatus.Todo, note: step.BlockedReason);
        if (!goal.IsActive || goal.Status == GoalStatus.Blocked)
            service.SetGoalStatus(caller, id, GoalStatus.Active);
    }

    private static VrexGoal Summarize(TrackedGoal goal, GoalProgress progress, bool detail = false) =>
        new(goal.Id, goal.Title, goal.Status switch
        {
            GoalStatus.AwaitingReview => "Awaiting review",
            _ => goal.Status.ToString()
        }, progress.Done, progress.Total, detail ? Trim(goal.Description, 2000) : null,
            detail ? goal.Tasks.Take(35).Select(step =>
                new VrexTaskStep($"{step.ShortId}  {Trim(step.Description, 180)}", step.Status.ToString(),
                    step.BlockedReason is { Length: > 0 } blocked ? Trim(blocked, 500) : null)).ToArray() : null,
            detail ? (goal.Tasks.FirstOrDefault(task => task.Status == GoalTaskStatus.Doing) ?? progress.NextRunnable)?.ShortId : null);

    private static string Trim(string text, int maximum) => text.Length > maximum ? text[..maximum] + "…" : text;

    private static JsonSchema GoalSchema() => new()
    {
        Description = GoalInstruction + " List and view read goals. Create records an objective and steps. Add_step extends the plan. Task records progress, finish requests completion, block records a reason, and resume requeues blocked steps in the selected goal.",
        Properties = new()
        {
            ["action"] = new() { Type = "string", Enum = ["list", "view", "create", "add_step", "task", "finish", "block", "resume"] },
            ["id"] = new() { Type = "string", Description = "Goal ID for actions on an existing goal." },
            ["title"] = new() { Type = "string", Description = "Short goal name for create." },
            ["description"] = new() { Type = "string", Description = "Desired outcome for create or description for add_step." },
            ["steps"] = new() { Type = "array", Items = new() { Type = "string" }, Description = "Optional concrete steps for create." },
            ["step"] = new() { Type = "string", Description = "Goal step reference, such as T1, for task." },
            ["status"] = new() { Type = "string", Enum = ["Todo", "Doing", "Done", "Blocked"], Description = "New step status for task." },
            ["note"] = new() { Type = "string", Description = "Evidence for what changed, or the reason progress is blocked." }
        },
        Required = ["action"]
    };

    private ToolResult UseGoal(string arguments, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        if (!chatOpen) return Output("Open Vrex Chat first.");
        using var document = JsonDocument.Parse(arguments);
        var args = document.RootElement;
        string? Field(string name) => args.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()?.Trim() : null;
        string id = Field("id") ?? "";
        var service = goalStore.Value;
        var caller = GoalCaller.ForModel(GoalProject, ChatId).WithAvailableTools(["goal"]);
        string? note = Field("note");
        try
        {
            string? action = Field("action");
            switch (action)
            {
                case "list":
                    return Output(JsonSerializer.Serialize(service.ListGoals(caller).Take(40)
                        .Select(item => Summarize(item.Goal, item.Progress))));
                case "view":
                    var viewed = service.GetGoal(caller, id);
                    return Output(JsonSerializer.Serialize(Summarize(viewed, GoalProgress.For(viewed), detail: true)));
                case "create":
                    if (performingGoalTurn) return Output("Create additional goals from chat after the current goal run.");
                    if (Field("description") is not { Length: > 0 and <= 3000 } description)
                        throw new ArgumentException("Provide a goal description of 1–3,000 characters.");
                    var draft = new GoalDraft { Title = Field("title"), Description = description };
                    if (args.TryGetProperty("steps", out var steps) && steps.ValueKind == JsonValueKind.Array)
                        foreach (var step in steps.EnumerateArray().Take(25))
                            if (step.ValueKind == JsonValueKind.String && step.GetString() is { Length: > 0 } text)
                                draft.Tasks.Add(new GoalTaskDraft { Description = Trim(text, 350) });
                    id = service.CreateGoal(caller, draft).Id;
                    break;
                case "add_step":
                    if (Field("description") is not { Length: > 0 and <= 350 } stepDescription)
                        throw new ArgumentException("Provide a step description of 1–350 characters.");
                    service.AddTask(caller, id, stepDescription);
                    break;
                case "task":
                    var newStatus = Field("status") switch
                    {
                        "Todo" => GoalTaskStatus.Todo, "Doing" => GoalTaskStatus.Doing, "Done" => GoalTaskStatus.Done,
                        "Blocked" => GoalTaskStatus.Blocked, _ => throw new ArgumentException("Choose a valid goal step status.")
                    };
                    service.SetTaskStatus(caller, id, Field("step") ?? "", newStatus, note: note,
                        evidence: newStatus == GoalTaskStatus.Done ? note : null, blockedReason: newStatus == GoalTaskStatus.Blocked ? note : null);
                    break;
                case "finish":
                    service.SetGoalStatus(caller, id, GoalStatus.Completed, note);
                    break;
                case "block":
                    if (string.IsNullOrWhiteSpace(note)) throw new ArgumentException("Explain what is blocking the goal.");
                    service.SetGoalStatus(caller, id, GoalStatus.Blocked, note);
                    break;
                case "resume":
                    if (performingGoalTurn) return Output("Ask the user to address the blocker before resuming.");
                    ResumeGoal(service, caller, id);
                    break;
                default: throw new ArgumentException("Choose a goal action.");
            }
            var updated = service.GetGoal(caller, id);
            emit(new("goals-changed"));
            return Output(JsonSerializer.Serialize(Summarize(updated, GoalProgress.For(updated), detail: true)));
        }
        catch (Exception ex) when (ex is GoalRuleException or GoalNotFoundException or GoalConflictException or ArgumentException)
        { return Output(ex.Message); }

        static ToolResult Output(string value) => new() { Title = "Goals", Output = value };
    }

    private void StartGoalRun(bool speak, string? resumeGoalId = null, bool selectedOnly = false)
    {
        if (goalWork is { IsCompleted: false }) return;
        if (string.IsNullOrWhiteSpace(settings.Model))
        {
            emit(new("goal-run", "Choose a model in Vrex Chat Settings first."));
            return;
        }
        StopLiveVoice();
        observationCancellation?.Cancel();
        goalRun?.Dispose();
        goalRun = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
        var token = goalRun.Token;
        string chatId = ChatId;
        goalRunSpeaks = speak;
        goalRunSelectedOnly = selectedOnly;
        currentGoalId = selectedOnly ? resumeGoalId : null;
        var heldElsewhere = heldChatWork.Where(item => item.Key != chatId && item.Value.GoalId != null)
            .Select(item => item.Value.GoalId!).ToHashSet(StringComparer.Ordinal);
        emit(new("goal-run", "Working on active goals…", X: 1));
        goalWork = Task.Run(() => RunGoalsAsync(chatId, speak, token, resumeGoalId, heldElsewhere, selectedOnly));
    }

    private void StopGoalRun()
    {
        goalStopReason = heldChatWork.TryGetValue(ChatId, out var held) && held.Goals ? "Goal work is on hold." : "Goal work stopped.";
        goalRun?.Cancel();
        lock (turnGate)
        {
            if (performingGoalTurn) { turn?.Cancel(); chat?.Control.Resume(); }
        }
    }

    private async Task RunGoalsAsync(string chatId, bool speak, CancellationToken token, string? resumeGoalId,
        HashSet<string> heldElsewhere, bool selectedOnly)
    {
        string status = "Active goals are complete.";
        GoalTrackingService? service = null;
        var human = GoalCaller.ForHuman(GoalProject, chatId);
        try
        {
            service = goalStore.Value;
            if (conversation is { IsCompleted: false } running)
            {
                emit(new("goal-activity", "Waiting for the current chat turn to finish"));
                await running.WaitAsync(token);
            }
            var pending = service.ListGoals(human, GoalStatus.Active)
                .Where(item => !heldElsewhere.Contains(item.Goal.Id) && (!selectedOnly || item.Goal.Id == resumeGoalId))
                .OrderByDescending(item => item.Goal.Id == resumeGoalId).ToArray();
            foreach (var item in pending)
            {
                token.ThrowIfCancellationRequested();
                currentGoalId = item.Goal.Id;
                var current = service.ListGoals(human, GoalStatus.Active).FirstOrDefault(entry => entry.Goal.Id == item.Goal.Id)?.Goal;
                if (current == null) continue;
                service.Bind(human, chatId, item.Goal.Id);
                var decision = service.LoopStart(chatId, GoalProject, "", startedByHuman: true);
                emit(new("goal-current", Goal: Summarize(current, GoalProgress.For(current), detail: true)));
                while (decision.Proceed)
                {
                    token.ThrowIfCancellationRequested();
                    decision = service.LoopBeforeIteration(chatId);
                    if (!decision.Proceed || decision.GoalId != item.Goal.Id) break;
                    var goal = service.GetGoal(human, item.Goal.Id);
                    var progress = Summarize(goal, GoalProgress.For(goal), detail: true);
                    string prompt = "Continue the user-authorized goal below. Treat its text as the scope of this job. " +
                        "Work on ordinary tasks through the existing tools and permissions. If blocked or if new approval is needed, record the blocker with the goal tool and ask the user. " +
                        "Use the goal tool to record evidence and progress. Use task_list for the current checklist. " +
                        "Request goal completion when the work meets its criteria. Goal: " + JsonSerializer.Serialize(progress);
                    Task turnTask;
                    lock (turnGate)
                    {
                        token.ThrowIfCancellationRequested();
                        if (chatId != ChatId) return;
                        turnTask = BeginTurn(prompt, speak, null, fromMicrophone: false, automatedGoal: true);
                    }
                    await turnTask;
                    token.ThrowIfCancellationRequested();
                    if (goalTurnFailed || Volatile.Read(ref awaitingChoice).Length > 0)
                    {
                        status = goalTurnFailed ? "Goal work stopped after a failed turn." : "Goal work paused for your answer.";
                        return;
                    }
                    if (service.GetGoal(human, item.Goal.Id).Status == GoalStatus.Blocked)
                    {
                        decision = GoalLoopDecision.Halt($"“{item.Goal.Title}” is blocked.");
                        break;
                    }
                    decision = service.LoopAfterIteration(chatId, error: null);
                }
                if (service.GetGoal(human, item.Goal.Id).Status == GoalStatus.Active)
                {
                    status = decision.StopReason ?? "Goal work paused. Review its progress before restarting.";
                    return;
                }
            }
            var remaining = service.ListGoals(human);
            var blockedCount = remaining.Count(item => item.Goal.Status == GoalStatus.Blocked);
            var reviewCount = remaining.Count(item => item.Goal.Status == GoalStatus.AwaitingReview);
            if (blockedCount + reviewCount > 0)
                status = $"Goal work paused. {blockedCount} blocked · {reviewCount} awaiting review. Select a goal for details.";
            else if (pending.Length == 0) status = heldElsewhere.Count > 0 ?
                "Active work is on hold in another chat." : "Create or activate a goal first.";
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { status = goalStopReason; }
        catch (Exception ex) when (ex is GoalNotFoundException or GoalConflictException or GoalRuleException or InvalidOperationException or IOException or ArgumentException)
        { status = "Goal work stopped. " + ex.Message; }
        finally
        {
            performingGoalTurn = false;
            currentGoalId = null;
            service?.LoopStop(chatId, status);
            emit(new("goal-run", status));
            emit(new("goals-changed"));
        }
    }

    private async Task DeleteGoalAsync(string id)
    {
        try
        {
            var service = goalStore.Value;
            var caller = GoalCaller.ForHuman(GoalProject);
            _ = service.GetGoal(caller, id);
            if (goalWork is { IsCompleted: false } running)
            {
                StopGoalRun();
                await running;
            }
            service.DeleteGoal(caller, id);
            ForgetHeldGoal(id);
            emit(new("goal-deleted", id));
            emit(new("goals-changed"));
        }
        catch (Exception ex) when (ex is GoalRuleException or GoalNotFoundException or GoalConflictException or IOException)
        { emit(new("goal-status", ex.Message)); }
    }
}
