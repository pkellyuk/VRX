using System;
using System.Collections.Generic;

namespace Automaton.Core.Services.GroupChat;

/// <summary>Who runs a member's turns. See MULTICHAT.md §2.</summary>
public enum GroupMemberKind
{
    /// <summary>A provider and model on this desktop; Automaton runs its turns.</summary>
    Model,

    /// <summary>An external MCP client (Claude Code, Codex); it runs its own turns.</summary>
    Agent
}

/// <summary>
/// The mode Paul sets with a member's button. Busy is not here: it is worked out from the
/// member's running assignment, so a crash can never leave a member stuck Busy (§5).
/// </summary>
public enum GroupMemberMode
{
    /// <summary>Takes part in discussion rounds, with read-only tools.</summary>
    Chat,

    /// <summary>Sees everything but is only asked to reply when Paul addresses it.</summary>
    Quiet
}

/// <summary>What a line in the channel is.</summary>
public enum GroupMessageKind
{
    /// <summary>Ordinary talk, from Paul or a member.</summary>
    Chat,

    /// <summary>A message of Paul's that handed out work. Carries the assignment ids it started.</summary>
    Assignment,

    /// <summary>Text a worker wrote while working on an assignment, mirrored from its work chat.</summary>
    Work,

    /// <summary>A tool call a worker made while working, mirrored from its work chat.</summary>
    Tool,

    /// <summary>How an assignment ended, posted when it ends.</summary>
    Outcome,

    /// <summary>A notice from Automaton itself (a member joined, a turn failed).</summary>
    System
}

/// <summary>Where an assignment stands.</summary>
public enum GroupAssignmentStatus
{
    Running,
    Done,
    Failed,
    Stopped,

    /// <summary>Was running when Automaton stopped; found that way on the next start.</summary>
    Interrupted
}

/// <summary>One channel.</summary>
/// <param name="RoundCap">How many discussion rounds one message of Paul's can start (§3.2).</param>
public sealed record GroupChatInfo(
    string Id,
    string Name,
    string Directory,
    int RoundCap,
    DateTimeOffset CreatedUtc,
    DateTimeOffset UpdatedUtc);

/// <summary>A participant other than Paul.</summary>
/// <param name="Name">Unique in the group, case-insensitively; what <c>@mentions</c> use.</param>
/// <param name="WorkStateId">The member's work chat, once it has been given work (§4.1).</param>
/// <param name="AgentTokenHash">SHA-256 of an agent member's MCP token; null for a model member.</param>
public sealed record GroupMemberInfo(
    string Id,
    string GroupId,
    string Name,
    GroupMemberKind Kind,
    string? ProviderKey,
    string? ModelId,
    string? ReasoningEffort,
    GroupMemberMode Mode,
    int Position,
    string? WorkStateId,
    string? AgentTokenHash,
    DateTimeOffset? LastSeenUtc);

/// <summary>One line in the channel.</summary>
/// <param name="Seq">Increases by one per group; the cursor pages and waits use.</param>
/// <param name="AuthorId">A member id, <see cref="GroupAuthors.Paul"/> or <see cref="GroupAuthors.System"/>.</param>
/// <param name="AssignmentId">For work, tool and outcome lines, the assignment they belong to; for an
/// assignment message, the ids it started, comma separated.</param>
/// <param name="Detail">Extra text that is shown folded: a tool's arguments and result.</param>
public sealed record GroupMessageInfo(
    string Id,
    string GroupId,
    long Seq,
    string AuthorId,
    GroupMessageKind Kind,
    string? AssignmentId,
    string Text,
    string? Detail,
    DateTimeOffset CreatedUtc);

/// <summary>A piece of work Paul gave one member.</summary>
public sealed record GroupAssignmentInfo(
    string Id,
    string GroupId,
    string MemberId,
    string MessageId,
    string Text,
    GroupAssignmentStatus Status,
    DateTimeOffset StartedUtc,
    DateTimeOffset? EndedUtc,
    string? Outcome);

/// <summary>The author ids that are not members.</summary>
public static class GroupAuthors
{
    public const string Paul = "paul";

    public const string System = "system";

    /// <summary>The name Paul's messages are tagged with when a member reads them.</summary>
    public const string PaulName = "Paul";
}

/// <summary>
/// Set on <c>ToolContext.GroupTurn</c> for a member's chat turn (MULTICHAT.md §3.5). Only the
/// allowed tools are offered and any other call is refused; <c>pass</c> ends the turn.
/// </summary>
/// <param name="AllowedToolIds">The read-only allowlist for this turn.</param>
/// <param name="MayPass">False in round 1 when Paul asked this member directly: it must answer.</param>
public sealed class GroupTurn
{
    public GroupTurn(string groupId, string memberName, IReadOnlyCollection<string> allowedToolIds, bool mayPass)
    {
        GroupId = groupId ?? throw new ArgumentNullException(nameof(groupId));
        MemberName = memberName ?? throw new ArgumentNullException(nameof(memberName));
        AllowedToolIds = allowedToolIds ?? throw new ArgumentNullException(nameof(allowedToolIds));
        MayPass = mayPass;
    }

    public string GroupId { get; }

    public string MemberName { get; }

    public IReadOnlyCollection<string> AllowedToolIds { get; }

    public bool MayPass { get; }

    /// <summary>Set by the turn processor when the model called <c>pass</c>.</summary>
    public bool Passed { get; set; }
}

/// <summary>What a member's chat turn is asked to do.</summary>
/// <param name="Messages">The whole request, system prompt first, built by <see cref="GroupTranscriptView"/>.</param>
public sealed record GroupTurnRequest(
    string GroupId,
    string MemberId,
    string MemberName,
    string ProviderKey,
    string ModelId,
    string? ReasoningEffort,
    string Directory,
    IReadOnlyList<Interfaces.Message> Messages,
    GroupTurn Turn);

/// <summary>How a member's chat turn went.</summary>
/// <param name="Text">The member's reply, reasoning stripped; empty when it passed.</param>
public sealed record GroupTurnResult(bool Passed, string Text)
{
    public static GroupTurnResult Pass() => new(true, string.Empty);

    public static GroupTurnResult Reply(string text) => new(false, text ?? string.Empty);
}
