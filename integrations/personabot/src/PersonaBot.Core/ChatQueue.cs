using System.Diagnostics;
using Automaton.Core.Models;

namespace PersonaBot;

/// <summary>A message the operator sent while PersonaBot was busy, waiting its turn, with any files attached to it.</summary>
public sealed record QueuedMessage(string Id, string Text, byte[]? Image, DateTimeOffset Time, List<ChatAttachment>? Files = null);

/// <summary>
/// The chat's message queue, after Automaton's: messages sent while the AI is running wait here in
/// order; each can be cancelled, or made immediate (moved to the front, and the host stops the
/// current run so it goes next). Thread-safe; <see cref="Changed"/> fires after every change, on the
/// thread that made it.
/// </summary>
public sealed class ChatQueue
{
    private readonly object _gate = new();
    private readonly List<QueuedMessage> _items = [];

    public event Action? Changed;

    public int Count
    {
        get
        {
            lock (_gate)
            {
                return _items.Count;
            }
        }
    }

    /// <summary>The waiting messages, front first.</summary>
    public IReadOnlyList<QueuedMessage> Snapshot()
    {
        lock (_gate)
        {
            return _items.ToList();
        }
    }

    /// <summary>Adds a message at the back. Text must not be empty.</summary>
    public QueuedMessage Enqueue(string text, byte[]? image = null, IReadOnlyList<ChatAttachment>? files = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(text);
        var item = new QueuedMessage(Guid.NewGuid().ToString("N"), text, image, DateTimeOffset.Now, files is { Count: > 0 } ? files.ToList() : null);
        int count;
        lock (_gate)
        {
            _items.Add(item);
            count = _items.Count;
        }
        Trace.WriteLine($"ChatQueue.Enqueue: {item.Id} added, {count} waiting, {text.Length} chars, image={(image != null)}, files={item.Files?.Count ?? 0}");
        Changed?.Invoke();
        return item;
    }

    /// <summary>Removes a waiting message. False if it is no longer queued (already sent or cancelled).</summary>
    public bool Cancel(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        bool removed;
        lock (_gate)
        {
            removed = _items.RemoveAll(item => item.Id == id) > 0;
        }
        Trace.WriteLine($"ChatQueue.Cancel: {id} removed={removed}");
        if (removed)
        {
            Changed?.Invoke();
        }
        return removed;
    }

    /// <summary>Moves a waiting message to the front. False if it is no longer queued.</summary>
    public bool MoveToFront(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        lock (_gate)
        {
            var index = _items.FindIndex(item => item.Id == id);
            if (index < 0)
            {
                Trace.WriteLine($"ChatQueue.MoveToFront: {id} not queued");
                return false;
            }
            var item = _items[index];
            _items.RemoveAt(index);
            _items.Insert(0, item);
            Trace.WriteLine($"ChatQueue.MoveToFront: {id} moved from {index + 1} to 1");
        }
        Changed?.Invoke();
        return true;
    }

    /// <summary>Takes the front message, if any.</summary>
    public bool TryDequeue(out QueuedMessage? item)
    {
        lock (_gate)
        {
            if (_items.Count == 0)
            {
                item = null;
                return false;
            }
            item = _items[0];
            _items.RemoveAt(0);
        }
        Trace.WriteLine($"ChatQueue.TryDequeue: {item.Id} taken");
        Changed?.Invoke();
        return true;
    }

    /// <summary>Replaces the contents, e.g. with a saved chat's queue. Null or blank entries are skipped.</summary>
    public void Replace(IEnumerable<QueuedMessage>? items)
    {
        var valid = (items ?? []).Where(item => item != null && !string.IsNullOrWhiteSpace(item.Id) && !string.IsNullOrWhiteSpace(item.Text)).ToList();
        lock (_gate)
        {
            _items.Clear();
            _items.AddRange(valid);
        }
        Trace.WriteLine($"ChatQueue.Replace: {valid.Count} waiting");
        Changed?.Invoke();
    }

    /// <summary>A one-line preview of a message for the queue strip.</summary>
    public static string Preview(string text, int maxLength = 70)
    {
        ArgumentNullException.ThrowIfNull(text);
        if (maxLength < 2)
        {
            throw new ArgumentOutOfRangeException(nameof(maxLength), "The preview needs room for at least two characters.");
        }
        var line = string.Join(' ', text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        return line.Length <= maxLength ? line : line[..(maxLength - 1)].TrimEnd() + "…";
    }
}
