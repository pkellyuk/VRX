using System;
using System.Collections.Generic;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services.RequestContext;

/// <summary>
/// Supplies text that belongs in one outgoing model request and nowhere else. It is rebuilt
/// for every request, never stored with the conversation, never indexed, and so never lost to
/// compaction - because it was never in the chat to begin with. See GOALSUPDATE.md §7.2.
/// </summary>
public interface IRequestContextProvider
{
    /// <summary>
    /// The text for this request, or null for none. <paramref name="toolIds"/> is the tool
    /// list actually being sent, so the text can avoid naming a tool this chat lacks.
    /// Must not throw for ordinary failures: a request is worth more than its decoration.
    /// </summary>
    string? BuildTransientContext(ToolContext context, IReadOnlyCollection<string> toolIds);
}

public static class RequestContextProviders
{
    /// <summary>
    /// Attaches <paramref name="text"/> to the end of the last user message, on a copy.
    ///
    /// <para>
    /// Why there and not as a trailing system message: the provider layer merges every system
    /// message into the first one, so a system block added at the tail would rewrite the head
    /// of the prompt, and a local server's prefix cache is only good up to the first byte that
    /// changed - every goal update would re-read the whole conversation. A new trailing user
    /// message would break templates that insist on alternating roles, and shifts the
    /// "last user message" index that reasoning replay depends on. Appending to the existing
    /// last user message moves no index, adds no message, leaves everything before it cached,
    /// and puts the status next to the most recent context.
    /// </para>
    /// </summary>
    public static List<Message> Attach(List<Message> messages, string text)
    {
        if (messages == null)
        {
            throw new ArgumentNullException(nameof(messages));
        }

        if (string.IsNullOrWhiteSpace(text))
        {
            return messages;
        }

        var copy = new List<Message>(messages);
        var lastUser = copy.FindLastIndex(message => string.Equals(message?.Role, "user", StringComparison.OrdinalIgnoreCase));

        if (lastUser < 0)
        {
            // A conversation with no user message at all (a seeded worker before its brief).
            copy.Add(new Message { Id = Guid.NewGuid().ToString(), Role = "system", Content = text });
            return copy;
        }

        var original = copy[lastUser];
        copy[lastUser] = new Message
        {
            Id = original.Id,
            Role = original.Role,
            Content = string.IsNullOrEmpty(original.Content) ? text : original.Content + "\n\n" + text,
            Parts = original.Parts,
            Attachments = original.Attachments
        };

        return copy;
    }
}
