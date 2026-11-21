using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Automaton.Core.Services.Search;

/// <summary>
/// Everything that decides whether two vectors can be compared, in one place.
/// </summary>
/// <remarks>
/// <para>
/// A model name and a dimension are not enough to identify compatible vectors. The instruction
/// the query was embedded with, the pooling mode the server ran, whether vectors were normalised,
/// how text was chunked and which distance the index uses all change what a stored vector means.
/// The <see cref="Fingerprint"/> hashes all of them, so any change produces a new vector
/// generation (see <see cref="VectorIndex"/>) instead of silently comparing incomparable numbers.
/// </para>
/// <para>
/// The chunking constants are hashed individually rather than only through
/// <see cref="ChunkingVersion"/>. A version integer someone has to remember to bump is a
/// silent-corruption trap: change <see cref="ChunkCharacters"/> and forget, and the old and new
/// chunk sets sit in one table, comparable in the arithmetic sense and meaningless in every
/// other. Hashing the values themselves makes that impossible. The version stays for changes to
/// the chunking <em>algorithm</em> that leave the constants alone.
/// </para>
/// <para>
/// The instruction wording and the pooling mode below are what the Qwen3-Embedding model card
/// specified when this was written. Check both against the model card and the current llama.cpp
/// release before relying on recall quality; a wrong pooling mode produces vectors that are all
/// wrong in a way no test notices, which is what the recall smoke test in SEARCH.md is for.
/// </para>
/// </remarks>
public sealed class EmbeddingContract
{
    /// <summary>
    /// Bump when the chunking algorithm changes but the constants below do not. Version 2:
    /// the transcript chunked is the full one (raw messages, then compaction summaries), each
    /// chunk carries an origin, and summaries are embedded whatever role carries them.
    /// </summary>
    public const int ChunkingVersion = 2;

    /// <summary>Pooling the server is started with. Last-token, for the Qwen3 embedding family.</summary>
    public const string Pooling = "last";

    /// <summary>Distance the vector tables use. Cosine over L2-normalised vectors.</summary>
    public const string Metric = "cosine";

    /// <summary>The retrieval instruction queries carry; documents carry none.</summary>
    public const string QueryInstruction = "Instruct: Given a question, retrieve passages from past conversations that answer it\nQuery: ";

    /// <summary>Longest chunk, in characters (a rough stand-in for ~512 tokens of English).</summary>
    public const int ChunkCharacters = 2000;

    /// <summary>Overlap between neighbouring chunks of one message, in characters.</summary>
    public const int ChunkOverlap = 250;

    /// <summary>
    /// Shortest message worth embedding. "ok", "yes, do that", "thanks" carry no meaning to
    /// retrieve; they embed to a generic point that is weakly near every query, so they crowd
    /// out real hits while never being the message anyone was looking for.
    /// </summary>
    public const int MinChunkCharacters = 32;

    /// <summary>
    /// Most chunks one message may produce. A single pathological message - the largest in this
    /// corpus is over 400 KB - must not dominate a generation.
    /// </summary>
    public const int MaxChunksPerMessage = 24;

    /// <summary>Longest message considered at all, mirroring the FTS write path's truncation.</summary>
    public const int MaxMessageCharacters = 100_000;

    /// <summary>The roles that are embedded. Tool output and system prompts are not; see SEARCH.md.</summary>
    public const string EmbeddedRoles = "user,assistant";

    public EmbeddingContract(string modelPath, int dimensions)
    {
        if (string.IsNullOrWhiteSpace(modelPath))
        {
            throw new ArgumentException("An embedding model path is required.", nameof(modelPath));
        }

        if (dimensions <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(dimensions), "Dimensions must be positive.");
        }

        ModelPath = modelPath;
        Dimensions = dimensions;
        Fingerprint = ComputeFingerprint(modelPath, dimensions);
        AutomatonDebug.Log($"EmbeddingContract: model={Path.GetFileName(modelPath)}, dims={dimensions}, fingerprint={Fingerprint[..12]}");
    }

    public string ModelPath { get; }

    public int Dimensions { get; }

    /// <summary>Hex SHA-256 over every input that changes what a vector means.</summary>
    public string Fingerprint { get; }

    /// <summary>The model file's name, for the server's model field and for logs.</summary>
    public string ModelName => Path.GetFileName(ModelPath);

    /// <summary>
    /// What a transcript chunk is embedded as. The role is included because "user: how do I..."
    /// and "assistant: you do it by..." are different kinds of text, and the model is asked to
    /// retrieve passages rather than facts.
    /// </summary>
    public static string DocumentText(string role, string chunk)
    {
        if (string.IsNullOrWhiteSpace(chunk))
        {
            throw new ArgumentException("A chunk is required.", nameof(chunk));
        }

        return string.IsNullOrWhiteSpace(role) ? chunk.Trim() : $"{role.Trim()}: {chunk.Trim()}";
    }

    /// <summary>What a search query is embedded as: the instruction, then the question.</summary>
    public static string QueryText(string query)
    {
        if (string.IsNullOrWhiteSpace(query))
        {
            throw new ArgumentException("A query is required.", nameof(query));
        }

        return QueryInstruction + query.Trim();
    }

    /// <summary>
    /// Splits one message into chunks that never span messages. Most messages produce one chunk;
    /// a long one produces windows of <see cref="ChunkCharacters"/> overlapping by
    /// <see cref="ChunkOverlap"/>, so a sentence at a cut appears in both neighbours.
    /// </summary>
    /// <remarks>
    /// Never spanning a message boundary is not a simplification, it is a requirement: a chunk
    /// has to map to exactly one message so the search dialog can scroll to it. See
    /// <see cref="TranscriptChunker"/>.
    /// </remarks>
    public static IReadOnlyList<string> Chunk(string? content)
    {
        if (string.IsNullOrWhiteSpace(content))
        {
            return Array.Empty<string>();
        }

        var text = content.Trim();
        if (text.Length > MaxMessageCharacters)
        {
            text = text[..MaxMessageCharacters];
        }

        if (text.Length <= ChunkCharacters)
        {
            return new[] { text };
        }

        var chunks = new List<string>();
        var step = ChunkCharacters - ChunkOverlap;
        for (var start = 0; start < text.Length; start += step)
        {
            var length = Math.Min(ChunkCharacters, text.Length - start);
            chunks.Add(text.Substring(start, length));

            if (chunks.Count >= MaxChunksPerMessage || start + length >= text.Length)
            {
                break;
            }
        }

        return chunks;
    }

    /// <summary>
    /// The character offset within the trimmed message where chunk <paramref name="chunkIndex"/>
    /// starts. Recorded with each chunk so a later change can highlight the matching region.
    /// </summary>
    public static int ChunkStart(int chunkIndex)
    {
        if (chunkIndex <= 0)
        {
            return 0;
        }

        return chunkIndex * (ChunkCharacters - ChunkOverlap);
    }

    /// <summary>
    /// The fingerprint over the model file (name, size and modified time stand in for a full
    /// hash of a multi-gigabyte file), the instruction, pooling, normalisation, every chunking
    /// constant, the roles embedded, the granularity, the dimension and the metric.
    /// </summary>
    public static string ComputeFingerprint(string modelPath, int dimensions)
    {
        if (string.IsNullOrWhiteSpace(modelPath))
        {
            throw new ArgumentException("An embedding model path is required.", nameof(modelPath));
        }

        var builder = new StringBuilder();
        builder.Append("model=").Append(Path.GetFileName(modelPath)).Append('\n');
        try
        {
            var info = new FileInfo(modelPath);
            if (info.Exists)
            {
                builder.Append("size=").Append(info.Length).Append('\n');
                builder.Append("modified=").Append(info.LastWriteTimeUtc.Ticks).Append('\n');
            }
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"EmbeddingContract.ComputeFingerprint: could not stat {modelPath}: {ex.Message}");
        }

        builder.Append("instruction=").Append(QueryInstruction).Append('\n');
        builder.Append("pooling=").Append(Pooling).Append('\n');
        builder.Append("normalised=l2\n");
        builder.Append("chunking=").Append(ChunkingVersion).Append('\n');
        builder.Append("chunkChars=").Append(ChunkCharacters).Append('\n');
        builder.Append("chunkOverlap=").Append(ChunkOverlap).Append('\n');
        builder.Append("minChars=").Append(MinChunkCharacters).Append('\n');
        builder.Append("maxChunks=").Append(MaxChunksPerMessage).Append('\n');
        builder.Append("maxMessage=").Append(MaxMessageCharacters).Append('\n');
        builder.Append("roles=").Append(EmbeddedRoles).Append('\n');
        builder.Append("origins=raw,compressed\n");
        builder.Append("granularity=message\n");
        builder.Append("dims=").Append(dimensions).Append('\n');
        builder.Append("metric=").Append(Metric).Append('\n');

        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(builder.ToString())));
    }
}
