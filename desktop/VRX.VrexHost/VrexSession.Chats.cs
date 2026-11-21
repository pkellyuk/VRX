using System.Collections.Concurrent;
using PersonaBot;
using Automaton.Core.Services.TaskLists;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private readonly object chatReportGate = new();
    private sealed record HeldChatWork(bool Goals, string? GoalId, bool TaskPending, bool Speak, bool SelectedOnly = false);
    private readonly ConcurrentDictionary<string, HeldChatWork> heldChatWork = new(StringComparer.Ordinal);
    private bool turnWorkPending, turnSpeaks;

    private void HoldForegroundWork(string chatId)
    {
        lock (turnGate)
        {
            if (goalWork is { IsCompleted: false } && goalRun is { IsCancellationRequested: false })
                heldChatWork[chatId] = new(true, currentGoalId, false, goalRunSpeaks, goalRunSelectedOnly);
            else if (turnWorkPending && conversation is { IsCompleted: false } && !performingGoalTurn)
                heldChatWork[chatId] = new(false, null, true, turnSpeaks);
        }
    }

    private void ResumeForegroundWork()
    {
        if (!chatOpen || Live || !heldChatWork.TryRemove(ChatId, out var held)) return;
        if (held.Goals) StartGoalRun(held.Speak, held.GoalId, held.SelectedOnly);
        else if (held.TaskPending)
        {
            const string resume = "Resume the interrupted task in this chat. Review the conversation and verify current state before taking action. " +
                "Preserve completed work, avoid repeating submissions, and ask about anything you cannot verify.";
            _ = BeginTurn(resume, held.Speak, null, fromMicrophone: false, resumedTask: true);
        }
        ReportChats(includeTranscript: false);
    }

    private async Task ChangeChatAsync(VrexRequest request)
    {
        string previous = ChatId;
        if (request.Type == "chat-switch" && request.Text == previous)
        {
            ReportChats(includeTranscript: true);
            return;
        }
        if (request.Type == "chat-delete" && request.Text is { } other && other != previous)
        {
            try
            {
                chats.Delete(other);
                heldChatWork.TryRemove(other, out _);
                ReportChats(includeTranscript: true);
            }
            catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or IOException)
            { emit(new("chat-list-error", ex.Message)); }
            return;
        }
        bool keepVoice = Live;
        bool reopenWork = false;
        lock (turnGate) changingChat = true;
        try
        {
            if (request.Type is "chat-new" or "chat-switch") HoldForegroundWork(previous);
            else heldChatWork.TryRemove(previous, out _);
            StopConversation();
            if (conversation != null) await conversation;
            if (goalWork != null) await goalWork;
            if (voiceLoop != null) await voiceLoop;
            if (observation != null) await observation;
            switch (request.Type)
            {
                case "chat-new": chats.Create(); break;
                case "chat-switch": chats.Select(request.Text ?? ""); break;
                case "chat-delete": chats.Delete(request.Text ?? ""); break;
                case "chat-clear":
                    if (request.Text != previous) throw new ArgumentException("That chat is no longer open.");
                    chats.Clear();
                    break;
            }
            if (ChatId != previous || request.Type == "chat-clear")
            {
                ResetChat();
                gridPreview.Clear();
                emit(new("chat-changed"));
            }
            ReportChats(includeTranscript: true);
            ReportTaskList(ChatId, ChatTaskListStore.ReadSafely(ChatId));
            reopenWork = true;
        }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or IOException or UnauthorizedAccessException)
        {
            emit(new("chat-list-error", ex.Message));
        }
        finally { lock (turnGate) changingChat = false; }
        if (reopenWork)
        {
            if (keepVoice) BeginLiveVoice();
            else ResumeForegroundWork();
        }
    }

    private void ReportChats(bool includeTranscript)
    {
        lock (chatReportGate)
        {
            string id = ChatId;
            var entries = chats.Entries.Select(entry => entry with { OnHold = heldChatWork.ContainsKey(entry.Id) }).ToArray();
            var transcript = includeTranscript ? chats.Load().Transcript.ToArray() : [];
            emit(new("chat-list-start", id, X: includeTranscript ? 1 : 0));
            foreach (var entry in entries) emit(new("chat-list-item", Chat: entry));
            foreach (var line in transcript)
            {
                if (line.Role is not ("You" or "Vrex") || string.IsNullOrWhiteSpace(line.Text)) continue;
                const int chunkSize = 6000;
                for (int start = 0; start < line.Text.Length;)
                {
                    int count = Math.Min(chunkSize, line.Text.Length - start);
                    if (start + count < line.Text.Length && char.IsHighSurrogate(line.Text[start + count - 1])) count--;
                    emit(new("chat-list-line", X: start == 0 ? 0 : 1, ChatLine: new(line.Role == "You", line.Text.Substring(start, count))));
                    start += count;
                }
            }
            emit(new("chat-list-ready", X: includeTranscript ? 1 : 0));
        }
    }

    private static string DisplayMessage(string text)
    {
        if (text.StartsWith(VrexPcPrompt.GuidePrefix, StringComparison.Ordinal))
        {
            const string question = " My question: ";
            int start = text.IndexOf(question, StringComparison.Ordinal);
            return start < 0 ? "Guide Me" : text[(start + question.Length)..];
        }
        return text.Replace(VrexPcPrompt.MissingContext, "", StringComparison.Ordinal);
    }
}
