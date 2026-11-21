using Automaton.Core.Interfaces;
using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Carries images produced by a tool back to the model.
///
/// A tool cannot simply return its image: the OpenAI-compatible chat schema only
/// accepts an <c>image_url</c> content part on a <b>user</b> message, and
/// llama-server answers 400 rather than degrading when one appears on a tool
/// result. So the agent loop appends the ordinary text tool result, and then a
/// synthetic user message carrying the image.
///
/// The synthetic message says plainly what it is, because the alternative is a
/// model that believes the operator just sent it a screenshot unprompted.
/// </summary>
public static class ToolImageMessages
{
    /// <summary>
    /// Part type marking a message as tool-produced imagery rather than something
    /// the operator sent. Not <c>"tool"</c>: parts of that type are scanned for
    /// tool-call pairing, and a synthetic user message must never join that dance.
    /// </summary>
    public const string PartType = "tool-image";

    /// <summary>
    /// How many tool images stay in the conversation. A grid descent produces one
    /// image per step and each is a full screenshot, so without a cap a handful of
    /// clicks will exhaust the provider's context window on stale views the model
    /// has already navigated past.
    /// </summary>
    public const int DefaultRetainedImages = 2;

    /// <summary>
    /// True when the result has at least one usable image attachment.
    /// </summary>
    public static bool CarriesImages(ToolResult? result)
    {
        if (result is null)
        {
            return false;
        }

        return result.Attachments.Any(IsUsableImage);
    }

    /// <summary>
    /// Builds the synthetic user message for a tool result's images, or null when
    /// the result has none.
    /// </summary>
    /// <param name="toolName">Tool that produced the images, named in the message text.</param>
    /// <param name="callId">Tool call id, for correlating the image with its call in logs.</param>
    /// <param name="result">The tool result. Image attachments carry raw base64 in
    /// <see cref="ToolAttachment.Content"/> — no <c>data:</c> prefix.</param>
    public static Message? Create(string toolName, string? callId, ToolResult? result)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Create: Entry - tool={toolName}, callId={callId ?? "null"}, attachments={result?.Attachments.Count ?? 0}");

        if (string.IsNullOrWhiteSpace(toolName))
        {
            throw new ArgumentException("Tool name is required.", nameof(toolName));
        }

        if (result is null)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[ToolImageMessages] Create: Exit - null result");
            return null;
        }

        var images = result.Attachments.Where(IsUsableImage).ToList();
        if (images.Count == 0)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Create: Exit - tool={toolName} returned no image attachments");
            return null;
        }

        var message = new Message
        {
            Id = Guid.NewGuid().ToString(),
            Role = "user",
            Content = BuildIntroText(toolName, images.Count)
        };

        var totalBytes = 0L;

        foreach (var image in images)
        {
            var byteSize = EstimateDecodedSize(image.Content);
            totalBytes += byteSize;

            message.Attachments.Add(new ChatAttachment
            {
                FileName = string.IsNullOrWhiteSpace(image.Filename) ? $"{toolName}.png" : image.Filename,
                MediaType = image.ContentType,
                Kind = ChatAttachmentKind.Image,
                Base64Data = image.Content,
                ByteSize = byteSize
            });
        }

        message.Parts.Add(new MessagePart
        {
            Id = Guid.NewGuid().ToString(),
            Type = PartType,
            Tool = toolName,
            Metadata = new Dictionary<string, object>
            {
                ["callId"] = callId ?? string.Empty,
                ["imageCount"] = images.Count,
                ["byteSize"] = totalBytes
            }
        });

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Create: Exit - tool={toolName}, {images.Count} image(s), {totalBytes} bytes total");
        return message;
    }

    /// <summary>
    /// Strips the image payload out of all but the most recent
    /// <paramref name="retain"/> tool-image messages, leaving a text stub in place.
    ///
    /// The messages themselves stay: removing them would renumber nothing but would
    /// leave the model's own replies referring to views that no longer exist. A stub
    /// keeps the narrative intact at a few tokens instead of a few thousand.
    /// </summary>
    /// <returns>Number of messages whose images were dropped.</returns>
    public static int Prune(IList<Message>? messages, int retain = DefaultRetainedImages)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Prune: Entry - messages={messages?.Count ?? 0}, retain={retain}");

        if (messages is null)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, "[ToolImageMessages] Prune: Exit - null message list");
            return 0;
        }

        if (retain < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(retain), retain, "Retain count must not be negative.");
        }

        var carrying = new List<int>();

        for (var i = 0; i < messages.Count; i++)
        {
            if (IsToolImageMessage(messages[i]) && messages[i].Attachments.Count > 0)
            {
                carrying.Add(i);
            }
        }

        var pruneCount = carrying.Count - retain;
        if (pruneCount <= 0)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Prune: Exit - {carrying.Count} tool image message(s), nothing to prune");
            return 0;
        }

        for (var i = 0; i < pruneCount; i++)
        {
            var message = messages[carrying[i]];
            var toolName = message.Parts.FirstOrDefault(p => p.Type == PartType)?.Tool ?? "tool";

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Prune: Dropping {message.Attachments.Count} image(s) from message index {carrying[i]} (tool={toolName})");

            message.Attachments.Clear();
            message.Content = $"[image from {toolName} removed — superseded by a later view]";
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[ToolImageMessages] Prune: Exit - pruned {pruneCount} message(s), kept the most recent {retain}");
        return pruneCount;
    }

    /// <summary>
    /// Moves tool image messages that sit among a block of tool results to just after that
    /// block, keeping their order.
    ///
    /// An assistant message's tool calls must be answered by consecutive tool messages:
    /// DeepSeek and OpenAI answer 400 ("insufficient tool messages following tool_calls")
    /// when anything else comes between them. With one call per message the image always
    /// followed the only result, so it never showed; two screenshots in one message put the
    /// first image between the two results. Only messages this class created are moved -
    /// a real user message is never reordered.
    /// </summary>
    /// <returns>How many image messages were moved.</returns>
    public static int KeepToolResultsTogether(IList<Message>? messages)
    {
        if (messages is null || messages.Count < 3)
        {
            return 0;
        }

        var moved = 0;
        var i = 0;
        while (i < messages.Count)
        {
            if (!IsToolMessage(messages[i]))
            {
                i++;
                continue;
            }

            // A run of tool results and tool images, starting at the first result.
            var end = i;
            while (end < messages.Count && (IsToolMessage(messages[end]) || IsToolImageMessage(messages[end])))
            {
                end++;
            }

            var run = new List<Message>();
            for (var k = i; k < end; k++)
            {
                run.Add(messages[k]);
            }

            var lastResult = run.FindLastIndex(IsToolMessage);
            var misplaced = run.Take(lastResult).Count(IsToolImageMessage);
            if (misplaced > 0)
            {
                var ordered = run.Where(IsToolMessage).Concat(run.Where(message => !IsToolMessage(message))).ToList();
                for (var k = 0; k < ordered.Count; k++)
                {
                    messages[i + k] = ordered[k];
                }

                moved += misplaced;
                AutomatonDebug.Log($"[ToolImageMessages] KeepToolResultsTogether: moved {misplaced} image message(s) after the tool results at index {i}");
            }

            i = end;
        }

        return moved;
    }

    private static bool IsToolMessage(Message? message) =>
        message is not null && string.Equals(message.Role, "tool", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// True when the message is one this class created.
    /// </summary>
    public static bool IsToolImageMessage(Message? message)
    {
        if (message is null)
        {
            return false;
        }

        return message.Parts.Any(part => part.Type == PartType);
    }

    private static bool IsUsableImage(ToolAttachment? attachment)
    {
        if (attachment is null)
        {
            return false;
        }

        if (string.IsNullOrEmpty(attachment.Content))
        {
            return false;
        }

        return attachment.ContentType.StartsWith("image/", StringComparison.OrdinalIgnoreCase);
    }

    private static string BuildIntroText(string toolName, int imageCount)
    {
        var noun = imageCount == 1 ? "image" : $"{imageCount} images";

        return
            $"[tool output] The {noun} attached to this message came from the `{toolName}` tool call above, not from the user. " +
            "It is delivered this way because the chat format cannot carry an image on a tool result. " +
            "Read it as that tool's output and continue the task.";
    }

    /// <summary>
    /// Decoded byte count of a base64 payload, worked out from its length rather
    /// than by decoding it — this runs on every image and the value is only ever
    /// displayed.
    /// </summary>
    private static long EstimateDecodedSize(string? base64)
    {
        if (string.IsNullOrEmpty(base64))
        {
            return 0;
        }

        var padding = 0;

        if (base64.EndsWith("==", StringComparison.Ordinal))
        {
            padding = 2;
        }
        else if (base64.EndsWith("=", StringComparison.Ordinal))
        {
            padding = 1;
        }

        return ((long)base64.Length * 3 / 4) - padding;
    }
}
