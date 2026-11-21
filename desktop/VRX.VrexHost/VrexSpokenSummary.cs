namespace VRX.VrexHost;

internal static class VrexSpokenSummary
{
    private const string Marker = "[VREX_SPOKEN_RECAP]";
    private const string ChoiceMarker = "[VREX_CHOICES]";
    internal const string ChoiceInstruction =
        "If your final reply asks the user to choose between concrete options, append a line starting with " + ChoiceMarker +
        " followed by two or three short button labels separated by vertical bars. Use the user's language. " +
        "Only add choices if an answer is needed, and do not add them midway through a task.";
    internal const string Instruction =
        "Write the full reply as usual. Only after the final reply, append a new line starting with " + Marker +
        " followed by a natural spoken recap in two or three short sentences and up to 320 characters. " +
        "Summarize the result or next step in the user's language. Use plain words. Do not include code, paths, " +
        "URLs, special symbols, or Markdown. Do not append a recap before or during a tool action.";

    internal static (string Written, string? Spoken) Split(string reply)
    {
        int marker = reply.LastIndexOf("\n" + Marker, StringComparison.Ordinal);
        if (marker < 0) return (reply, null);
        string written = reply[..marker].TrimEnd();
        string spoken = reply[(marker + 1 + Marker.Length)..].Trim();
        if (spoken.Length is 0 or > 340 || spoken.Contains("://", StringComparison.Ordinal) ||
            spoken.IndexOfAny(['\n', '\r', '\\', '`', '|', '<', '>', '[', ']', '*', '#']) >= 0)
            return (written, null);
        return (written, spoken);
    }

    internal static (string Written, string[] Choices) SplitChoices(string reply)
    {
        int marker = reply.LastIndexOf("\n" + ChoiceMarker, StringComparison.Ordinal);
        if (marker < 0) return (reply, []);
        int labels = marker + 1 + ChoiceMarker.Length;
        int nextLine = reply.IndexOf('\n', labels);
        string[] choices = reply[labels..(nextLine < 0 ? reply.Length : nextLine)].Split('|', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)
            .Where(label => label.Length <= 48).Distinct(StringComparer.OrdinalIgnoreCase).Take(3).ToArray();
        return (reply[..marker] + (nextLine < 0 ? "" : reply[nextLine..]), choices.Length >= 2 ? choices : []);
    }

    internal static string Excerpt(string written)
    {
        var regex = System.Text.RegularExpressions.RegexOptions.Multiline;
        string text = System.Text.RegularExpressions.Regex.Replace(written, @"```[\s\S]*?(?:```|$)|`[^`\r\n]*`", " ");
        text = System.Text.RegularExpressions.Regex.Replace(text, @"\[([^\]\r\n]+)\]\([^\s)]+\)", "$1");
        text = System.Text.RegularExpressions.Regex.Replace(text, @"\b(?:https?://|www\.)\S+|(?<!\w)(?:[a-zA-Z]:\\|/)[^\s,;]+", " ");
        text = System.Text.RegularExpressions.Regex.Replace(text, @"^\s*(?:[-*#>]+|\d+\.)\s*", "", regex);
        text = System.Text.RegularExpressions.Regex.Replace(text, @"[^\p{L}\p{N}\p{M}\s.,!?。！？'’:-]", " ");
        text = System.Text.RegularExpressions.Regex.Replace(text, @"\s+", " ").Trim();
        if (text.Length == 0) return "";
        int end = Math.Min(320, text.Length);
        int lastComplete = 0, sentences = 0;
        for (int i = 0; i < end; i++)
        {
            if (".!?。！？".Contains(text[i]) && (i + 1 == text.Length || char.IsWhiteSpace(text[i + 1])))
            {
                lastComplete = i + 1;
                if (++sentences == 3) return text[..lastComplete];
            }
        }
        if (end == text.Length) return text;
        if (lastComplete >= 65) return text[..lastComplete];
        int word = text.LastIndexOf(' ', end);
        return text[..(word > 0 ? word : end)].TrimEnd(',', ':', '-') + ".";
    }
}
