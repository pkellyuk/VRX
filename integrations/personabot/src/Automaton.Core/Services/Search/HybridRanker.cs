namespace Automaton.Core.Services.Search;

/// <summary>How a result was found.</summary>
public static class MatchKinds
{
    /// <summary>Found by keyword only.</summary>
    public const string Lexical = "lexical";

    /// <summary>Found by meaning only - the query's words are not necessarily in it.</summary>
    public const string Semantic = "semantic";

    /// <summary>Found both ways.</summary>
    public const string Both = "both";
}

/// <summary>One item and where it placed in each list it appeared in.</summary>
public sealed record FusedHit<T>(T Item, double Score, int? LexicalRank, int? SemanticRank)
{
    /// <summary>Which lists this item came from.</summary>
    public string MatchKind =>
        LexicalRank.HasValue && SemanticRank.HasValue ? MatchKinds.Both
        : SemanticRank.HasValue ? MatchKinds.Semantic
        : MatchKinds.Lexical;
}

/// <summary>
/// Merges a keyword result list and a semantic one into a single order.
/// </summary>
/// <remarks>
/// <para>
/// The two lists carry numbers that cannot be compared. bm25 is unbounded and negative, and its
/// scale depends on the corpus and the query's term statistics; cosine distance is between 0 and
/// 2 and depends on the embedding model. There is no honest constant that maps one onto the
/// other, and inventing one - normalising each list into 0..1, say - manufactures a precision
/// that is not in the data and shifts every time the corpus grows.
/// </para>
/// <para>
/// Reciprocal Rank Fusion sidesteps that by using only the <em>positions</em>. Each list
/// contributes <c>1 / (k + rank)</c>, and the constant k (60 is the value from the original
/// paper, and what everyone uses) flattens the difference between the first few places so a
/// confident second place is not swamped by an accidental first. It needs no calibration, it
/// cannot drift, and it has the property that matters most here: with an empty semantic list it
/// reproduces the lexical order exactly, so nothing regresses when no model is configured.
/// </para>
/// </remarks>
public static class HybridRanker
{
    /// <summary>The RRF constant. 60 is the standard value.</summary>
    public const int DefaultK = 60;

    /// <summary>
    /// Fuses two ranked lists. Items are matched by <paramref name="key"/>; ties break on the
    /// lexical position and then the key, so the order is deterministic.
    /// </summary>
    public static IReadOnlyList<FusedHit<T>> Fuse<T>(
        IReadOnlyList<T>? lexical,
        IReadOnlyList<T>? semantic,
        Func<T, string> key,
        int take,
        int k = DefaultK)
    {
        if (key == null)
        {
            throw new ArgumentNullException(nameof(key));
        }

        if (take <= 0)
        {
            return Array.Empty<FusedHit<T>>();
        }

        lexical ??= Array.Empty<T>();
        semantic ??= Array.Empty<T>();

        if (semantic.Count == 0)
        {
            // The no-model, no-server and no-generation path. Order preserved exactly.
            return lexical
                .Take(take)
                .Select((item, index) => new FusedHit<T>(item, 1.0 / (k + index), index, null))
                .ToList();
        }

        var order = new List<string>();
        var items = new Dictionary<string, T>(StringComparer.Ordinal);
        var lexicalRanks = new Dictionary<string, int>(StringComparer.Ordinal);
        var semanticRanks = new Dictionary<string, int>(StringComparer.Ordinal);

        void Note(string id, T item)
        {
            if (items.TryAdd(id, item))
            {
                order.Add(id);
            }
        }

        for (var i = 0; i < lexical.Count; i++)
        {
            var id = key(lexical[i]);
            if (string.IsNullOrEmpty(id) || lexicalRanks.ContainsKey(id))
            {
                continue;
            }

            lexicalRanks[id] = i;
            Note(id, lexical[i]);
        }

        for (var i = 0; i < semantic.Count; i++)
        {
            var id = key(semantic[i]);
            if (string.IsNullOrEmpty(id) || semanticRanks.ContainsKey(id))
            {
                continue;
            }

            semanticRanks[id] = i;

            // A lexical hit for the same key wins the item itself: it carries the highlighted
            // snippet, which is what a reader wants to see when both agree.
            Note(id, semantic[i]);
        }

        return order
            .Select(id =>
            {
                lexicalRanks.TryGetValue(id, out var lexicalRank);
                var hasLexical = lexicalRanks.ContainsKey(id);
                semanticRanks.TryGetValue(id, out var semanticRank);
                var hasSemantic = semanticRanks.ContainsKey(id);

                var score = 0.0;
                if (hasLexical)
                {
                    score += 1.0 / (k + lexicalRank);
                }

                if (hasSemantic)
                {
                    score += 1.0 / (k + semanticRank);
                }

                return new FusedHit<T>(
                    items[id],
                    score,
                    hasLexical ? lexicalRank : null,
                    hasSemantic ? semanticRank : null);
            })
            .OrderByDescending(h => h.Score)
            .ThenBy(h => h.LexicalRank ?? int.MaxValue)
            .ThenBy(h => key(h.Item), StringComparer.Ordinal)
            .Take(take)
            .ToList();
    }
}
