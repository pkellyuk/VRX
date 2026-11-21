using System;
using System.Collections.Generic;

namespace Automaton.Core.Services.Goals;

/// <summary>
/// The goal does not exist, or exists in another project. The two are deliberately
/// indistinguishable to the caller so that ids cannot be probed (GOALSUPDATE.md §5.0.1).
/// </summary>
public sealed class GoalNotFoundException : Exception
{
    public GoalNotFoundException(string goalId)
        : base($"Goal not found: {goalId}")
    {
        GoalId = goalId;
    }

    public string GoalId { get; }
}

/// <summary>Someone else wrote first and the change cannot be merged safely.</summary>
public sealed class GoalConflictException : Exception
{
    public GoalConflictException(string message)
        : base(message)
    {
    }
}

/// <summary>
/// A rule refused the change. The message is written for the model (or the human) to act
/// on, so callers show it as it is.
/// </summary>
public class GoalRuleException : Exception
{
    public GoalRuleException(string message)
        : base(message)
    {
    }
}

/// <summary>The completion gate refused. <see cref="Outstanding"/> names what is left.</summary>
public sealed class GoalGateRefusedException : GoalRuleException
{
    public GoalGateRefusedException(IReadOnlyList<string> outstanding)
        : base("The goal cannot be completed yet. Outstanding: " + string.Join("; ", outstanding))
    {
        Outstanding = outstanding;
    }

    public IReadOnlyList<string> Outstanding { get; }
}
