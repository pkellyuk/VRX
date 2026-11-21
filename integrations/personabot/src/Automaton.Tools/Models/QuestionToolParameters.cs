using System.Text.Json.Serialization;

namespace Automaton.Tools.Models;

public class QuestionToolParameters
{
    [JsonPropertyName("question")]
    public string Question { get; set; } = string.Empty;

    [JsonPropertyName("options")]
    public List<QuestionOption>? Options { get; set; }

    [JsonPropertyName("multiSelect")]
    public bool MultiSelect { get; set; } = false;

    /// <summary>
    /// When true, the question is posted as a non-blocking alert to the Inbox instead of
    /// opening an interactive modal. Use this when no human is in the loop (e.g. a scheduled
    /// background check) or to surface "something happened, check this" from any conversation.
    /// </summary>
    [JsonPropertyName("broadcast")]
    public bool Broadcast { get; set; } = false;

    /// <summary>
    /// Optional short title/heading shown on the inbox alert. Only used when broadcast=true.
    /// </summary>
    [JsonPropertyName("title")]
    public string? Title { get; set; }
}

public class QuestionOption
{
    [JsonPropertyName("label")]
    public string Label { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("markdown")]
    public string? Markdown { get; set; }
}