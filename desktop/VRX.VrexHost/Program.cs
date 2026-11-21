using System.Diagnostics;
using System.Text.Json;
using PersonaBot;
using Automaton.Core.Services;
using VRX.VrexBridge;

namespace VRX.VrexHost;

internal static class Program
{
    private static readonly object outputLock = new();
    internal static void Emit(VrexEvent message)
    {
        lock (outputLock) { try {Console.WriteLine(JsonSerializer.Serialize(message)); Console.Out.Flush();} catch(IOException) { } }
    }
    public static async Task<int> Main(string[] args)
    {
        string root = Path.Combine(VrxDataDirectory.Root, "Vrex");
        if (args.Contains("--self-test")) root = Path.Combine(Path.GetTempPath(), "VRX-VrexTests", Guid.NewGuid().ToString("N"));
        if (args.Contains("--voice-self-test")) root = Path.Combine(Path.GetTempPath(), "VRX-VoiceTests");
        bool testUbuntu = args.Contains("--ubuntu-self-test");
        if (testUbuntu) root = Path.Combine(Path.GetTempPath(), "VRX-UbuntuTests", "verification");
        Settings.DataDirectory = root; Settings.KeyStore = new VrexKeyStore();
        AutomatonPaths.ConfigureHomeDirectory(Path.Combine(root, "agent"));
        Directory.CreateDirectory(root);
        Trace.Listeners.Add(new TextWriterTraceListener(Path.Combine(root, "vrex.log"))); Trace.AutoFlush = true;
        if (args.Contains("--self-test")) return await VrexSelfTest.RunAsync();
        if (args.Contains("--voice-self-test")) return await VrexSelfTest.VoiceAsync();
        if (testUbuntu) return await VrexSelfTest.UbuntuAsync(root);
        string? channel = args.FirstOrDefault(a => a.StartsWith("--channel=", StringComparison.Ordinal))?[10..];
        if (channel == null || !VrexProtocol.IsChannel(channel)) { Console.Error.WriteLine("A private VRX Vrex channel is required."); return 2; }
        try
        {
            using var lease = MachineLease.Acquire(root);
            await using var session = new VrexSession(channel, root, Emit, args.Contains("--verify-vm"));
            session.ReportSettings();
            while (await Console.In.ReadLineAsync() is { } line)
            {
                if (line.Length > VrexProtocol.MaxRequestChars) { Emit(new("error", "Vrex request exceeds the input limit.")); continue; }
                try
                {
                    var command = JsonSerializer.Deserialize<VrexRequest>(line) ?? throw new ArgumentException("Empty Vrex request.");
                    if (command.Type == "stop") break;
                    await session.HandleAsync(command);
                }
                catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or JsonException or IOException or UnauthorizedAccessException)
                { Emit(new("error", ex.Message)); }
            }
            return 0;
        }
        catch (Exception ex) { Emit(new("error", ex.Message)); return 1; }
    }
}
