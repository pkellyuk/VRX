namespace Vrx.Linux;

// vrx-linux --check: the controller's non-UI behaviour, runnable without a
// display (CTest). Returns 0 when every check passes.
public static class Checks
{
    public static int Run()
    {
        var failures = 0;
        void Expect(bool condition, string what)
        {
            if (condition) return;
            Console.Error.WriteLine($"FAILED: {what}");
            ++failures;
        }
        bool Throws(Action action)
        {
            try { action(); return false; } catch (ArgumentException) { return true; }
        }

        // The snapshot is exactly what live_settings.h parses, in any culture.
        Expect(LiveSettings.Snapshot(LinuxProfile.Default) ==
               "VRXL 3 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 0\n", "default VRXL 3 snapshot");
        Expect(LiveSettings.Snapshot(LinuxProfile.Default, 7).EndsWith(" 16757867 7\n"), "recenter counter in the snapshot");
        var previous = Thread.CurrentThread.CurrentCulture;
        try
        {
            Thread.CurrentThread.CurrentCulture = new System.Globalization.CultureInfo("de-DE");
            Expect(LiveSettings.Snapshot(LinuxProfile.Default with { Width = 2.5 }).StartsWith("VRXL 3 2.500 "),
                   "snapshot ignores the current culture");
        }
        catch (System.Globalization.CultureNotFoundException) { }   // invariant-globalization builds
        finally { Thread.CurrentThread.CurrentCulture = previous; }

        // Values outside the engine's ranges are rejected.
        Expect(Throws(() => (LinuxProfile.Default with { Width = 11 }).Normalized()), "width above range");
        Expect(Throws(() => (LinuxProfile.Default with { Distance = double.NaN }).Normalized()), "distance NaN");
        Expect(Throws(() => (LinuxProfile.Default with { Room = 101 }).Normalized()), "room above range");
        Expect(Throws(() => (LinuxProfile.Default with { LightColor = "FFB46B" }).Normalized()), "colour without #");
        Expect((LinuxProfile.Default with { LightColor = " #ffb46b " }).Normalized().LightColor == "#FFB46B",
               "colour normalised");

        // Profiles round-trip, and version 1 files (no room controls) load with defaults.
        var directory = Directory.CreateTempSubdirectory("vrx-linux-check-");
        try
        {
            var path = Path.Combine(directory.FullName, "profiles.json");
            var saved = new SortedDictionary<string, LinuxProfile>(StringComparer.Ordinal)
            {
                ["Default"] = LinuxProfile.Default,
                ["Game"] = LinuxProfile.Default with { Cuda = false, Width = 3.25, Room = 0, LightColor = "#102030" },
            };
            ProfileStore.Save(path, saved);
            var loaded = ProfileStore.Load(path);
            Expect(loaded.Count == 2 && loaded["Game"] == saved["Game"] && loaded["Default"] == saved["Default"],
                   "profiles round-trip");
            File.WriteAllText(path, """{"version": 1, "profiles": {"Old": {"cuda": true, "width": 3.0}}}""");
            var old = ProfileStore.Load(path)["Old"];
            Expect(old.Width == 3.0 && old.Room == LinuxProfile.Default.Room, "version 1 profile defaults");
            File.WriteAllText(path, """{"version": 3, "profiles": {}}""");
            Expect(ThrowsData(() => ProfileStore.Load(path)), "unknown profile version rejected");
            File.WriteAllText(path, """{"version": 2, "profiles": {"Bad": {"cuda": true, "room": 30.5}}}""");
            Expect(ThrowsData(() => ProfileStore.Load(path)), "fractional room rejected");
            Expect(ProfileStore.Load(Path.Combine(directory.FullName, "missing.json")).ContainsKey("Default"),
                   "missing file gives the default profile");
        }
        finally { directory.Delete(recursive: true); }

        // The engine is started as the PyQt controller started it, with the chosen source.
        var paths = new EnginePaths("/repo", "/repo/build/linux-release/vrx-xr-synthetic", "/repo/model.onnx");
        Expect(string.Join(' ', EngineSession.Arguments(paths, "/tmp/s", true, "--source=window")) ==
               "--until-stop --live --room --source=window --settings=/tmp/s --cuda --model=/repo/model.onnx",
               "engine arguments");

        Console.WriteLine(failures == 0 ? "vrx-linux checks passed" : $"vrx-linux checks: {failures} failed");
        return failures == 0 ? 0 : 1;
    }

    static bool ThrowsData(Action action)
    {
        try { action(); return false; } catch (InvalidDataException) { return true; }
    }
}
