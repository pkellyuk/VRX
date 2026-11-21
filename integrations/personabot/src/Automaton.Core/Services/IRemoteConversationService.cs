using Automaton.Core.Interfaces;
using Automaton.Core.Models;
using Automaton.Core.Streaming;

namespace Automaton.Core.Services;

public interface IRemoteConversationService
{
    Task<RemoteConversationTranscript> SendMessageAsync(
        string stateId,
        string displayName,
        string fallbackWorkingDirectory,
        string userMessage,
        CancellationToken cancellationToken = default);

    Task<RemoteConversationTranscript> SendMessageWithStreamingAsync(
        string stateId,
        string displayName,
        string fallbackWorkingDirectory,
        string userMessage,
        Action<string>? onTextDelta = null,
        Action<RemoteConversationStreamEvent>? onStreamEvent = null,
        Func<PermissionRequest, Task>? askPermission = null,
        Func<QuestionRequest, Task<QuestionResponse>>? askQuestion = null,
        string? calendarTaskId = null,
        RawStreamRecorder? rawStreamRecorder = null,
        IReadOnlyList<ChatAttachment>? attachments = null,
        CancellationToken cancellationToken = default);

    void ClearConversation(string stateId);

    /// <summary>
    /// Runs the named roles (all enabled ones when the list is empty) against the conversation
    /// as it stands, with no new user message, and lets the assistant act on what they say.
    /// The default declines, so a runner that cannot do this (a test double) needs no change.
    /// </summary>
    Task<RemoteConversationTranscript> RunRolesNowAsync(
        string stateId,
        string displayName,
        string fallbackWorkingDirectory,
        IReadOnlyList<string> roleKeys,
        Action<string>? onTextDelta = null,
        Action<RemoteConversationStreamEvent>? onStreamEvent = null,
        Func<PermissionRequest, Task>? askPermission = null,
        Func<QuestionRequest, Task<QuestionResponse>>? askQuestion = null,
        CancellationToken cancellationToken = default)
    {
        AutomatonDebug.LogWarning($"IRemoteConversationService.RunRolesNowAsync: default implementation declines - stateId={stateId}");
        throw new NotSupportedException("This conversation runner cannot run roles on demand.");
    }

    /// <summary>
    /// Predicts the user's next message for the chat box (SUGGESTREPLY.md). Read-only: takes no
    /// turn lease and saves nothing. The default declines, so a runner that cannot make one
    /// (a test double, a client with no model) needs no change.
    /// </summary>
    Task<ReplySuggestionBody> SuggestNextUserMessageAsync(string stateId, CancellationToken cancellationToken = default)
    {
        AutomatonDebug.Log($"IRemoteConversationService.SuggestNextUserMessageAsync: default implementation declines - stateId={stateId}");
        return Task.FromResult(ReplySuggestionBody.Skipped("this conversation runner does not make suggestions"));
    }

    /// <summary>
    /// Runs a turn for a guest who came in through an invite link (GUESTS.md). The message is
    /// saved with the guest's <c>[Guest Name]</c> tag, and the turn runs with no tools, no
    /// permission or question prompts, no roles and no hosted capabilities. The default
    /// declines, so a runner that cannot enforce that never runs a guest's turn at all.
    /// </summary>
    Task<RemoteConversationTranscript> SendGuestMessageAsync(
        string stateId,
        string displayName,
        string fallbackWorkingDirectory,
        Automaton.Core.Services.Guests.GuestTurn guest,
        string guestMessage,
        Action<string>? onTextDelta = null,
        Action<RemoteConversationStreamEvent>? onStreamEvent = null,
        CancellationToken cancellationToken = default)
    {
        AutomatonDebug.LogWarning($"IRemoteConversationService.SendGuestMessageAsync: default implementation declines - stateId={stateId}");
        throw new NotSupportedException("This conversation runner cannot run a guest's turn.");
    }

    /// <summary>
    /// Runs one group chat member's chat turn (MULTICHAT.md §3): the request is built by the
    /// caller from the channel, nothing is saved to any conversation, only the turn's read-only
    /// allowlist is offered, and <c>pass</c> ends it. The default declines, so a runner that
    /// cannot enforce the allowlist never runs one.
    /// </summary>
    Task<Automaton.Core.Services.GroupChat.GroupTurnResult> RunGroupTurnAsync(
        Automaton.Core.Services.GroupChat.GroupTurnRequest request,
        Action<string>? onTextDelta = null,
        Action<RemoteConversationStreamEvent>? onStreamEvent = null,
        CancellationToken cancellationToken = default)
    {
        AutomatonDebug.LogWarning($"IRemoteConversationService.RunGroupTurnAsync: default implementation declines - member={request?.MemberName}");
        throw new NotSupportedException("This conversation runner cannot run a group chat turn.");
    }
}
