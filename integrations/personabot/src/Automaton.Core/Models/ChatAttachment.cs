using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

/// <summary>
/// How an attachment reaches the model.
///
/// The three kinds exist because a chat model can only ever receive text or images.
/// Everything else has to be turned into one of those two, and <see cref="Staged"/>
/// is the escape hatch for formats we cannot decode ourselves: the bytes are written
/// somewhere the agent's file tools can reach and the model is told the path.
/// </summary>
public static class ChatAttachmentKind
{
    /// <summary>Sent to the provider as an inline image content part.</summary>
    public const string Image = "image";

    /// <summary>Decoded to text up front and inlined into the user message.</summary>
    public const string Text = "text";

    /// <summary>Written to disk; the model gets the path and reads it with its own tools.</summary>
    public const string Staged = "staged";
}

/// <summary>
/// A file the user attached to a chat message.
///
/// One attachment carries at most one payload: <see cref="Base64Data"/> for images,
/// <see cref="TextContent"/> for decoded text, <see cref="StagedPath"/> for everything
/// else. Which one is populated follows from <see cref="Kind"/>.
/// </summary>
public sealed class ChatAttachment
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString();

    /// <summary>Original file name, shown in the UI and given to the model.</summary>
    [JsonPropertyName("fileName")]
    public string FileName { get; set; } = string.Empty;

    /// <summary>IANA media type, e.g. "image/png" or "application/pdf".</summary>
    [JsonPropertyName("mediaType")]
    public string MediaType { get; set; } = "application/octet-stream";

    /// <summary>One of the <see cref="ChatAttachmentKind"/> constants.</summary>
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = ChatAttachmentKind.Staged;

    /// <summary>Size of the original file in bytes, for display only.</summary>
    [JsonPropertyName("byteSize")]
    public long ByteSize { get; set; }

    /// <summary>Image bytes, already normalized. Base64, no data: prefix. Image kind only.</summary>
    [JsonPropertyName("base64Data")]
    public string? Base64Data { get; set; }

    /// <summary>Decoded file text. Text kind only.</summary>
    [JsonPropertyName("textContent")]
    public string? TextContent { get; set; }

    /// <summary>Absolute path the bytes were written to. Staged kind only.</summary>
    [JsonPropertyName("stagedPath")]
    public string? StagedPath { get; set; }

    /// <summary>
    /// Set when the attachment could not be fully prepared (unreadable file, oversized
    /// text, decode failure). The message still carries the attachment so the model is
    /// told what happened rather than silently seeing nothing.
    /// </summary>
    [JsonPropertyName("error")]
    public string? Error { get; set; }

    [JsonIgnore]
    public bool IsImage => string.Equals(Kind, ChatAttachmentKind.Image, StringComparison.Ordinal);

    /// <summary>Data URI form the OpenAI-compatible <c>image_url</c> content part expects.</summary>
    public string ToDataUri() => $"data:{MediaType};base64,{Base64Data}";
}
