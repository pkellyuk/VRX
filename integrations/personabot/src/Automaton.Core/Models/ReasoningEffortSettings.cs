using System.Text.Json;
using Automaton.Core.Services;

namespace Automaton.Core.Models;

/// <summary>
/// The per-chat thinking level, and how it reaches the model.
///
/// A chat stores nothing but a level string; the request fields are derived here so the
/// local chat path and the remote-driven one cannot drift apart. Null or blank means the
/// chat has never touched the control, and the provider's own configured parameters are
/// left exactly as the user set them in Settings.
///
/// llama-server reads <c>reasoning_effort: "none"</c> as "no thinking, and discard the
/// effort the server was started with", which is what makes an explicit Off meaningful on
/// a server launched with thinking on. The same value is repeated inside
/// <c>chat_template_kwargs</c> because some chat templates read it from there instead.
/// </summary>
public static class ReasoningEffortSettings
{
    public const string None = "none";
    public const string Low = "low";
    public const string Medium = "medium";
    public const string High = "high";

    /// <summary>
    /// A rung above <see cref="High"/>. llama.cpp interpolates the level into the model's
    /// Jinja chat template, so which levels mean anything is a property of that template
    /// rather than of any standard: templates that do not branch on <c>xhigh</c> simply
    /// ignore it. Automaton offers it because the servers this app is pointed at are
    /// started at it - see the shared-server note in AUTOMATONPLAN.md.
    /// </summary>
    public const string XHigh = "xhigh";

    /// <summary>
    /// DeepSeek's top rung. Their hosted API defines the ladder as none/low/high/max, so
    /// <see cref="Medium"/> and <see cref="XHigh"/> mean nothing there while max — the level
    /// that raises the output ceiling — had no way to be sent at all.
    /// </summary>
    public const string Max = "max";

    /// <summary>
    /// The order the chat-area control steps through on each click: rising effort, then off.
    /// Used when a provider does not name its own ladder.
    /// </summary>
    public static readonly IReadOnlyList<string> CycleOrder = new[] { Low, Medium, High, XHigh, None };

    /// <summary>
    /// The ladder this provider's model actually understands, or the default when it names
    /// none. Which levels mean anything is a property of the model and the server in front
    /// of it, not of any standard: llama.cpp interpolates the level into a Jinja template,
    /// while DeepSeek accepts a fixed set and ignores the rest. One global ladder made the
    /// button a five-position control where two of the positions quietly did nothing.
    /// </summary>
    public static IReadOnlyList<string> LevelsFor(ProviderConfig? config) =>
        config?.ReasoningEffortLevels is { Count: > 0 } levels
            ? levels
            : CycleOrder;

    /// <summary>
    /// Validates a ladder as configured, dropping anything that is not a known level so a
    /// typo in Settings cannot become a request field. Returns null when nothing survives,
    /// which means "use the default".
    /// </summary>
    public static List<string>? ParseLevels(string? levels)
    {
        if (string.IsNullOrWhiteSpace(levels))
        {
            return null;
        }

        var parsed = levels
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(Normalize)
            .Where(level => level is not null)
            .Select(level => level!)
            .Distinct(StringComparer.Ordinal)
            .ToList();

        return parsed.Count > 0 ? parsed : null;
    }

    /// <summary>
    /// Trims and lower-cases a stored or wire value, returning null for anything that is
    /// not a known level. Unknown values are dropped rather than forwarded: a typo in a
    /// synced chat must not silently become a request field.
    /// </summary>
    public static string? Normalize(string? effort)
    {
        if (string.IsNullOrWhiteSpace(effort))
        {
            return null;
        }

        var trimmed = effort.Trim().ToLowerInvariant();
        if (trimmed != None && trimmed != Low && trimmed != Medium && trimmed != High && trimmed != XHigh && trimmed != Max)
        {
            AutomatonDebug.LogWarning($"[ReasoningEffortSettings] Discarding unknown reasoning effort '{effort}'");
            return null;
        }

        return trimmed;
    }

    /// <summary>
    /// The level after this one in the cycle. An unset level starts the cycle at its head.
    /// </summary>
    public static string Next(string? current) => Next(current, CycleOrder);

    /// <summary>
    /// The level after this one in a specific provider's ladder. A level the ladder does
    /// not contain — because the chat carries one from a model it used to be pointed at —
    /// restarts the cycle rather than falling off the end.
    /// </summary>
    public static string Next(string? current, IReadOnlyList<string> ladder)
    {
        if (ladder == null || ladder.Count == 0)
        {
            ladder = CycleOrder;
        }

        var normalized = Normalize(current);
        var index = normalized == null ? -1 : ladder.ToList().IndexOf(normalized);

        return index < 0 ? ladder[0] : ladder[(index + 1) % ladder.Count];
    }

    /// <summary>
    /// A copy of <paramref name="config"/> whose request parameters carry
    /// <paramref name="effort"/>. The original is never mutated - it belongs to the loaded
    /// app config, which every other chat shares. Returns the original instance when there
    /// is no level to apply, so the no-override path allocates nothing.
    /// </summary>
    public static ProviderConfig ApplyTo(ProviderConfig config, string? effort)
    {
        if (config == null)
        {
            throw new ArgumentNullException(nameof(config));
        }

        var normalized = Normalize(effort);
        if (normalized == null)
        {
            return config;
        }

        AutomatonDebug.Log($"[ReasoningEffortSettings] Enter {nameof(ApplyTo)} - effort={normalized}, existingParameters={config.RequestParameters?.Count ?? 0}");

        var parameters = config.RequestParameters == null
            ? new Dictionary<string, JsonElement>()
            : new Dictionary<string, JsonElement>(config.RequestParameters);

        parameters["reasoning_effort"] = JsonSerializer.SerializeToElement(normalized);
        parameters["chat_template_kwargs"] = BuildChatTemplateKwargs(config, normalized);

        // Only the Off level asserts a budget. Any other level leaves the server's own
        // budget alone, because a number chosen here would silently cap a model the user
        // deliberately started with room to think.
        if (normalized == None)
        {
            parameters["reasoning_budget_tokens"] = JsonSerializer.SerializeToElement(0);
        }

        var applied = new ProviderConfig
        {
            Type = config.Type,
            ApiKey = config.ApiKey,
            BaseUrl = config.BaseUrl,
            DefaultModel = config.DefaultModel,
            MaxContextTokens = config.MaxContextTokens,
            MaxOutputTokens = config.MaxOutputTokens,
            ReasoningEffortLevels = config.ReasoningEffortLevels,
            Enabled = config.Enabled,
            SuggestReplies = config.SuggestReplies,
            IsCloud = config.IsCloud,
            RequestParameters = parameters
        };

        AutomatonDebug.Log($"[ReasoningEffortSettings] Exit {nameof(ApplyTo)} - parameters={parameters.Count}");
        return applied;
    }

    /// <summary>
    /// The provider's config with thinking switched off, for a compaction summary; null when
    /// the endpoint is not known to take the switch, and the caller keeps the config it has.
    /// </summary>
    /// <remarks>
    /// A summary is a rewrite of text already in front of the model, and reasoning tokens are
    /// billed against the same reply as the summary itself: a 900,000-token compaction came
    /// back as 1,588 characters of thought and no summary at all. The switch is only sent
    /// where it is known to be understood - the provider names its own ladder and "none" is
    /// on it, or its requests already carry these fields - because an endpoint that rejects
    /// unknown fields would fail the request instead.
    /// </remarks>
    public static ProviderConfig? WithThinkingOff(ProviderConfig? config)
    {
        if (config == null || !LevelsFor(config).Contains(None))
        {
            return null;
        }

        var namesItsLadder = config.ReasoningEffortLevels is { Count: > 0 };
        var alreadySendsLevel = config.RequestParameters != null &&
            (config.RequestParameters.ContainsKey("reasoning_effort") || config.RequestParameters.ContainsKey("chat_template_kwargs"));

        return namesItsLadder || alreadySendsLevel ? ApplyTo(config, None) : null;
    }

    /// <summary>
    /// The template arguments, keeping any keys the provider was already configured with so
    /// a hand-written kwarg is not lost the moment the user clicks the effort control.
    /// </summary>
    private static JsonElement BuildChatTemplateKwargs(ProviderConfig config, string normalized)
    {
        var kwargs = new Dictionary<string, object?>();

        if (config.RequestParameters != null &&
            config.RequestParameters.TryGetValue("chat_template_kwargs", out var existing) &&
            existing.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in existing.EnumerateObject())
            {
                kwargs[property.Name] = property.Value;
            }
        }

        kwargs["enable_thinking"] = normalized != None;
        kwargs["reasoning_effort"] = normalized;

        return JsonSerializer.SerializeToElement(kwargs);
    }
}
