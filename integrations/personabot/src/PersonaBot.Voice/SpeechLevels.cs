namespace PersonaBot.Voice;

internal static class SpeechLevels
{
    internal static float[] PrepareUtterance(float[] samples, int sampleRate, float speechStartedAt)
    {
        int first = Math.Min(samples.Length, (int)(Math.Max(0, speechStartedAt - .60f) * sampleRate));
        return ForRecognition(first == 0 ? samples : samples[first..]);
    }

    // Keep echo estimation in the original signal domain. Only the speech
    // detector and recognizer get a level-adjusted copy of the samples.
    internal static float[] ForRecognition(float[] samples, float targetRms = .075f)
    {
        if (samples.Length == 0) return samples;
        double power = 0;
        float peak = 0;
        foreach (float sample in samples)
        {
            power += sample * sample;
            peak = Math.Max(peak, Math.Abs(sample));
        }
        float rms = (float)Math.Sqrt(power / samples.Length);
        if (rms < .0005f) return samples;
        float gain = Math.Min(10f, Math.Min(targetRms / rms, .95f / peak));
        if (gain <= 1.05f) return samples;
        return Array.ConvertAll(samples, sample => sample * gain);
    }
}
