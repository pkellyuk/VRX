using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;

namespace VRX.VrexHost;

internal static class QemuRuntimeFiles
{
    internal const string Executable = "qemu-system-x86_64.exe";
    internal const string ReleaseTag = "qemu-windows-e9fa1f66";
    internal const string ArchiveName = "vrx-qemu-win-x64.zip";
    internal const string ReleaseUrl = "https://github.com/pkellyuk/VRX/releases/tag/" + ReleaseTag;
    internal const string ArchiveUrl = "https://github.com/pkellyuk/VRX/releases/download/" + ReleaseTag + "/" + ArchiveName;
    internal const string ArchiveSha256 = "67AC623A68E165A649958469223A193840C1EB2A1EF1F618699E07805CB6EAF8";
    internal const long ArchiveBytes = 143895942;
    private const string Receipt = ".archive-sha256";
    private static readonly HttpClient Downloads = new() { Timeout = TimeSpan.FromMinutes(2) };
    private static readonly string[] RequiredFiles =
        [Executable, "qemu-img.exe", "personabot-build.json", "share/bios-256k.bin"];

    internal static async Task<string> ResolveAsync(string root, Action<string> progress, CancellationToken token)
    {
        // Local development publishes may still include a prepared runtime.
        string bundled = Path.Combine(AppContext.BaseDirectory, "qemu");
        if (HasRequiredFiles(bundled)) return Path.Combine(bundled, Executable);
        return await InstallAsync(root, Downloads, progress, token);
    }

    internal static async Task<string> InstallAsync(string root, HttpClient client, Action<string> progress,
        CancellationToken token)
    {
        string cache = Path.Combine(root, "Qemu");
        string destination = Path.Combine(cache, ReleaseTag);
        string receipt = Path.Combine(destination, Receipt);
        if (HasRequiredFiles(destination) && File.Exists(receipt) &&
            File.ReadAllText(receipt) == ArchiveSha256)
            return Path.Combine(destination, Executable);

        // VrexHost holds the profile's worker lease while installing the runtime.
        Directory.CreateDirectory(cache);
        string staging = Path.Combine(cache, ".install-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        try
        {
            string archive = Path.Combine(staging, ArchiveName);
            await DownloadAsync(client, archive, progress, token);
            progress("Installing the verified QEMU runtime…");
            string extracted = Path.Combine(staging, "runtime");
            ZipFile.ExtractToDirectory(archive, extracted);
            token.ThrowIfCancellationRequested();
            if (!HasRequiredFiles(extracted))
                throw new IOException("The QEMU runtime download is incomplete.");
            File.WriteAllText(Path.Combine(extracted, Receipt), ArchiveSha256);
            if (Directory.Exists(destination))
                Directory.Move(destination, Path.Combine(staging, "previous"));
            Directory.Move(extracted, destination);
            return Path.Combine(destination, Executable);
        }
        finally
        {
            try { Directory.Delete(staging, recursive: true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    private static bool HasRequiredFiles(string folder) =>
        RequiredFiles.All(file => File.Exists(Path.Combine(folder, file)));

    private static async Task DownloadAsync(HttpClient client, string archive, Action<string> progress, CancellationToken token)
    {
        progress("Downloading QEMU from the public VRX release…");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMinutes(30));
        try
        {
            using var response = await client.GetAsync(ArchiveUrl, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is long size && size != ArchiveBytes)
                throw new IOException("The QEMU runtime download has an unexpected size.");

            await using var input = await response.Content.ReadAsStreamAsync(timeout.Token);
            await using var output = File.Create(archive);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            byte[] buffer = new byte[128 * 1024];
            long received = 0, reported = 0;
            int count;
            while ((count = await input.ReadAsync(buffer, timeout.Token)) != 0)
            {
                received += count;
                if (received > ArchiveBytes) throw new IOException("The QEMU runtime download is larger than expected.");
                hash.AppendData(buffer.AsSpan(0, count));
                await output.WriteAsync(buffer.AsMemory(0, count), timeout.Token);
                if (received - reported >= 4 * 1024 * 1024 || received == ArchiveBytes)
                {
                    reported = received;
                    progress($"Downloading QEMU… {100 * received / ArchiveBytes}%");
                }
            }
            if (received != ArchiveBytes || Convert.ToHexString(hash.GetHashAndReset()) != ArchiveSha256)
                throw new IOException("The QEMU runtime download failed its checksum. Start Ubuntu to retry.");
        }
        catch (HttpRequestException ex)
        {
            throw new IOException("Could not download QEMU from the public VRX release. Check your connection and start Ubuntu again.", ex);
        }
        catch (OperationCanceledException ex) when (!token.IsCancellationRequested)
        {
            throw new IOException("The QEMU download timed out. Start Ubuntu to retry.", ex);
        }
    }
}
