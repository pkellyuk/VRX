using System.Diagnostics;

namespace PersonaBot.Voice;

/// <summary>
/// personabot-voice --bargetest: barge-in tried offline, in a simulated room, with the real models. The bot says a
/// long sentence; its echo reaches the "microphone" through a room (a delay the timestamps don't know about, a
/// direct path and fading reflections) with background noise; and in some runs a second voice, the person, talks
/// over it. When it barges in, the bot stops (after what the speaker had already been given, 100 ms), as it does in
/// the helper. Each run says whether it barged in, when, what the person was heard to say, and how much of the bot's
/// echo the canceller took out. The blocks go through exactly the classes the helper uses.
/// </summary>
public static class BargeTest
{
    private const int Block = 480;

    public static int Run(string kokoro, string moonshine, string vad, Action<string> log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(kokoro);
        ArgumentException.ThrowIfNullOrWhiteSpace(moonshine);
        ArgumentException.ThrowIfNullOrWhiteSpace(vad);
        ArgumentNullException.ThrowIfNull(log);
        using var speaker = new Speaker(kokoro, log);
        using var hearer = new Hearer(moonshine, log);
        hearer.Warm();
        var botSays = new List<float>();
        speaker.Say("Right, these are the tyres for your car. The Continental EcoContact six is the one Halfords recommends, and it's good for range. " +
                    "The Premium Contact seven grips better in the wet. Shall I add two of them to the basket for you?", 0, "gb", 1f, botSays.AddRange, CancellationToken.None);
        var personSays = new List<float>();
        // Another voice (a man's, Kokoro speaker 7) for the person.
        speaker.Say("Stop, show me the cheaper ones instead please.", 7, "us", 1f, personSays.AddRange, CancellationToken.None);
        var bot16 = new Resampler(speaker.SampleRate, EchoCanceller.Rate).Process(botSays.ToArray());
        var person16 = new Resampler(speaker.SampleRate, EchoCanceller.Rate).Process(personSays.ToArray());
        Console.WriteLine($"Bot: {bot16.Length / (double)EchoCanceller.Rate:0.0}s; person: {person16.Length / (double)EchoCanceller.Rate:0.0}s");

        var results = new List<(string Name, bool? Pass)>();
        // expect null: a known limit, reported but not passed or failed.
        void Try(string name, float[]? person, double personAt, float echoGain, double hiddenDelay, bool? expect, bool warmFirst = false)
        {
            Console.WriteLine();
            Console.WriteLine($"== {name}");
            using var silero = new SileroDetector(vad, log);
            var detector = new BargeInDetector(new EchoCanceller(), silero);
            if (warmFirst)
            {
                Console.WriteLine("   (a first sentence, not talked over, to learn the room)");
                Simulate(detector, silero, botSays, speaker.SampleRate, bot16, null, 0, echoGain, hiddenDelay, hearer, quiet: true);
            }
            var (barged, at, heard) = Simulate(detector, silero, botSays, speaker.SampleRate, bot16, person, personAt, echoGain, hiddenDelay, hearer, quiet: false);
            if (expect == null)
            {
                results.Add((name + $" — known limit: {(barged ? "barged in" : "not barged in")}", null));
                return;
            }
            var pass = expect == barged;
            // In time (within a second of the person starting, and not before) and the right words.
            if (barged && expect == true) pass &= at >= personAt && at <= personAt + 1.0 && heard.Contains("cheap", StringComparison.OrdinalIgnoreCase);
            results.Add((name, pass));
        }

        // Speakers, the echo 6 dB under what's played, 120 ms more delay than the timestamps say; nobody talks.
        Try("speakers, bot alone", null, 0, 0.5f, 0.120, expect: false);
        // A very loud echo (as loud as what's played, a laptop at full volume): still not a barge-in.
        Try("loud speakers, bot alone", null, 0, 1.0f, 0.060, expect: false);
        Try("loud speakers, second sentence, bot alone", null, 0, 1.0f, 0.060, expect: false, warmFirst: true);
        // The person talks over it.
        Try("speakers, person at 3s", person16, 3.0, 0.5f, 0.120, expect: true);
        // The very first sentence, before the canceller is trusted (2 s of the bot alone): the person must be clearly
        // louder than the echo. Quieter than that, it's a known limit (they can press the microphone, or it's caught
        // from the second sentence on).
        Try("speakers, first sentence, person louder than the echo at 1.2s", person16, 1.2, 0.2f, 0.120, expect: true);
        Try("speakers, first sentence, person as loud as the echo at 1.2s", person16, 1.2, 0.5f, 0.120, expect: null);
        Try("speakers, second sentence, person at 0.8s", person16, 0.8, 0.5f, 0.120, expect: true, warmFirst: true);
        Try("loud speakers, second sentence, person at 2s", person16, 2.0, 1.0f, 0.060, expect: true, warmFirst: true);
        // Headphones: no echo at all.
        Try("headphones, person at 2s", person16, 2.0, 0f, 0, expect: true);
        Try("headset, quiet person at 2s", Array.ConvertAll(person16, sample => sample * .23f), 2.0, 0f, 0, expect: true);
        using (var silero = new SileroDetector(vad, log))
        {
            var listening = new Endpointer(1.3f, 45f, 45f);
            var captured = new List<float>();
            var noise = new Random(29);
            var signal = new float[11 * EchoCanceller.Rate + person16.Length];
            for (int i = 0; i < signal.Length; ++i)
            {
                int source = i - 9 * EchoCanceller.Rate;
                signal[i] = (float)((noise.NextDouble() - .5) * .0015) +
                    (source >= 0 && source < person16.Length ? person16[source] * .12f : 0);
            }
            for (int i = 0; i + Block <= signal.Length && !listening.Done; i += Block)
            {
                var block = signal.AsSpan(i, Block).ToArray();
                captured.AddRange(block);
                listening.Add(block, EchoCanceller.Rate, silero.IsSpeech(block));
            }
            string heard = hearer.Transcribe(
                SpeechLevels.PrepareUtterance(captured.ToArray(), EchoCanceller.Rate, listening.SpeechStartedAt), EchoCanceller.Rate);
            Console.WriteLine($"Quiet live phrase: {listening.Reason}, heard \"{heard}\"");
            results.Add(("live mic recognizes a quiet phrase after nine seconds of silence",
                listening.HeardSpeech && listening.Reason == "pause" && heard.Contains("cheap", StringComparison.OrdinalIgnoreCase)));
        }
        Console.WriteLine();
        foreach (var (name, pass) in results) Console.WriteLine($"{(pass == null ? "INFO" : pass.Value ? "PASS" : "FAIL")}  {name}");
        return results.All(r => r.Pass != false) ? 0 : 1;
    }

    private static (bool Barged, double At, string Heard) Simulate(BargeInDetector detector, ISpeechDetector speech, List<float> bot, int botRate, float[] bot16, float[]? person, double personAt,
        float echoGain, double hiddenDelay, Hearer hearer, bool quiet)
    {
        detector.BeginUtterance();
        detector.FrameTrace = Environment.GetEnvironmentVariable("PB_BARGE_TRACE") == "2" && !quiet ? line => Console.WriteLine("     " + line) : null;
        var resampler = new Resampler(botRate, EchoCanceller.Rate);
        var room = Room(new Random(3));
        var random = new Random(11);
        var length = bot16.Length + EchoCanceller.Rate;
        var shift = (int)(hiddenDelay * EchoCanceller.Rate);
        var stopAt = int.MaxValue;
        // Played in blocks of Kokoro's rate, stamped as the device would, 200 ms ahead; recorded in blocks of 480 at 16 kHz.
        var start = 1000.0;
        var botBlock = Block * botRate / EchoCanceller.Rate;
        var played = 0;
        var watch = Stopwatch.StartNew();
        double micPower = 0, residualPower = 0;
        double? bargedAt = null;
        var heard = new List<float>();
        var endpointer = new Endpointer(1.3f, 45f);
        var mic = new float[Block];
        for (var at = 0; at + Block <= length; at += Block)
        {
            var now = start + at / (double)EchoCanceller.Rate;
            while (played < bot.Count && start + played / (double)botRate < now + 0.2)
            {
                var count = Math.Min(botBlock, bot.Count - played);
                var stopped = (long)played * EchoCanceller.Rate / botRate >= stopAt;
                var chunk = stopped ? new float[count] : bot.GetRange(played, count).ToArray();
                detector.AddReference(resampler.Process(chunk), start + played / (double)botRate);
                played += count;
            }
            // The microphone: the bot's echo through the room (only what was played), the person, and noise.
            for (var j = 0; j < Block; j++)
            {
                var n = at + j;
                double echo = 0;
                for (var k = 0; k < room.Length; k++)
                {
                    var i = n - shift - k;
                    if (i >= 0 && i < bot16.Length && i < stopAt) echo += room[k] * bot16[i];
                }
                var voice = 0f;
                if (person != null)
                {
                    var i = n - (int)(personAt * EchoCanceller.Rate);
                    if (i >= 0 && i < person.Length) voice = person[i] * 0.6f;
                }
                mic[j] = (float)(echoGain * echo) + voice + (float)((random.NextDouble() - 0.5) * 0.002);
            }
            var residual = detector.Process(mic, now);
            if (Environment.GetEnvironmentVariable("PB_BARGE_TRACE") == "1" && !quiet && at < 2 * EchoCanceller.Rate)
            {
                double pm = 0, pr = 0;
                foreach (var v in mic) pm += v * v;
                foreach (var v in residual) pr += v * v;
                Console.WriteLine($"     {at / 16000.0:0.00}s mic {10 * Math.Log10(pm / Block + 1e-12):0} dB residual {10 * Math.Log10(pr / Block + 1e-12):0} dB delay {detector.Echo.Delay} aligned {detector.Echo.Aligned} conf {detector.Echo.LastConfidence:0.0}");
            }
            if (bargedAt == null)
            {
                if (at > length - 2 * EchoCanceller.Rate && person == null)
                {
                    foreach (var s in mic) micPower += s * s;
                    foreach (var s in residual) residualPower += s * s;
                }
                if (!detector.BargedIn) continue;
                bargedAt = at / (double)EchoCanceller.Rate;
                // The helper stops the speech: what the speaker had been given (100 ms) still plays.
                stopAt = at + Block + EchoCanceller.Rate / 10;
                if (!quiet) Console.WriteLine($"   barged in at {bargedAt:0.00}s: {detector.Reason}");
                heard.AddRange(detector.TakePreroll());
                endpointer.MarkSpeech();
                continue;
            }
            heard.AddRange(residual);
            endpointer.Add(residual, EchoCanceller.Rate, speech.IsSpeech(residual));
            if (endpointer.Done) break;
        }
        if (!quiet)
        {
            Console.WriteLine($"   {watch.ElapsedMilliseconds} ms for {length / (double)EchoCanceller.Rate:0.0}s of audio; delay {detector.Echo.Delay / 16.0:0} ms " +
                              $"({(detector.Echo.Aligned ? "found" : "not found")}, true {hiddenDelay * 1000:0} ms, last confidence {detector.Echo.LastConfidence:0.0}); " +
                              $"echo share {10 * Math.Log10(detector.Coupling):0} dB; settled {detector.Settled}; {detector.PersonFrames} of {detector.Frames} frames sounded like the person");
            if (residualPower > 0) Console.WriteLine($"   echo taken out over the last 2s: {10 * Math.Log10(micPower / residualPower):0.0} dB");
        }
        if (bargedAt is not { } t) return (false, 0, "");
        var text = hearer.Transcribe(SpeechLevels.ForRecognition(heard.ToArray()), EchoCanceller.Rate);
        if (!quiet) Console.WriteLine($"   heard: \"{text}\"");
        return (true, t, text);
    }

    /// <summary>A small room: the direct path, then reflections fading over 40 ms.</summary>
    private static double[] Room(Random random)
    {
        var room = new double[640];
        room[0] = 1.0;
        for (var k = 40; k < room.Length; k++) room[k] = (random.NextDouble() - 0.5) * 0.5 * Math.Exp(-k / 160.0);
        return room;
    }
}
