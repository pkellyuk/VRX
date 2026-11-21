using Automaton.Core.Interfaces;
using Automaton.Core.Models;

namespace Automaton.Core.Services.Search;

/// <summary>One embeddable piece of one message, with everything needed to find it again.</summary>
/// <param name="ChunkId">Stable across re-indexing: source, state id, message index, chunk index.</param>
/// <param name="Source">"chat" or "dream", matching <see cref="ConversationSearchService.ChatSource"/>.</param>
/// <param name="StateId">The source row's key - which stored row this came from.</param>
/// <param name="ConversationId">The SessionId every search surface opens conversations by.</param>
/// <param name="MessageIndex">
/// The message's position counting <em>every</em> message, including the ones not embedded.
/// This is what lets the search dialog scroll to the right message; see the remarks on
/// <see cref="Chunk(string, string, ConversationState)"/>.
/// </param>
/// <param name="CharStart">Offset of this chunk within the trimmed message, for future highlighting.</param>
/// <param name="Origin"><see cref="SearchOrigins.Raw"/> for a message as sent, <see cref="SearchOrigins.Compressed"/> for a compaction summary.</param>
public readonly record struct TranscriptChunk(
    string ChunkId,
    string Source,
    string StateId,
    string ConversationId,
    string? ProjectId,
    string Role,
    string? MessageId,
    int MessageIndex,
    int ChunkIndex,
    int CharStart,
    string Text,
    string Origin);

/// <summary>
/// Cuts a conversation into the pieces that get embedded.
/// </summary>
/// <remarks>
/// <para>
/// This is the part the knowledge-store version of this machinery never needed. A fact is one
/// row and one vector; a conversation is many messages, each of which may be many chunks, and
/// the whole set has to be replaceable when the conversation changes.
/// </para>
/// <para>
/// Only user and assistant messages are embedded, plus every compaction summary. Tool output is
/// three quarters of the stored text in this corpus and is exactly what FTS is better at - a
/// file path, an error string, a command - while system prompts are near-identical boilerplate
/// that would match every query weakly and crowd out real hits. Both remain fully searchable by
/// keyword. A summary is carried under the system role but is the opposite of boilerplate: it
/// is a dense account of what the conversation was about, which is what a search by meaning is
/// for. See SEARCH.md.
/// </para>
/// </remarks>
public static class TranscriptChunker
{
    /// <summary>The roles worth embedding.</summary>
    private static readonly string[] Roles = { "user", "assistant" };

    /// <summary>
    /// Every chunk of every eligible message in <paramref name="state"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>The message counter advances for every message, including skipped ones.</b> The index
    /// recorded on a chunk has to be the message's position in the whole transcript, because
    /// that is what the reader that renders a conversation counts. Counting only embedded
    /// messages would make every chunk after the first tool call point at the wrong message, and
    /// nothing would fail - the dialog would just quietly scroll to somewhere else.
    /// </para>
    /// </remarks>
    public static IReadOnlyList<TranscriptChunk> Chunk(string source, string stateId, ConversationState state)
    {
        return Chunk(source, stateId, state, null);
    }

    /// <summary>
    /// Every chunk of every eligible message, with <paramref name="origins"/> giving each
    /// message's origin by position. A message whose origin is
    /// <see cref="SearchOrigins.Compressed"/> is embedded whatever its role; a null or short
    /// list means every message is raw.
    /// </summary>
    public static IReadOnlyList<TranscriptChunk> Chunk(string source, string stateId, ConversationState state, IReadOnlyList<string>? origins)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        if (state == null)
        {
            throw new ArgumentNullException(nameof(state));
        }

        var messages = state.Messages;
        if (messages == null || messages.Count == 0)
        {
            AutomatonDebug.Log($"TranscriptChunker.Chunk: {source}:{stateId} has no messages");
            return Array.Empty<TranscriptChunk>();
        }

        var conversationId = string.IsNullOrWhiteSpace(state.SessionId) ? stateId : state.SessionId;
        var chunks = new List<TranscriptChunk>();
        var skipped = 0;

        for (var messageIndex = 0; messageIndex < messages.Count; messageIndex++)
        {
            var message = messages[messageIndex];
            var origin = origins != null && messageIndex < origins.Count
                ? SearchOrigins.Normalize(origins[messageIndex])
                : SearchOrigins.Raw;

            if (!IsEligible(message, origin))
            {
                skipped++;
                continue;
            }

            var pieces = EmbeddingContract.Chunk(message.Content);
            for (var chunkIndex = 0; chunkIndex < pieces.Count; chunkIndex++)
            {
                chunks.Add(new TranscriptChunk(
                    ChunkIdFor(source, stateId, messageIndex, chunkIndex),
                    source,
                    stateId,
                    conversationId,
                    state.ProjectId,
                    message.Role,
                    string.IsNullOrWhiteSpace(message.Id) ? null : message.Id,
                    messageIndex,
                    chunkIndex,
                    EmbeddingContract.ChunkStart(chunkIndex),
                    pieces[chunkIndex],
                    origin));
            }
        }

        AutomatonDebug.Log($"TranscriptChunker.Chunk: {source}:{stateId} - {chunks.Count} chunk(s) from {messages.Count - skipped} of {messages.Count} message(s), {chunks.Count(c => c.Origin == SearchOrigins.Compressed)} from summaries");
        return chunks;
    }

    /// <summary>Whether this raw message is embedded at all.</summary>
    public static bool IsEligible(Message? message)
    {
        return IsEligible(message, SearchOrigins.Raw);
    }

    /// <summary>
    /// Whether this message is embedded: a user or assistant message of useful length, or a
    /// compaction summary of useful length whatever role carries it.
    /// </summary>
    public static bool IsEligible(Message? message, string? origin)
    {
        if (message == null)
        {
            return false;
        }

        if (ConversationSearchService.IsPlaceholderContent(message.Content))
        {
            return false;
        }

        var compressed = SearchOrigins.Normalize(origin) == SearchOrigins.Compressed;
        if (!compressed && !Roles.Contains(message.Role, StringComparer.OrdinalIgnoreCase))
        {
            return false;
        }

        return message.Content.Trim().Length >= EmbeddingContract.MinChunkCharacters;
    }

    /// <summary>
    /// The id of one chunk. Stable across re-indexing, so the same message re-chunked the same
    /// way keeps its identity - which matters only for debugging, since a conversation's chunks
    /// are replaced wholesale rather than merged.
    /// </summary>
    public static string ChunkIdFor(string source, string stateId, int messageIndex, int chunkIndex)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        return $"{source}|{stateId}|{messageIndex}|{chunkIndex}";
    }
}
