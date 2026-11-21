using System.Diagnostics;
using PortAudioSharp;

namespace PersonaBot.Voice;

/// <summary>
/// personabot-voice: PersonaBot's voice, offline. Kokoro speaks, Moonshine listens, both through sherpa-onnx.
///   --serve [--tts &lt;kokoro folder&gt;] [--stt &lt;moonshine folder&gt;] [--vad &lt;silero_vad.onnx&gt;]   the app's mode: requests on stdin, events on stdout
///   --bargetest &lt;kokoro folder&gt; &lt;moonshine folder&gt; &lt;silero_vad.onnx&gt;   tries barge-in offline in a simulated room
///   --wav &lt;kokoro folder&gt; &lt;out.wav&gt; &lt;speaker&gt; &lt;gb|us&gt; &lt;text&gt;      writes speech to a wave file
///   --say &lt;kokoro folder&gt; &lt;speaker&gt; &lt;gb|us&gt; &lt;text&gt;                 says it on the speakers
///   --prep &lt;kokoro folder&gt; &lt;gb|us&gt; &lt;text&gt;                     prints the words Kokoro would be given
///   --hear &lt;moonshine folder&gt; &lt;in.wav&gt;                             prints the words in a 16-bit mono wave file
///   --listen &lt;moonshine folder&gt;                                     listens on the microphone and prints the words
/// It's its own process because sherpa-onnx brings its own ONNX Runtime, and the app already loads another.
/// </summary>
public static class Program
{
    private static void Log(string line) => Console.Error.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff ") + line);

    public static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: personabot-voice --serve|--wav|--say|--hear|--listen ...");
            return 2;
        }
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        Console.InputEncoding = System.Text.Encoding.UTF8;
        try
        {
            switch (args[0])
            {
                case "--serve":
                    return Serve(args);
                case "--wav" when args.Length >= 6:
                    return Wav(args[1], args[2], int.Parse(args[3]), args[4], string.Join(' ', args[5..]));
                case "--say" when args.Length >= 5:
                    return Say(args[1], int.Parse(args[2]), args[3], string.Join(' ', args[4..]));
                case "--prep" when args.Length >= 4:
                    using (var speaker = new Speaker(args[1], Log))
                    {
                        var text = string.Join(' ', args[3..]);
                        Console.WriteLine(speaker.Prepare(text, args[2]));
                        foreach (var chunk in speaker.Phonemes(text, args[2])) Console.WriteLine("  [" + chunk + "]");
                    }
                    return 0;
                case "--hear" when args.Length >= 3:
                    return Hear(args[1], args[2]);
                case "--listen" when args.Length >= 2:
                    return Listen(args[1]);
                case "--bargetest" when args.Length >= 4:
                    return BargeTest.Run(args[1], args[2], args[3], Log);
                default:
                    Console.Error.WriteLine("Unknown or incomplete mode " + args[0]);
                    return 2;
            }
        }
        catch (Exception ex)
        {
            Log("personabot-voice failed: " + ex);
            return 1;
        }
    }

    private static string? Option(string[] args, string name)
    {
        var at = Array.IndexOf(args, name);
        return at >= 0 && at + 1 < args.Length ? args[at + 1] : null;
    }

    private static int Serve(string[] args)
    {
        var tts = Option(args, "--tts");
        var stt = Option(args, "--stt");
        var vad = Option(args, "--vad");
        Log($"Serve: tts={tts ?? "none"} ({Speaker.IsModel(tts)}), stt={stt ?? "none"} ({Hearer.IsModel(stt)}), vad={vad ?? "none"} ({SileroDetector.IsModel(vad)})");
        PortAudio.Initialize();
        try
        {
            using var speaker = Speaker.IsModel(tts) ? new Speaker(tts!, Log) : null;
            using var hearer = Hearer.IsModel(stt) ? new Hearer(stt!, Log) : null;
            using var detector = SileroDetector.IsModel(vad) ? new SileroDetector(vad!, Log) : null;
            using var server = new VoiceServer(speaker, hearer, detector, Console.Out, Log);
            server.Run(Console.In);
        }
        finally
        {
            PortAudio.Terminate();
        }
        return 0;
    }

    private static int Wav(string folder, string file, int speaker, string accent, string text)
    {
        using var tts = new Speaker(folder, Log);
        var samples = new List<float>();
        var watch = Stopwatch.StartNew();
        // PB_VOICE_REPEAT=n says it n times, to time a warm engine (the first run includes loading).
        var repeats = int.TryParse(Environment.GetEnvironmentVariable("PB_VOICE_REPEAT"), out var n) ? Math.Max(1, n) : 1;
        for (var r = 0; r < repeats; r++)
        {
            samples.Clear();
            watch.Restart();
            tts.Say(text, speaker, accent, 1f, samples.AddRange, CancellationToken.None);
        }
        WaveFile.Write(file, samples.ToArray(), tts.SampleRate);
        Console.WriteLine($"{samples.Count / (double)tts.SampleRate:0.0}s of speech made in {watch.ElapsedMilliseconds} ms -> {file}");
        return 0;
    }

    private static int Say(string folder, int speaker, string accent, string text)
    {
        PortAudio.Initialize();
        try
        {
            using var tts = new Speaker(folder, Log);
            using var output = new AudioOut(Log);
            output.Begin(tts.SampleRate);
            tts.Say(text, speaker, accent, 1f, output.Add, CancellationToken.None);
            output.End();
            while (!output.Drained) Thread.Sleep(20);
        }
        finally
        {
            PortAudio.Terminate();
        }
        return 0;
    }

    private static int Hear(string folder, string file)
    {
        var (samples, rate) = WaveFile.Read(file);
        using var hearer = new Hearer(folder, Log);
        hearer.Warm();
        var watch = Stopwatch.StartNew();
        var text = hearer.Transcribe(samples, rate);
        Console.WriteLine($"[{samples.Length / (double)rate:0.0}s in {watch.ElapsedMilliseconds} ms] {text}");
        return 0;
    }

    private static int Listen(string folder)
    {
        PortAudio.Initialize();
        try
        {
            using var hearer = new Hearer(folder, Log);
            hearer.Warm();
            using var input = new AudioIn(Log);
            var endpointer = new Endpointer(1.2f, 30f);
            var samples = new List<float>();
            using var done = new ManualResetEventSlim();
            input.Samples += (block, _) =>
            {
                if (done.IsSet) return;
                samples.AddRange(block);
                endpointer.Add(block, AudioIn.SampleRate);
                if (endpointer.Done) done.Set();
            };
            Console.WriteLine("Speak now…");
            input.Start();
            done.Wait();
            input.Stop();
            Console.WriteLine($"Stopped ({endpointer.Reason}) after {endpointer.Elapsed:0.0}s; heard speech: {endpointer.HeardSpeech}");
            Console.WriteLine(hearer.Transcribe(samples.ToArray(), AudioIn.SampleRate));
        }
        finally
        {
            PortAudio.Terminate();
        }
        return 0;
    }
}
