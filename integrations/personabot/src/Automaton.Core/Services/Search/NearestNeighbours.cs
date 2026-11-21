using System.Runtime.InteropServices;
using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Search;

/// <summary>One chunk, and how far it is from the query.</summary>
/// <param name="Distance">Cosine distance, 0 (identical) to 2 (opposite). Lower is nearer.</param>
public readonly record struct ChunkNeighbour(
    string ChunkId,
    string Source,
    string StateId,
    string ConversationId,
    string? ProjectId,
    string Role,
    string? MessageId,
    int MessageIndex,
    string Text,
    double Distance,
    string Origin);

/// <summary>Finds the chunks nearest a query vector.</summary>
/// <remarks>
/// An interface so an extension-backed implementation can replace the managed scan later without
/// touching a caller. At this corpus size there is no reason to.
/// </remarks>
public interface INearestNeighbours
{
    /// <summary>
    /// The <paramref name="k"/> nearest chunks, nearest first, restricted to the given scope.
    /// Returns empty rather than throwing when there is nothing to search.
    /// </summary>
    IReadOnlyList<ChunkNeighbour> Nearest(float[] query, int generation, string? projectId, SearchSource source, int k);
}

/// <summary>
/// Reads every chunk in a generation and computes cosine distance in managed code.
/// </summary>
/// <remarks>
/// <para>
/// A full scan, deliberately. This corpus is thousands of chunks - the whole transcript history
/// of one person - so the arithmetic is a few million multiply-adds, single-digit milliseconds,
/// and the blob read is served from the page cache after the first query. An approximate index
/// would trade exactness for a saving that does not exist at this size.
/// </para>
/// <para>
/// Vectors are L2-normalised when they are stored, so cosine distance is <c>1 - dot</c>. The scope
/// filters are ordinary indexed predicates on an ordinary table, so they narrow the scan rather
/// than filtering after it.
/// </para>
/// </remarks>
public sealed class ManagedVectorScan : INearestNeighbours
{
    private readonly SearchDatabase _db;
    private int _malformed;

    public ManagedVectorScan(SearchDatabase database)
    {
        _db = database ?? throw new ArgumentNullException(nameof(database));
    }

    public IReadOnlyList<ChunkNeighbour> Nearest(float[] query, int generation, string? projectId, SearchSource source, int k)
    {
        if (query == null)
        {
            throw new ArgumentNullException(nameof(query));
        }

        if (k <= 0)
        {
            return Array.Empty<ChunkNeighbour>();
        }

        if (query.Length == 0)
        {
            AutomatonDebug.LogWarning("ManagedVectorScan.Nearest: Exit - the query vector is empty");
            return Array.Empty<ChunkNeighbour>();
        }

        if (!_db.IsAvailable)
        {
            AutomatonDebug.Log("ManagedVectorScan.Nearest: Exit - no search database in this process");
            return Array.Empty<ChunkNeighbour>();
        }

        AutomatonDebug.Log($"ManagedVectorScan.Nearest: Entry - generation={generation}, k={k}, project={projectId ?? "(any)"}, source={source}");
        var table = VectorIndex.ChunkTable(generation);
        var parameters = new List<SqliteParameter>();
        var sql = $@"
            SELECT ChunkId, Source, StateId, ConversationId, ProjectId, Role, MessageId, MessageIndex, Text, Embedding, Origin
            FROM {table}
            WHERE 1 = 1";

        if (!string.IsNullOrWhiteSpace(projectId))
        {
            sql += " AND ProjectId = @ProjectId";
            parameters.Add(new SqliteParameter("@ProjectId", projectId));
        }

        switch (source)
        {
            case SearchSource.Chats:
                sql += " AND Source = @Source";
                parameters.Add(new SqliteParameter("@Source", ConversationSearchService.ChatSource));

                // The same rule the FTS path applies, read from the source through the attached
                // alias so a chat newly excluded from dreaming drops out without a re-index.
                sql += $@" AND NOT EXISTS (
                    SELECT 1 FROM {SearchDatabase.SourceAlias}.ConversationState cs
                    WHERE cs.SessionId = {table}.ConversationId AND cs.ExcludeFromDream = 1)";
                break;

            case SearchSource.Dreams:
                sql += " AND Source = @Source";
                parameters.Add(new SqliteParameter("@Source", ConversationSearchService.DreamSource));
                break;
        }

        List<Dictionary<string, object?>> rows;
        try
        {
            rows = _db.Query(sql, parameters.ToArray());
        }
        catch (Exception ex)
        {
            // A generation that was dropped between the read of ActiveGeneration and this query
            // is the ordinary case here, and it is not worth an error.
            AutomatonDebug.LogWarning($"ManagedVectorScan.Nearest: Exit - could not read {table}: {ex.Message}");
            return Array.Empty<ChunkNeighbour>();
        }

        var best = new List<ChunkNeighbour>(rows.Count);
        foreach (var row in rows)
        {
            var neighbour = Score(row, query);
            if (neighbour != null)
            {
                best.Add(neighbour.Value);
            }
        }

        var nearest = best.OrderBy(n => n.Distance).ThenBy(n => n.ChunkId, StringComparer.Ordinal).Take(k).ToList();
        AutomatonDebug.Log($"ManagedVectorScan.Nearest: Exit - scanned {rows.Count}, returning {nearest.Count}");
        return nearest;
    }

    private ChunkNeighbour? Score(Dictionary<string, object?> row, float[] query)
    {
        if (row["Embedding"] is not byte[] blob)
        {
            return null;
        }

        if (blob.Length == 0 || blob.Length % sizeof(float) != 0 || blob.Length / sizeof(float) != query.Length)
        {
            // One unreadable chunk must not fail a query. Logged once so a systematic problem is
            // still visible without a line per row per search.
            if (Interlocked.Exchange(ref _malformed, 1) == 0)
            {
                AutomatonDebug.LogWarning($"ManagedVectorScan.Score: skipping a chunk whose vector is {blob.Length} bytes; the query is {query.Length} dims");
            }

            return null;
        }

        var stored = MemoryMarshal.Cast<byte, float>(blob);
        double dot = 0;
        for (var i = 0; i < query.Length; i++)
        {
            dot += (double)query[i] * stored[i];
        }

        return new ChunkNeighbour(
            row["ChunkId"] as string ?? string.Empty,
            row["Source"] as string ?? string.Empty,
            row["StateId"] as string ?? string.Empty,
            row["ConversationId"] as string ?? string.Empty,
            row["ProjectId"] as string,
            row["Role"] as string ?? string.Empty,
            row["MessageId"] as string,
            row["MessageIndex"] == null ? -1 : Convert.ToInt32(row["MessageIndex"]),
            row["Text"] as string ?? string.Empty,
            1.0 - dot,
            SearchOrigins.Normalize(row.TryGetValue("Origin", out var origin) ? origin as string : null));
    }
}
