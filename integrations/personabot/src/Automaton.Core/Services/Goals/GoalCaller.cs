using System;
using System.Collections.Generic;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

/// <summary>
/// Who is calling the goal tracking service, and for which project.
///
/// Built by the surface that owns the call: the tool layer builds a model caller from its
/// ToolContext, the desktop and the CLI build a human caller, the migrator and the Director
/// build a system caller. It is never deserialised from tool arguments, and no tool has an
/// author parameter - that is what makes "the human can override, the model cannot"
/// enforceable. See GOALSUPDATE.md §5.0.
/// </summary>
public sealed class GoalCaller
{
    private GoalCaller(GoalAuthor author, string? projectId, string? stateId, string? claimantId, GoalClaimKind claimKind)
    {
        Author = author;
        ProjectId = string.IsNullOrWhiteSpace(projectId) ? null : projectId.Trim();
        StateId = string.IsNullOrWhiteSpace(stateId) ? null : stateId.Trim();
        ClaimantId = string.IsNullOrWhiteSpace(claimantId) ? null : claimantId.Trim();
        ClaimKind = claimKind;
    }

    public GoalAuthor Author { get; }

    /// <summary>
    /// The project every goal id is resolved inside. Null only for a system caller, which
    /// resolves a goal by id alone (the migrator and restart reconciliation have no project).
    /// </summary>
    public string? ProjectId { get; }

    /// <summary>The conversation making the call, when there is one.</summary>
    public string? StateId { get; }

    /// <summary>"chat:{stateId}" or "worker:{directorGoalId}:{runId}". Null when the caller cannot hold a claim.</summary>
    public string? ClaimantId { get; }

    public GoalClaimKind ClaimKind { get; }

    /// <summary>The tool ids the calling chat really has, so messages never name one it lacks.</summary>
    public IReadOnlyCollection<string>? AvailableToolIds { get; private set; }

    public bool IsHuman => Author == GoalAuthor.Human;

    /// <summary>A model working in a chat. The chat is the claimant.</summary>
    public static GoalCaller ForModel(string projectId, string? stateId)
    {
        if (string.IsNullOrWhiteSpace(projectId))
        {
            throw new ArgumentException("A model caller must be scoped to a project.", nameof(projectId));
        }

        var claimant = string.IsNullOrWhiteSpace(stateId) ? null : $"chat:{stateId.Trim()}";
        return new GoalCaller(GoalAuthor.Model, projectId, stateId, claimant, GoalClaimKind.Chat);
    }

    /// <summary>A Director worker. Still a model for every proof and gate decision.</summary>
    public static GoalCaller ForWorker(string projectId, string stateId, string directorGoalId, string runId)
    {
        if (string.IsNullOrWhiteSpace(projectId))
        {
            throw new ArgumentException("A worker caller must be scoped to a project.", nameof(projectId));
        }

        if (string.IsNullOrWhiteSpace(directorGoalId))
        {
            throw new ArgumentException("directorGoalId is required.", nameof(directorGoalId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId is required.", nameof(runId));
        }

        return new GoalCaller(GoalAuthor.Model, projectId, stateId, $"worker:{directorGoalId.Trim()}:{runId.Trim()}", GoalClaimKind.Worker);
    }

    /// <summary>A person using the desktop, the CLI or a remote device.</summary>
    public static GoalCaller ForHuman(string projectId, string? stateId = null)
    {
        if (string.IsNullOrWhiteSpace(projectId))
        {
            throw new ArgumentException("A human caller must name the project of the view they are in.", nameof(projectId));
        }

        // A person can take a task too; every human surface shares one claimant.
        return new GoalCaller(GoalAuthor.Human, projectId, stateId, "human", GoalClaimKind.Chat);
    }

    /// <summary>The migrator, reconciliation and other code acting for nobody.</summary>
    public static GoalCaller ForSystem(string? projectId = null)
    {
        return new GoalCaller(GoalAuthor.System, projectId, null, null, GoalClaimKind.Chat);
    }

    public GoalCaller WithAvailableTools(IReadOnlyCollection<string>? toolIds)
    {
        AvailableToolIds = toolIds;
        return this;
    }

    /// <summary>True when the chat is known to lack <paramref name="toolId"/>.</summary>
    public bool Lacks(string toolId)
    {
        if (AvailableToolIds == null || string.IsNullOrWhiteSpace(toolId))
        {
            return false;
        }

        foreach (var id in AvailableToolIds)
        {
            if (string.Equals(id, toolId, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }

        return true;
    }

    public override string ToString()
    {
        return $"{Author}/project={ProjectId ?? "*"}/state={StateId ?? "-"}/claimant={ClaimantId ?? "-"}";
    }
}
