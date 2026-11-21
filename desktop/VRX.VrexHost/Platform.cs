using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace PersonaBot;

// The upstream guest runtime uses this helper to validate captured PNG frames.
internal static class WindowsPlatform
{
    public static (int Width, int Height) PngSize(byte[] png)
    {
        if (png.Length < 24 || !png.AsSpan(0, 8).SequenceEqual(new byte[] {137,80,78,71,13,10,26,10}))
            throw new IOException("The Ubuntu guest did not supply a PNG image.");
        return (BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(16,4)), BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(20,4)));
    }
}

internal static partial class QemuAcceleration
{
    internal const string Whpx = "whpx", Tcg = "tcg";
    [LibraryImport("WinHvPlatform.dll")]
    private static partial int WHvGetCapability(int capability, out int value, uint size, out uint written);
    internal static string Select()
    {
        try { return WHvGetCapability(0, out int present, 4, out _) == 0 && present != 0 ? Whpx : Tcg; }
        catch (DllNotFoundException) { return Tcg; }
        catch (EntryPointNotFoundException) { return Tcg; }
    }
    internal static IReadOnlyList<string> Arguments(string accelerator, string architecture)
    {
        if (architecture != "x86_64") throw new NotSupportedException("Vrex supports only x86-64 Ubuntu.");
        return accelerator switch
        {
            Whpx => ["-accel", "whpx"],
            Tcg => ["-accel", "tcg,thread=multi", "-cpu", "max"],
            _ => throw new ArgumentException("Unsupported Ubuntu accelerator.")
        };
    }
}

internal sealed class VrexKeyStore : IKeyStore
{
    public string Protect(string plain) => Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), null, DataProtectionScope.CurrentUser));
    public string Unprotect(string value) => Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(value), null, DataProtectionScope.CurrentUser));
}
