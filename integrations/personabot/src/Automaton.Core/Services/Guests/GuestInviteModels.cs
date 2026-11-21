using System;

namespace Automaton.Core.Services.Guests;

/// <summary>
/// Who is talking on a guest's turn. Set on <c>ToolContext.Guest</c> for the
/// length of that one turn; null on every other turn. See GUESTS.md.
/// </summary>
/// <param name="InviteId">The invite the guest came in through.</param>
/// <param name="GuestName">The name the owner gave them, already normalised.</param>
public sealed record GuestTurn(string InviteId, string GuestName);

/// <summary>Where an invite is in its life.</summary>
public enum GuestInviteStatus
{
    /// <summary>Made, and nobody has joined through it yet.</summary>
    Waiting,

    /// <summary>A browser has joined and its session is still good.</summary>
    Joined,

    /// <summary>Revoked, or past its expiry. Nothing reaches the chat through it.</summary>
    Ended,
}

/// <summary>
/// One stored invite. Only hashes of the link token and of the guest's cookie are
/// kept, so a copy of <c>config.db</c> lets nobody in.
/// </summary>
/// <param name="Id">Short random id, used by the owner's UI to name the invite.</param>
/// <param name="StateId">The chat (conversation state id) the guest is locked into.</param>
/// <param name="ChatName">The chat's name when the invite was made, for the guest's page.</param>
/// <param name="GuestName">Who the invite is for.</param>
/// <param name="TokenHash">SHA-256 of the link token.</param>
/// <param name="SessionHash">SHA-256 of the cookie handed to the browser that claimed it; null until claimed.</param>
/// <param name="ShowHistory">True when the guest may see the chat from before the invite.</param>
/// <param name="HistoryAnchorId">The chat's last message when the invite was made; the guest sees only what follows it.</param>
/// <param name="CreatedAt">When the invite was made.</param>
/// <param name="ExpiresAt">An unclaimed link lapses here; once claimed, the guest's session ends here.</param>
/// <param name="ClaimedAt">When a browser joined; null while waiting.</param>
/// <param name="ClaimedFrom">The address that joined, for the owner's list.</param>
/// <param name="RevokedAt">When the owner ended it; null while it stands.</param>
/// <param name="LastSeenAt">The guest's last request, for the owner's list.</param>
public sealed record GuestInviteRecord(
    string Id,
    string StateId,
    string ChatName,
    string GuestName,
    string TokenHash,
    string? SessionHash,
    bool ShowHistory,
    string? HistoryAnchorId,
    DateTimeOffset CreatedAt,
    DateTimeOffset ExpiresAt,
    DateTimeOffset? ClaimedAt,
    string? ClaimedFrom,
    DateTimeOffset? RevokedAt,
    DateTimeOffset? LastSeenAt)
{
    /// <summary>The invite's status at <paramref name="now"/>.</summary>
    public GuestInviteStatus StatusAt(DateTimeOffset now)
    {
        if (RevokedAt is not null || ExpiresAt <= now)
        {
            return GuestInviteStatus.Ended;
        }

        return ClaimedAt is null ? GuestInviteStatus.Waiting : GuestInviteStatus.Joined;
    }
}

/// <summary>A freshly made invite and the one copy of its link token that will ever exist.</summary>
/// <param name="Invite">The stored invite.</param>
/// <param name="Token">The link token. Not stored anywhere; lose it and the invite is unusable.</param>
public sealed record GuestInviteCreated(GuestInviteRecord Invite, string Token);

/// <summary>The outcome of a browser pressing Join.</summary>
/// <param name="Ok">True when this browser now holds the invite.</param>
/// <param name="SessionToken">The cookie value to set; null unless <paramref name="Ok"/>.</param>
/// <param name="Invite">The invite, when the token named one.</param>
/// <param name="Error">What to tell the person when it did not work.</param>
public sealed record GuestClaimResult(bool Ok, string? SessionToken, GuestInviteRecord? Invite, string? Error)
{
    public static GuestClaimResult Claimed(GuestInviteRecord invite, string sessionToken) => new(true, sessionToken, invite, null);

    public static GuestClaimResult Refused(GuestInviteRecord? invite, string error) => new(false, null, invite, error);
}

/// <summary>The outcome of a guest sending a message.</summary>
public readonly record struct GuestSendResult(bool Accepted, string? Reason)
{
    public static GuestSendResult Ok() => new(true, null);

    public static GuestSendResult Rejected(string reason) => new(false, reason);
}

/// <summary>One line of the guest's view of the chat.</summary>
/// <param name="Id">The message id, so the page can keep its place.</param>
/// <param name="Who"><c>you</c>, <c>guest</c> (another guest), <c>host</c> (the owner) or <c>assistant</c>.</param>
/// <param name="Name">The speaker's name for <c>you</c> and <c>guest</c>; empty otherwise.</param>
/// <param name="Text">What was said, tag and thinking removed.</param>
public sealed record GuestTranscriptEntry(string Id, string Who, string Name, string Text);
