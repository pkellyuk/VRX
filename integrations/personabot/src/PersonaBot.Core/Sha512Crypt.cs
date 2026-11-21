using System.Security.Cryptography;
using System.Text;

namespace PersonaBot;

/// <summary>
/// Linux's SHA-512 password hash ("$6$salt$hash", glibc crypt), as /etc/shadow stores it, so PersonaBot can set
/// the person's Linux password on every machine without ever keeping the password itself. Follows Ulrich
/// Drepper's "Unix crypt using SHA-256 and SHA-512" (https://www.akkadia.org/drepper/SHA-crypt.txt), whose
/// test vectors are in Sha512CryptTests.
/// </summary>
public static class Sha512Crypt
{
    private const string Prefix = "$6$";
    private const string RoundsPrefix = "rounds=";
    private const int DefaultRounds = 5000;
    private const int MinRounds = 1000;
    private const int MaxRounds = 999_999_999;
    private const int MaxSaltLength = 16;
    private const string Alphabet = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    /// <summary>A new hash of <paramref name="password"/> with a random 16-character salt and the default 5000 rounds.</summary>
    public static string Hash(string password)
    {
        ArgumentNullException.ThrowIfNull(password);
        var salt = new char[MaxSaltLength];
        for (var i = 0; i < salt.Length; i++) salt[i] = Alphabet[RandomNumberGenerator.GetInt32(Alphabet.Length)];
        return Hash(password, Prefix + new string(salt));
    }

    /// <summary>
    /// The hash of <paramref name="password"/> for a setting ("$6$salt" or "$6$rounds=N$salt", optionally with a
    /// trailing "$" and hash, which is ignored), as crypt(3) computes it.
    /// </summary>
    public static string Hash(string password, string setting)
    {
        ArgumentNullException.ThrowIfNull(password);
        ArgumentNullException.ThrowIfNull(setting);
        // Never the password, the salt or the hash: only what was refused, the rounds and the time.
        if (!setting.StartsWith(Prefix, StringComparison.Ordinal))
        {
            System.Diagnostics.Trace.TraceWarning("Sha512Crypt.Hash: refused a setting that isn't $6$");
            throw new ArgumentException("A SHA-512 crypt setting starts with $6$.", nameof(setting));
        }
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var rest = setting[Prefix.Length..];
        var rounds = DefaultRounds;
        var customRounds = false;
        if (rest.StartsWith(RoundsPrefix, StringComparison.Ordinal))
        {
            var end = rest.IndexOf('$');
            if (end < 0 || !long.TryParse(rest[RoundsPrefix.Length..end], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var asked))
            {
                System.Diagnostics.Trace.TraceWarning("Sha512Crypt.Hash: refused a setting whose rounds aren't a number");
                throw new ArgumentException("The rounds in the setting aren't a number.", nameof(setting));
            }
            rounds = (int)Math.Clamp(asked, MinRounds, MaxRounds);
            customRounds = true;
            rest = rest[(end + 1)..];
        }
        var saltEnd = rest.IndexOf('$');
        var saltText = saltEnd < 0 ? rest : rest[..saltEnd];
        if (saltText.Length > MaxSaltLength) saltText = saltText[..MaxSaltLength];
        var key = Encoding.UTF8.GetBytes(password);
        var salt = Encoding.UTF8.GetBytes(saltText);

        // Digest B: key, salt, key.
        var b = SHA512.HashData([.. key, .. salt, .. key]);
        // Digest A: key, salt, then B repeated over the key's length, then key or B for each bit of the length.
        using var a = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
        a.AppendData(key);
        a.AppendData(salt);
        AppendRepeated(a, b, key.Length);
        for (var length = key.Length; length > 0; length >>= 1)
        {
            a.AppendData((length & 1) != 0 ? b : key);
        }
        var digestA = a.GetHashAndReset();
        // P: the key repeated key.Length times, digested; S: the salt repeated 16 + A[0] times, digested.
        using var dp = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
        for (var i = 0; i < key.Length; i++) dp.AppendData(key);
        var p = Produce(dp.GetHashAndReset(), key.Length);
        using var ds = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
        for (var i = 0; i < 16 + digestA[0]; i++) ds.AppendData(salt);
        var s = Produce(ds.GetHashAndReset(), salt.Length);
        // The rounds.
        var c = digestA;
        using var round = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
        for (var i = 0; i < rounds; i++)
        {
            round.AppendData((i & 1) != 0 ? p : c);
            if (i % 3 != 0) round.AppendData(s);
            if (i % 7 != 0) round.AppendData(p);
            round.AppendData((i & 1) != 0 ? c : p);
            c = round.GetHashAndReset();
        }

        var output = new StringBuilder(Prefix);
        if (customRounds) output.Append(RoundsPrefix).Append(rounds.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append('$');
        output.Append(saltText).Append('$');
        int[][] groups =
        [
            [0, 21, 42], [22, 43, 1], [44, 2, 23], [3, 24, 45], [25, 46, 4], [47, 5, 26], [6, 27, 48], [28, 49, 7],
            [50, 8, 29], [9, 30, 51], [31, 52, 10], [53, 11, 32], [12, 33, 54], [34, 55, 13], [56, 14, 35], [15, 36, 57],
            [37, 58, 16], [59, 17, 38], [18, 39, 60], [40, 61, 19], [62, 20, 41]
        ];
        foreach (var g in groups) Encode(output, (c[g[0]] << 16) | (c[g[1]] << 8) | c[g[2]], 4);
        Encode(output, c[63], 2);
        System.Diagnostics.Trace.WriteLine($"Sha512Crypt.Hash: {rounds} rounds{(customRounds ? " (custom)" : "")} in {watch.ElapsedMilliseconds} ms");
        return output.ToString();
    }

    /// <summary>Whether a text is a SHA-512 crypt hash as Linux stores it.</summary>
    public static bool IsHash(string? text) =>
        text != null && System.Text.RegularExpressions.Regex.IsMatch(text, @"^\$6\$(rounds=\d+\$)?[./0-9A-Za-z]{1,16}\$[./0-9A-Za-z]{86}$");

    private static void AppendRepeated(IncrementalHash hash, byte[] block, int length)
    {
        for (var left = length; left > 0; left -= block.Length)
        {
            hash.AppendData(block, 0, Math.Min(block.Length, left));
        }
    }

    /// <summary>A digest repeated to <paramref name="length"/> bytes.</summary>
    private static byte[] Produce(byte[] digest, int length)
    {
        var result = new byte[length];
        for (var i = 0; i < length; i++) result[i] = digest[i % digest.Length];
        return result;
    }

    private static void Encode(StringBuilder output, int value, int characters)
    {
        for (var i = 0; i < characters; i++)
        {
            output.Append(Alphabet[value & 0x3f]);
            value >>= 6;
        }
    }
}
