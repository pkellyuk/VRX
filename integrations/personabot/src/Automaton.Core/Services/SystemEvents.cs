using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Static event aggregator for cross-service communication.
/// Provides a decoupled way for services to subscribe to system-wide events.
/// </summary>
public static class SystemEvents
{
    private static readonly object _lock = new();
    private static event Action<ConversationState>? _onConversationSaved;
    private static event Action<ConversationSavedEventInfo>? _onConversationSavedDetailed;
    private static event Action<ConversationState>? _onDreamConversationSaved;
    private static event Action<ConversationStreamingEventInfo>? _onConversationStreamingUpdated;
    private static event Action<ConversationContextUsageEventInfo>? _onConversationContextUsage;
    private static event Action<ConversationStreamEventInfo>? _onConversationStreamEvent;
    private static event Action<ConversationGenerationStatusEventInfo>? _onConversationGenerationStatusChanged;
    private static event Action? _onProjectTreeChanged;
    private static event Action? _onApplicationResumed;
    private static event Action<RemoteThreadRenamedEventInfo>? _onRemoteThreadRenamed;
    private static event Action<RemoteThreadRemovedEventInfo>? _onRemoteThreadRemoved;
    private static event Action<ThreadArchiveChangedEventInfo>? _onThreadArchiveChanged;
    private static event Action<InboxItem>? _onInboxItemAdded;

    /// <summary>An inbox item was read or archived, including changes made on this desktop.</summary>
    public static event Action? OnInboxStatusChanged;

    public static void RaiseInboxStatusChanged() => OnInboxStatusChanged?.Invoke();

    /// <summary>
    /// Event raised when a conversation state is saved.
    /// Subscribe to this to perform actions after conversation saves (e.g., indexing for search).
    /// </summary>
    public static event Action<ConversationState>? OnConversationSaved
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationSaved event handler added");
            lock (_lock)
            {
                _onConversationSaved += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationSaved event handler removed");
            lock (_lock)
            {
                _onConversationSaved -= value;
            }
        }
    }

    /// <summary>
    /// Event raised when a dream mirror conversation is written.
    /// Subscribe to this to index dream mirrors into the dream FTS table.
    /// </summary>
    public static event Action<ConversationState>? OnDreamConversationSaved
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnDreamConversationSaved event handler added");
            lock (_lock)
            {
                _onDreamConversationSaved += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnDreamConversationSaved event handler removed");
            lock (_lock)
            {
                _onDreamConversationSaved -= value;
            }
        }
    }

    public static event Action<ConversationSavedEventInfo>? OnConversationSavedDetailed
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationSavedDetailed event handler added");
            lock (_lock)
            {
                _onConversationSavedDetailed += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationSavedDetailed event handler removed");
            lock (_lock)
            {
                _onConversationSavedDetailed -= value;
            }
        }
    }

    /// <summary>
    /// A conversation's context figure, raised by the process running its turn before every
    /// model request - so the context ring moves during a long turn, not only when it ends.
    /// </summary>
    public static event Action<ConversationContextUsageEventInfo>? OnConversationContextUsage
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationContextUsage event handler added");
            lock (_lock)
            {
                _onConversationContextUsage += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationContextUsage event handler removed");
            lock (_lock)
            {
                _onConversationContextUsage -= value;
            }
        }
    }

    /// <summary>Public, unlike its neighbours: the turn runs in Automaton.AI.</summary>
    public static void RaiseConversationContextUsage(ConversationContextUsageEventInfo info)
    {
        if (info == null || string.IsNullOrWhiteSpace(info.StateId) || string.IsNullOrWhiteSpace(info.ContextUsage))
        {
            return;
        }

        Action<ConversationContextUsageEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onConversationContextUsage;
        }

        handlers?.Invoke(info);
    }

    public static event Action<ConversationStreamingEventInfo>? OnConversationStreamingUpdated
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationStreamingUpdated event handler added");
            lock (_lock)
            {
                _onConversationStreamingUpdated += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationStreamingUpdated event handler removed");
            lock (_lock)
            {
                _onConversationStreamingUpdated -= value;
            }
        }
    }

    public static event Action<ConversationStreamEventInfo>? OnConversationStreamEvent
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationStreamEvent event handler added");
            lock (_lock)
            {
                _onConversationStreamEvent += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationStreamEvent event handler removed");
            lock (_lock)
            {
                _onConversationStreamEvent -= value;
            }
        }
    }

    public static event Action<ConversationGenerationStatusEventInfo>? OnConversationGenerationStatusChanged
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnConversationGenerationStatusChanged event handler added");
            lock (_lock)
            {
                _onConversationGenerationStatusChanged += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnConversationGenerationStatusChanged event handler removed");
            lock (_lock)
            {
                _onConversationGenerationStatusChanged -= value;
            }
        }
    }


    public static event Action? OnProjectTreeChanged
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnProjectTreeChanged event handler added");
            lock (_lock)
            {
                _onProjectTreeChanged += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnProjectTreeChanged event handler removed");
            lock (_lock)
            {
                _onProjectTreeChanged -= value;
            }
        }
    }

    public static event Action? OnApplicationResumed
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnApplicationResumed event handler added");
            lock (_lock)
            {
                _onApplicationResumed += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnApplicationResumed event handler removed");
            lock (_lock)
            {
                _onApplicationResumed -= value;
            }
        }
    }

    /// <summary>
    /// Raises the OnConversationSaved event.
    /// Called by ConversationPersistenceService after a successful save.
    /// </summary>
    internal static void RaiseConversationSaved(ConversationState state)
    {
        if (state == null)
        {
            throw new ArgumentNullException(nameof(state));
        }

        AutomatonDebug.Log($"SystemEvents.RaiseConversationSaved: Entry - state.SessionId={state.SessionId}");
        Action<ConversationState>? handlers;
        Action<ConversationSavedEventInfo>? detailedHandlers;
        lock (_lock)
        {
            handlers = _onConversationSaved;
            detailedHandlers = _onConversationSavedDetailed;
        }
        handlers?.Invoke(state);
        detailedHandlers?.Invoke(new ConversationSavedEventInfo(string.Empty, state));
        AutomatonDebug.Log("SystemEvents.RaiseConversationSaved: Exiting");
    }

    internal static void RaiseConversationSaved(string stateId, ConversationState state)
    {
        if (state == null)
        {
            throw new ArgumentNullException(nameof(state));
        }

        AutomatonDebug.Log($"SystemEvents.RaiseConversationSaved(stateId): Entry - stateId={stateId}, state.SessionId={state.SessionId}");
        Action<ConversationState>? handlers;
        Action<ConversationSavedEventInfo>? detailedHandlers;
        lock (_lock)
        {
            handlers = _onConversationSaved;
            detailedHandlers = _onConversationSavedDetailed;
        }

        handlers?.Invoke(state);
        detailedHandlers?.Invoke(new ConversationSavedEventInfo(stateId, state));
        AutomatonDebug.Log("SystemEvents.RaiseConversationSaved(stateId): Exiting");
    }

    /// <summary>
    /// Raises the OnDreamConversationSaved event.
    /// Called by DreamPersistenceService after a successful dream mirror write.
    /// </summary>
    internal static void RaiseDreamConversationSaved(ConversationState state)
    {
        if (state == null)
        {
            throw new ArgumentNullException(nameof(state));
        }

        AutomatonDebug.Log($"SystemEvents.RaiseDreamConversationSaved: Entry - state.SessionId={state.SessionId}");
        Action<ConversationState>? handlers;
        lock (_lock)
        {
            handlers = _onDreamConversationSaved;
        }
        handlers?.Invoke(state);
        AutomatonDebug.Log("SystemEvents.RaiseDreamConversationSaved: Exiting");
    }

    internal static void RaiseConversationStreamingUpdated(ConversationStreamingEventInfo info)
    {
        AutomatonDebug.Log($"SystemEvents.RaiseConversationStreamingUpdated: stateId={info.StateId}, messageId={info.MessageId}, deltaLength={info.Delta?.Length ?? 0}, contentLength={info.Content?.Length ?? 0}, isFinal={info.IsFinal}");
        Action<ConversationStreamingEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onConversationStreamingUpdated;
        }

        handlers?.Invoke(info);
    }

    internal static void RaiseConversationStreamEvent(ConversationStreamEventInfo info)
    {
        AutomatonDebug.Log($"SystemEvents.RaiseConversationStreamEvent: stateId={info.StateId}, kind={info.StreamEvent.Kind}");
        Action<ConversationStreamEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onConversationStreamEvent;
        }

        handlers?.Invoke(info);
    }

    internal static void RaiseConversationGenerationStatusChanged(ConversationGenerationStatusEventInfo info)
    {
        AutomatonDebug.Log($"SystemEvents.RaiseConversationGenerationStatusChanged: stateId={info.StateId}, isProcessing={info.IsProcessing}, status={info.StatusText}");
        Action<ConversationGenerationStatusEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onConversationGenerationStatusChanged;
        }

        handlers?.Invoke(info);
    }

    public static void RaiseProjectTreeChanged()
    {
        AutomatonDebug.Log("SystemEvents.RaiseProjectTreeChanged: Entry");
        Action? handlers;
        lock (_lock)
        {
            handlers = _onProjectTreeChanged;
        }

        handlers?.Invoke();
        AutomatonDebug.Log("SystemEvents.RaiseProjectTreeChanged: Exiting");
    }

    public static void RaiseApplicationResumed()
    {
        AutomatonDebug.Log("SystemEvents.RaiseApplicationResumed: Entry");
        Action? handlers;
        lock (_lock)
        {
            handlers = _onApplicationResumed;
        }

        handlers?.Invoke();
        AutomatonDebug.Log("SystemEvents.RaiseApplicationResumed: Exiting");
    }

    /// <summary>
    /// Event raised when a thread we own has been renamed by an incoming
    /// ThreadRename envelope. Subscribers (typically the UI) should update
    /// their in-memory model so the change is visible in the tree and is
    /// not overwritten by a subsequent stale-model save back to the DB.
    /// </summary>
    public static event Action<RemoteThreadRenamedEventInfo>? OnRemoteThreadRenamed
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnRemoteThreadRenamed event handler added");
            lock (_lock)
            {
                _onRemoteThreadRenamed += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnRemoteThreadRenamed event handler removed");
            lock (_lock)
            {
                _onRemoteThreadRenamed -= value;
            }
        }
    }

    /// <summary>
    /// Event raised when a thread we own has been removed by an incoming
    /// ThreadRemove envelope. Same in-memory sync rationale as
    /// <see cref="OnRemoteThreadRenamed"/>.
    /// </summary>
    public static event Action<RemoteThreadRemovedEventInfo>? OnRemoteThreadRemoved
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnRemoteThreadRemoved event handler added");
            lock (_lock)
            {
                _onRemoteThreadRemoved += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnRemoteThreadRemoved event handler removed");
            lock (_lock)
            {
                _onRemoteThreadRemoved -= value;
            }
        }
    }

    public static void RaiseRemoteThreadRenamed(RemoteThreadRenamedEventInfo info)
    {
        if (info == null)
        {
            return;
        }

        AutomatonDebug.Log($"SystemEvents.RaiseRemoteThreadRenamed: projectId={info.ProjectId}, threadId={info.ThreadId}, newName={info.NewName}");
        Action<RemoteThreadRenamedEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onRemoteThreadRenamed;
        }
        handlers?.Invoke(info);
    }

    public static void RaiseRemoteThreadRemoved(RemoteThreadRemovedEventInfo info)
    {
        if (info == null)
        {
            return;
        }

        AutomatonDebug.Log($"SystemEvents.RaiseRemoteThreadRemoved: projectId={info.ProjectId}, threadId={info.ThreadId}");
        Action<RemoteThreadRemovedEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onRemoteThreadRemoved;
        }
        handlers?.Invoke(info);
    }

    /// <summary>
    /// Event raised when a thread we own has been archived or restored, whether by
    /// the manage_chats tool here or by a peer through it. The DB is already
    /// updated; subscribers (typically the UI) take an archived chat out of their
    /// in-memory tree and put a restored one back, for the reason given on
    /// <see cref="OnRemoteThreadRenamed"/>.
    /// </summary>
    public static event Action<ThreadArchiveChangedEventInfo>? OnThreadArchiveChanged
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnThreadArchiveChanged event handler added");
            lock (_lock)
            {
                _onThreadArchiveChanged += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnThreadArchiveChanged event handler removed");
            lock (_lock)
            {
                _onThreadArchiveChanged -= value;
            }
        }
    }

    public static void RaiseThreadArchiveChanged(ThreadArchiveChangedEventInfo info)
    {
        if (info == null)
        {
            return;
        }

        AutomatonDebug.Log($"SystemEvents.RaiseThreadArchiveChanged: projectId={info.ProjectId}, threadId={info.ThreadId}, isArchived={info.IsArchived}");
        Action<ThreadArchiveChangedEventInfo>? handlers;
        lock (_lock)
        {
            handlers = _onThreadArchiveChanged;
        }
        handlers?.Invoke(info);
    }

    /// <summary>
    /// Event raised when an inbox alert is added. Subscribers (typically the UI)
    /// should refresh the inbox list and unread badge.
    /// </summary>
    public static event Action<InboxItem>? OnInboxItemAdded
    {
        add
        {
            AutomatonDebug.Log("SystemEvents: OnInboxItemAdded event handler added");
            lock (_lock)
            {
                _onInboxItemAdded += value;
            }
        }
        remove
        {
            AutomatonDebug.Log("SystemEvents: OnInboxItemAdded event handler removed");
            lock (_lock)
            {
                _onInboxItemAdded -= value;
            }
        }
    }

    /// <summary>
    /// Raises the OnInboxItemAdded event. Called by InboxService after a successful add.
    /// </summary>
    public static void RaiseInboxItemAdded(InboxItem item)
    {
        if (item == null)
        {
            throw new ArgumentNullException(nameof(item));
        }

        AutomatonDebug.Log($"SystemEvents.RaiseInboxItemAdded: Entry - id={item.Id}, sourceSessionId={item.SourceSessionId}");
        Action<InboxItem>? handlers;
        lock (_lock)
        {
            handlers = _onInboxItemAdded;
        }
        handlers?.Invoke(item);
        AutomatonDebug.Log("SystemEvents.RaiseInboxItemAdded: Exiting");
    }
}

public sealed record ConversationSavedEventInfo(string StateId, ConversationState State);

/// <summary>A conversation's context figure mid-turn, in the "Context: N tokens / Max: M  P%" form.</summary>
public sealed record ConversationContextUsageEventInfo(string StateId, string ContextUsage);

public sealed record ConversationStreamingEventInfo(
    string StateId,
    string MessageId,
    string Role,
    string Content,
    string Delta,
    bool IsFinal,
    string? UserMessage = null);

public sealed record ConversationStreamEventInfo(
    string StateId,
    RemoteConversationStreamEvent StreamEvent);

public sealed record ConversationGenerationStatusEventInfo(
    string StateId,
    bool IsProcessing,
    string StatusText);

public sealed record RemoteThreadRenamedEventInfo(string ProjectId, string ThreadId, string NewName);

public sealed record RemoteThreadRemovedEventInfo(string ProjectId, string ThreadId);

public sealed record ThreadArchiveChangedEventInfo(string ProjectId, string ThreadId, bool IsArchived);
