using Microsoft.Data.Sqlite;

namespace Automaton.Core.Services.Search;

/// <summary>
/// The chunk tables in <see cref="SearchDatabase"/>, and the generations they belong to.
/// </summary>
/// <remarks>
/// <para>
/// Vectors are stored as float32 little-endian blobs in an ordinary table, and distance is
/// computed in managed code by <see cref="ManagedVectorScan"/>. There is no sqlite-vec and no
/// loadable extension. The deciding reason is where the query runs: <c>remember</c> executes
/// inside conversation hosts, which open search.db read-only and never load native extensions,
/// so an extension-backed query path would leave semantic search unavailable in the one process
/// where it matters most. At this corpus size - thousands of chunks, not millions - a full scan
/// is single-digit milliseconds, so the extension buys nothing else either. See SEARCH.md.
/// </para>
/// <para>
/// A generation is one embedding contract's worth of vectors. Writes go to the generation being
/// built whether or not it is active; queries only ever read the active one. That is what makes
/// a model change safe: the new vectors accumulate beside the old, and the swap that replaces
/// them is one transaction.
/// </para>
/// </remarks>
public sealed class VectorIndex
{
    /// <summary>Generation still being filled. Not visible to queries.</summary>
    public const string Building = "building";

    /// <summary>The generation queries read.</summary>
    public const string Active = "active";

    private readonly SearchDatabase _db;

    public VectorIndex(SearchDatabase database)
    {
        _db = database ?? throw new ArgumentNullException(nameof(database));
    }

    /// <summary>The table holding one generation's chunks.</summary>
    public static string ChunkTable(int generation) => $"ConversationChunks_g{generation}";

    /// <summary>The generation queries should read, or null when none has been completed yet.</summary>
    public int? ActiveGeneration()
    {
        try
        {
            var rows = _db.Query(
                "SELECT Generation FROM IndexGenerations WHERE State = @State ORDER BY Generation DESC LIMIT 1",
                new SqliteParameter("@State", Active));

            if (rows.Count == 0)
            {
                return null;
            }

            return Convert.ToInt32(rows[0]["Generation"]);
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"VectorIndex.ActiveGeneration: {ex.Message}");
            return null;
        }
    }

    /// <summary>The generation holding vectors for this fingerprint, active or still building.</summary>
    public int? GenerationFor(string fingerprint)
    {
        if (string.IsNullOrWhiteSpace(fingerprint))
        {
            throw new ArgumentException("fingerprint is required", nameof(fingerprint));
        }

        var rows = _db.Query(
            "SELECT Generation FROM IndexGenerations WHERE Fingerprint = @Fingerprint AND State IN (@Active, @Building) ORDER BY Generation DESC LIMIT 1",
            new SqliteParameter("@Fingerprint", fingerprint),
            new SqliteParameter("@Active", Active),
            new SqliteParameter("@Building", Building));

        if (rows.Count == 0)
        {
            return null;
        }

        return Convert.ToInt32(rows[0]["Generation"]);
    }

    /// <summary>Creates the next generation and its table. Returns its number.</summary>
    public int CreateGeneration(EmbeddingContract contract)
    {
        if (contract == null)
        {
            throw new ArgumentNullException(nameof(contract));
        }

        AutomatonDebug.Log($"VectorIndex.CreateGeneration: Entry - fingerprint={contract.Fingerprint[..12]}, dims={contract.Dimensions}");
        // ExecuteScalar<long> and not <long?>: Convert.ChangeType cannot produce a Nullable, so a
        // nullable T throws on every non-null value. DBNull comes back as zero, which is what
        // an empty table should mean here anyway.
        var highest = _db.ExecuteScalar<long>("SELECT MAX(Generation) FROM IndexGenerations");
        var generation = (int)highest + 1;
        var table = ChunkTable(generation);

        _db.ExecuteInTransaction(tx =>
        {
            tx.Execute($@"
                CREATE TABLE IF NOT EXISTS {table} (
                    ChunkId        TEXT PRIMARY KEY,
                    Source         TEXT NOT NULL,
                    StateId        TEXT NOT NULL,
                    ConversationId TEXT NOT NULL,
                    ProjectId      TEXT,
                    Role           TEXT NOT NULL,
                    MessageId      TEXT,
                    MessageIndex   INTEGER NOT NULL,
                    ChunkIndex     INTEGER NOT NULL,
                    CharStart      INTEGER NOT NULL,
                    Text           TEXT NOT NULL,
                    Embedding      BLOB NOT NULL,
                    Origin         TEXT NOT NULL DEFAULT 'raw'
                );
                CREATE INDEX IF NOT EXISTS IX_Chunks_g{generation}_State ON {table}(Source, StateId);
                CREATE INDEX IF NOT EXISTS IX_Chunks_g{generation}_Project ON {table}(ProjectId);");

            tx.Execute(
                "INSERT INTO IndexGenerations (Generation, Fingerprint, Dims, State, CreatedAt) VALUES (@G, @F, @D, @S, @Now)",
                new SqliteParameter("@G", generation),
                new SqliteParameter("@F", contract.Fingerprint),
                new SqliteParameter("@D", contract.Dimensions),
                new SqliteParameter("@S", Building),
                new SqliteParameter("@Now", DateTime.UtcNow.ToString("O")));
        });

        AutomatonDebug.Log($"VectorIndex.CreateGeneration: Exit - generation {generation} building");
        return generation;
    }

    /// <summary>How many chunks a generation holds.</summary>
    public int ChunkCount(int generation)
    {
        try
        {
            return (int)_db.ExecuteScalar<long>($"SELECT COUNT(*) FROM {ChunkTable(generation)}");
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"VectorIndex.ChunkCount: generation {generation}: {ex.Message}");
            return 0;
        }
    }

    /// <summary>
    /// Makes <paramref name="generation"/> the one queries read, and drops every other one.
    /// </summary>
    /// <remarks>
    /// One transaction, so there is no moment where two generations are active or none is. The
    /// old generation's table and its progress rows go together - leaving either behind would
    /// mean a later reconciliation seeing work as already done, or a table nothing can drop.
    /// </remarks>
    public void Activate(int generation)
    {
        AutomatonDebug.Log($"VectorIndex.Activate: Entry - generation {generation}");
        var others = _db
            .Query("SELECT Generation FROM IndexGenerations WHERE Generation <> @G", new SqliteParameter("@G", generation))
            .Select(r => Convert.ToInt32(r["Generation"]))
            .ToList();

        _db.ExecuteInTransaction(tx =>
        {
            tx.Execute(
                "UPDATE IndexGenerations SET State = @State WHERE Generation = @G",
                new SqliteParameter("@State", Active),
                new SqliteParameter("@G", generation));

            foreach (var old in others)
            {
                tx.Execute($"DROP TABLE IF EXISTS {ChunkTable(old)}");
                tx.Execute("DELETE FROM IndexGenerations WHERE Generation = @G", new SqliteParameter("@G", old));
                tx.Execute("DELETE FROM VectorProgress WHERE Generation = @G", new SqliteParameter("@G", old));
            }
        });

        AutomatonDebug.Log($"VectorIndex.Activate: Exit - generation {generation} active, dropped {others.Count} other(s)");
    }

    /// <summary>
    /// Replaces every chunk of one conversation, and records the progress, in one transaction.
    /// </summary>
    /// <remarks>
    /// The delete is keyed by <c>(Source, StateId)</c> and never by chunk id. A conversation that
    /// shrinks from forty chunks to twelve has to lose the other twenty-eight, and a per-id
    /// upsert cannot express that - it would leave the tail of the old conversation in the index,
    /// searchable and wrong. Progress commits with the rows, so a crash cannot leave the index
    /// claiming work it did not do.
    /// </remarks>
    public void ReplaceChunks(
        int generation,
        string fingerprint,
        string source,
        string stateId,
        IReadOnlyList<TranscriptChunk> chunks,
        IReadOnlyList<float[]> vectors,
        string contentFingerprint)
    {
        if (string.IsNullOrWhiteSpace(fingerprint))
        {
            throw new ArgumentException("fingerprint is required", nameof(fingerprint));
        }

        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        if (chunks == null)
        {
            throw new ArgumentNullException(nameof(chunks));
        }

        if (vectors == null)
        {
            throw new ArgumentNullException(nameof(vectors));
        }

        if (chunks.Count != vectors.Count)
        {
            throw new ArgumentException($"{chunks.Count} chunk(s) but {vectors.Count} vector(s).", nameof(vectors));
        }

        // A vector belongs to exactly one generation. Committing one built under a different
        // contract into this table would put incomparable numbers in the same distance query.
        var owner = _db.Query(
            "SELECT Fingerprint FROM IndexGenerations WHERE Generation = @G",
            new SqliteParameter("@G", generation)).FirstOrDefault();

        if (owner == null)
        {
            throw new InvalidOperationException($"Generation {generation} does not exist.");
        }

        if (!string.Equals(owner["Fingerprint"] as string, fingerprint, StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"Generation {generation} holds a different embedding fingerprint; refusing the write.");
        }

        AutomatonDebug.Log($"VectorIndex.ReplaceChunks: Entry - {source}:{stateId} generation {generation}, {chunks.Count} chunk(s)");
        var table = ChunkTable(generation);
        var now = DateTime.UtcNow.ToString("O");

        _db.ExecuteInTransaction(tx =>
        {
            tx.Execute(
                $"DELETE FROM {table} WHERE Source = @Source AND StateId = @StateId",
                new SqliteParameter("@Source", source),
                new SqliteParameter("@StateId", stateId));

            for (var i = 0; i < chunks.Count; i++)
            {
                var chunk = chunks[i];
                tx.Execute($@"
                    INSERT INTO {table}
                        (ChunkId, Source, StateId, ConversationId, ProjectId, Role, MessageId, MessageIndex, ChunkIndex, CharStart, Text, Embedding, Origin)
                    VALUES
                        (@ChunkId, @Source, @StateId, @ConversationId, @ProjectId, @Role, @MessageId, @MessageIndex, @ChunkIndex, @CharStart, @Text, @Embedding, @Origin)",
                    new SqliteParameter("@ChunkId", chunk.ChunkId),
                    new SqliteParameter("@Source", chunk.Source),
                    new SqliteParameter("@StateId", chunk.StateId),
                    new SqliteParameter("@ConversationId", chunk.ConversationId),
                    new SqliteParameter("@ProjectId", (object?)chunk.ProjectId ?? DBNull.Value),
                    new SqliteParameter("@Role", chunk.Role),
                    new SqliteParameter("@MessageId", (object?)chunk.MessageId ?? DBNull.Value),
                    new SqliteParameter("@MessageIndex", chunk.MessageIndex),
                    new SqliteParameter("@ChunkIndex", chunk.ChunkIndex),
                    new SqliteParameter("@CharStart", chunk.CharStart),
                    new SqliteParameter("@Text", chunk.Text),
                    new SqliteParameter("@Embedding", ToBlob(vectors[i])),
                    new SqliteParameter("@Origin", SearchOrigins.Normalize(chunk.Origin)));
            }

            WriteProgress(tx, source, stateId, generation, contentFingerprint, fingerprint, chunks.Count, now);
        });

        AutomatonDebug.Log($"VectorIndex.ReplaceChunks: Exit - {source}:{stateId} now {chunks.Count} chunk(s)");
    }

    /// <summary>
    /// The content fingerprint a conversation's vectors in <paramref name="generation"/> were
    /// cut from, or null when it has none there. The worker asks before it embeds: a
    /// conversation queued because FTS re-indexed it unchanged has nothing to re-embed.
    /// </summary>
    public string? RecordedRevision(int generation, string source, string stateId)
    {
        if (string.IsNullOrWhiteSpace(source) || string.IsNullOrWhiteSpace(stateId))
        {
            return null;
        }

        var row = _db.Query(
            "SELECT VecRevision FROM VectorProgress WHERE Source = @Source AND Id = @Id AND Generation = @G",
            new SqliteParameter("@Source", source),
            new SqliteParameter("@Id", stateId),
            new SqliteParameter("@G", generation)).FirstOrDefault();
        return row?["VecRevision"] as string;
    }

    /// <summary>
    /// Records that a conversation was considered and produced no chunks - every message was a
    /// tool result, or too short. Without this the reconciliation would offer it again forever.
    /// </summary>
    public void RecordEmpty(int generation, string fingerprint, string source, string stateId, string contentFingerprint)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        var now = DateTime.UtcNow.ToString("O");
        _db.ExecuteInTransaction(tx =>
        {
            tx.Execute(
                $"DELETE FROM {ChunkTable(generation)} WHERE Source = @Source AND StateId = @StateId",
                new SqliteParameter("@Source", source),
                new SqliteParameter("@StateId", stateId));

            WriteProgress(tx, source, stateId, generation, contentFingerprint, fingerprint, 0, now);
        });

        AutomatonDebug.Log($"VectorIndex.RecordEmpty: {source}:{stateId} has nothing to embed");
    }

    /// <summary>Drops one conversation's chunks and its progress row - the source is gone.</summary>
    public void RemoveState(int generation, string source, string stateId)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source is required", nameof(source));
        }

        if (string.IsNullOrWhiteSpace(stateId))
        {
            throw new ArgumentException("stateId is required", nameof(stateId));
        }

        _db.ExecuteInTransaction(tx =>
        {
            tx.Execute(
                $"DELETE FROM {ChunkTable(generation)} WHERE Source = @Source AND StateId = @StateId",
                new SqliteParameter("@Source", source),
                new SqliteParameter("@StateId", stateId));

            tx.Execute(
                "DELETE FROM VectorProgress WHERE Source = @Source AND Id = @Id AND Generation = @G",
                new SqliteParameter("@Source", source),
                new SqliteParameter("@Id", stateId),
                new SqliteParameter("@G", generation));
        });

        AutomatonDebug.Log($"VectorIndex.RemoveState: {source}:{stateId} removed from generation {generation}");
    }

    private static void WriteProgress(
        SearchDatabase.SearchTransaction tx,
        string source,
        string stateId,
        int generation,
        string contentFingerprint,
        string fingerprint,
        int chunkCount,
        string now)
    {
        tx.Execute(@"
            INSERT INTO VectorProgress (Source, Id, Generation, VecRevision, VecFingerprint, ChunkCount, IndexedAt)
            VALUES (@Source, @Id, @G, @Rev, @Fingerprint, @Count, @Now)
            ON CONFLICT(Source, Id, Generation) DO UPDATE SET
                VecRevision = excluded.VecRevision,
                VecFingerprint = excluded.VecFingerprint,
                ChunkCount = excluded.ChunkCount,
                IndexedAt = excluded.IndexedAt",
            new SqliteParameter("@Source", source),
            new SqliteParameter("@Id", stateId),
            new SqliteParameter("@G", generation),
            new SqliteParameter("@Rev", contentFingerprint),
            new SqliteParameter("@Fingerprint", fingerprint),
            new SqliteParameter("@Count", chunkCount),
            new SqliteParameter("@Now", now));
    }

    /// <summary>A vector as it is stored: float32, little-endian, the contract's width.</summary>
    public static byte[] ToBlob(float[] vector)
    {
        if (vector == null)
        {
            throw new ArgumentNullException(nameof(vector));
        }

        var blob = new byte[vector.Length * sizeof(float)];
        Buffer.BlockCopy(vector, 0, blob, 0, blob.Length);
        return blob;
    }
}
