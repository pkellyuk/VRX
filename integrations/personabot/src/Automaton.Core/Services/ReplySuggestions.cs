using Automaton.Core.Interfaces;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// The parts of a suggested next reply (SUGGESTREPLY.md) that do not need a model or a
/// database: whether a conversation is at a point worth suggesting for, the request that asks
/// for one, the clean-up of the answer, and whether thinking may be turned off for it.
/// The host that ran the turn does the I/O around these.
/// </summary>
public static class ReplySuggestions
{
    /// <summary>The longest suggestion kept. The instruction asks for 12 words or fewer.</summary>
    public const int MaxSuggestionCharacters = 200;

    /// <summary>Output budget when thinking has been turned off for the request.</summary>
    public const int MaxOutputTokensWithoutThinking = 96;

    /// <summary>Output budget when the model may think first, before the provider's own cap.</summary>
    public const int MaxOutputTokensWithThinking = 1024;

    /// <summary>What the model answers when there is no obvious next message.</summary>
    public const string NoSuggestionAnswer = "NONE";

    public const string Instruction =
        "[Automaton - this message is not from the user and is not part of the conversation.] " +
        "Predict the user's next message in this conversation: the thing they are most likely to type now that your reply is finished. " +
        "Write it as the user would, in their own voice and style, in 12 words or fewer. " +
        "Reply with only that message - no quotes, no label, no explanation - and do not call any tools. " +
        "If there is no obvious next message, reply with exactly " + NoSuggestionAnswer + ".";

    private static readonly string[] SpeakerPrefixes = ["User:", "Me:", "You:", "Next message:", "Suggestion:"];

    /// <summary>
    /// Why this conversation is not at a point worth suggesting for, or null when it is: the
    /// last message must be an assistant reply with visible text that did not end on a tool call.
    /// </summary>
    public static string? SkipReason(IReadOnlyList<Message>? messages)
    {
        if (messages == null || messages.Count == 0)
        {
            return "conversation is empty";
        }

        var last = messages[messages.Count - 1];
        if (last == null)
        {
            return "last message is missing";
        }

        if (!string.Equals(last.Role, "assistant", StringComparison.OrdinalIgnoreCase))
        {
            return $"last message is from '{last.Role}', not the assistant";
        }

        if ((last.Parts ?? []).Any(part => string.Equals(part?.Type, "tool", StringComparison.OrdinalIgnoreCase)))
        {
            return "turn ended on a tool call";
        }

        if (string.IsNullOrWhiteSpace(VisibleText(last)))
        {
            return "last reply has no visible text";
        }

        return null;
    }

    /// <summary>
    /// The suggestion request: a copy of exactly what the turn's last round trip sent, with the
    /// instruction appended last. <paramref name="requestMessages"/> must already carry the
    /// per-request context (the goal digest), so it stays on the same user message as before
    /// and the provider's prompt cache still matches everything up to the instruction.
    /// </summary>
    public static List<Message> BuildRequest(IReadOnlyList<Message> requestMessages)
    {
        if (requestMessages == null)
        {
            throw new ArgumentNullException(nameof(requestMessages));
        }

        var request = new List<Message>(requestMessages.Count + 1);
        request.AddRange(requestMessages);
        request.Add(new Message
        {
            Id = Guid.NewGuid().ToString(),
            Role = "user",
            Content = Instruction
        });

        return request;
    }

    /// <summary>
    /// The suggestion inside the model's answer, or null when there is none: the first
    /// non-empty line, without wrapping quotes or a speaker label, and no longer than
    /// <see cref="MaxSuggestionCharacters"/>.
    /// </summary>
    public static string? Clean(string? answer, out string? rejectedBecause)
    {
        rejectedBecause = null;
        if (string.IsNullOrWhiteSpace(answer))
        {
            rejectedBecause = "model answered with nothing";
            return null;
        }

        var line = answer
            .Replace("\r", string.Empty)
            .Split('\n')
            .Select(text => text.Trim())
            .FirstOrDefault(text => text.Length > 0);
        if (line == null)
        {
            rejectedBecause = "model answered with nothing";
            return null;
        }

        foreach (var prefix in SpeakerPrefixes)
        {
            if (line.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                line = line[prefix.Length..].Trim();
                break;
            }
        }

        line = StripWrappingQuotes(line);
        if (line.Length == 0)
        {
            rejectedBecause = "nothing left after removing quotes and labels";
            return null;
        }

        if (string.Equals(line.TrimEnd('.', '!'), NoSuggestionAnswer, StringComparison.OrdinalIgnoreCase))
        {
            rejectedBecause = "model said there is no obvious next message";
            return null;
        }

        if (line.Length > MaxSuggestionCharacters)
        {
            rejectedBecause = $"answer is {line.Length} characters, over the {MaxSuggestionCharacters} limit";
            return null;
        }

        return line;
    }

    /// <summary>
    /// Whether the suggestion request may carry thinking level "none". Only when the model's
    /// ladder has an off rung and the endpoint is known to take these fields: the chat already
    /// sends its own level, or the provider is configured with them. Anything else keeps the
    /// chat's own settings, because an endpoint that rejects unknown fields would fail the request.
    /// </summary>
    public static bool CanTurnThinkingOff(ProviderConfig? config, string? chatReasoningEffort)
    {
        if (config == null)
        {
            return false;
        }

        if (!ReasoningEffortSettings.LevelsFor(config).Contains(ReasoningEffortSettings.None))
        {
            return false;
        }

        if (ReasoningEffortSettings.Normalize(chatReasoningEffort) != null)
        {
            return true;
        }

        var parameters = config.RequestParameters;
        return parameters != null &&
               (parameters.ContainsKey("reasoning_effort") || parameters.ContainsKey("chat_template_kwargs"));
    }

    /// <summary>The output budget for the request, never above the provider's own cap.</summary>
    public static int OutputBudget(bool thinkingOff, int? providerMaxOutputTokens)
    {
        var budget = thinkingOff ? MaxOutputTokensWithoutThinking : MaxOutputTokensWithThinking;
        return providerMaxOutputTokens is > 0 ? Math.Min(budget, providerMaxOutputTokens.Value) : budget;
    }

    private static string VisibleText(Message message)
    {
        if (!string.IsNullOrWhiteSpace(message.Content))
        {
            return message.Content;
        }

        return string.Concat((message.Parts ?? [])
            .Where(part => string.Equals(part?.Type, "text", StringComparison.OrdinalIgnoreCase))
            .Select(part => part.Text ?? string.Empty));
    }

    private static string StripWrappingQuotes(string text)
    {
        var pairs = new (char Open, char Close)[] { ('"', '"'), ('\'', '\''), ('`', '`'), ('“', '”'), ('‘', '’') };
        var changed = true;
        while (changed && text.Length >= 2)
        {
            changed = false;
            foreach (var (open, close) in pairs)
            {
                if (text[0] == open && text[^1] == close)
                {
                    text = text[1..^1].Trim();
                    changed = true;
                    break;
                }
            }
        }

        return text;
    }
}
