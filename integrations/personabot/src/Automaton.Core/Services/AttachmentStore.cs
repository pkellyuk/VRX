using System;
using System.IO;
using System.Text.RegularExpressions;

namespace Automaton.Core.Services;

/// <summary>
/// Where a conversation's attachment bytes live.
/// </summary>
/// <remarks>
/// <para>
/// On disk under <c>~/.automaton/attachments</c>, with only the metadata - id,
/// name, media type - stored beside the message. Images are capped at a few
/// hundred kilobytes each after downscaling, so a few hundred of them inlined
/// would turn a small settings database into a few hundred megabytes, alongside
/// everything else that shares it. Files also stay easy to inspect, back up and
/// prune.
/// </para>
/// <para>
/// The id is the file name, so it is validated rather than trusted: an id comes
/// back from stored data and must never be able to point outside this folder.
/// </para>
/// </remarks>
public static class AttachmentStore
{
    /// <summary>Ids are GUIDs as issued; anything else is refused.</summary>
    private static readonly Regex SafeId = new("^[A-Za-z0-9_-]{1,128}$", RegexOptions.Compiled);

    public static string Directory => Path.Combine(AutomatonPaths.HomeDirectory, "attachments");

    /// <summary>
    /// Writes <paramref name="bytes"/> under <paramref name="id"/>, and returns
    /// whether it is now stored. Existing content is left alone: an id names one
    /// immutable payload, so rewriting it would only risk a partial file.
    /// </summary>
    public static bool Save(string id, byte[] bytes)
    {
        if (!SafeId.IsMatch(id ?? string.Empty) || bytes is null || bytes.Length == 0)
        {
            AutomatonDebug.LogWarning($"AttachmentStore.Save: refusing id='{id}' ({bytes?.Length ?? 0} bytes)");
            return false;
        }

        try
        {
            System.IO.Directory.CreateDirectory(Directory);
            var path = Path.Combine(Directory, id);

            if (File.Exists(path))
            {
                return true;
            }

            // Written beside the target and moved into place, so a crash midway
            // cannot leave a truncated file that later reads as a broken image.
            var temp = path + ".tmp";
            File.WriteAllBytes(temp, bytes);
            File.Move(temp, path, overwrite: true);
            return true;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"AttachmentStore.Save: could not store '{id}': {ex.Message}");
            return false;
        }
    }

    /// <summary>The stored bytes, or <see langword="null"/> when there are none.</summary>
    public static byte[]? TryLoad(string id)
    {
        if (!SafeId.IsMatch(id ?? string.Empty))
        {
            return null;
        }

        try
        {
            var path = Path.Combine(Directory, id!);
            return File.Exists(path) ? File.ReadAllBytes(path) : null;
        }
        catch (Exception ex)
        {
            AutomatonDebug.LogWarning($"AttachmentStore.TryLoad: could not read '{id}': {ex.Message}");
            return null;
        }
    }
}
