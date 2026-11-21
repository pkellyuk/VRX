using System.Text.Json;
using Automaton.Core.Interfaces;

namespace Automaton.AI.Interfaces;

/// <summary>
/// Keeps a <c>/responses</c> reply's reasoning items on the assistant message, so the next request
/// of the turn can send them back. They ride in the reasoning part's metadata as one JSON string:
/// metadata is saved with the transcript and survives the round trip as a plain string, where a
/// typed list would come back as a <see cref="JsonElement"/> of whatever shape.
/// </summary>
public static class ProviderReasoningItems
{
    public const string MetadataKey = "openai_reasoning_items";

    /// <summary>Stores the reply's reasoning items on the message, adding a reasoning part if it has none.</summary>
    public static void Attach(Message message, AIResponse response)
    {
        if (message == null || response?.ReasoningItems == null || response.ReasoningItems.Count == 0)
        {
            return;
        }

        var part = message.Parts.FirstOrDefault(candidate => candidate.Type == "reasoning");
        if (part == null)
        {
            // GPT does not always send a readable summary; the part then carries only the items.
            part = new MessagePart { Id = Guid.NewGuid().ToString(), Type = "reasoning" };
            message.Parts.Insert(0, part);
        }

        part.Metadata[MetadataKey] = JsonSerializer.Serialize(response.ReasoningItems);
    }

    /// <summary>The reasoning items stored on a message, in the order the model produced them.</summary>
    public static IReadOnlyList<ProviderReasoningItem> Read(Message message)
    {
        var stored = message?.Parts
            .Where(part => part.Type == "reasoning")
            .Select(part => part.Metadata.TryGetValue(MetadataKey, out var value) ? value : null)
            .FirstOrDefault(value => value != null);

        var json = stored switch
        {
            string text => text,
            JsonElement { ValueKind: JsonValueKind.String } element => element.GetString(),
            _ => null
        };

        if (string.IsNullOrWhiteSpace(json))
        {
            return Array.Empty<ProviderReasoningItem>();
        }

        try
        {
            return JsonSerializer.Deserialize<List<ProviderReasoningItem>>(json) ?? new List<ProviderReasoningItem>();
        }
        catch (JsonException)
        {
            return Array.Empty<ProviderReasoningItem>();
        }
    }
}
