using Automaton.Core.Services;
using Automaton.Core.Services.Search;
using Automaton.Core.Services.TaskLists;
using PersonaBot;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed class VrexChatStore
{
    internal const string Project = "vrx-vrex";
    private readonly ChatStore chats;
    private readonly ConversationSearchService search;

    internal VrexChatStore(string root)
    {
        PersonaBotDatabase.EnsureReady(Path.Combine(Settings.Root, "agent"));
        SearchDatabase.ConfigureWritable();
        string legacyPath = Path.Combine(root, "conversation.json");
        SessionState? legacy = SessionStore.Load(legacyPath);
        if (legacy is { Transcript.Count: 0 })
            legacy.Transcript = LegacyLines(legacy).Select(line =>
                new TranscriptEntry(line.FromUser ? "You" : "Vrex", line.Text, null, DateTimeOffset.UtcNow)).ToList();
        chats = new ChatStore(Project, legacy, "Vrex");
        search = ConversationSearchService.Instance;
        foreach (var chat in chats.Chats) Index(chat.Id);
    }

    internal string Active => chats.ActiveId;
    internal IEnumerable<VrexChatSummary> Entries => chats.Chats.Select(chat => new VrexChatSummary(chat.Id, chat.Title));
    internal SessionState Load() => chats.Load(Active);

    internal void Save(string id, SessionState state)
    {
        chats.Save(id, state);
        Index(id);
    }

    internal void Create() => chats.Create();
    internal void Select(string id) => chats.Activate(id);

    internal void Clear()
    {
        string id = Active;
        chats.Clear(id);
        Forget(id);
    }

    internal void Delete(string id)
    {
        chats.Delete(id);
        Forget(id);
    }

    internal string Recall(string query, string currentId)
    {
        var titles = chats.Chats.Where(chat => chat.Id != currentId)
            .ToDictionary(chat => chat.Id, chat => chat.Title);
        var matches = search.SearchMessages(query, 60, Project, origins: null, dates: default, excludeConversationId: currentId)
            .Where(hit => titles.ContainsKey(hit.ConversationId) && hit.Role is "user" or "assistant")
            .DistinctBy(hit => (hit.ConversationId, hit.MarkedSnippet)).Take(8).Select(hit =>
                $"[{titles[hit.ConversationId]}] {(hit.Role == "user" ? "You" : "Vrex")}: " +
                (hit.MarkedSnippet.Length > 700 ? hit.MarkedSnippet[..700] + "…" : hit.MarkedSnippet));
        var text = string.Join("\n", matches);
        return text.Length == 0 ? "No matching messages in other Vrex chats." : text;
    }

    private void Index(string id) => search.IndexConversationById(chats.StateId(id), Project);

    private void Forget(string id)
    {
        search.RemoveFromIndex(id);
        ChatTaskListStore.Shared.Clear(id);
    }

    private static IEnumerable<VrexChatLine> LegacyLines(SessionState state) =>
        state.History.Where(message => !string.IsNullOrWhiteSpace(message.Content) &&
            (message.Role == "assistant" && !message.Parts.Any(part => part.Type == "tool") ||
            message.Role == "user" && !IsRestoreNote(message.Content)))
            .Select(message => new VrexChatLine(message.Role == "user", message.Content));

    private static bool IsRestoreNote(string text) => text is
        "The Ubuntu desktop is available. Inspect its current screen before resuming any guest actions." or
        "The Ubuntu desktop is currently off. Saved chat remains available." or
        "The operator interacted with the guest screen manually. Inspect the current screen before continuing.";
}
