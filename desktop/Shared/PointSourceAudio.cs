namespace VRX.Audio;

// Head-relative delay, level and head-shadow cues for a moving headphone source.
internal sealed class PointSourceAudio
{
    private readonly float[] history = new float[128];
    private int cursor;
    private float pan, rear, shadowLeft, shadowRight, delay, smoothing;
    private float targetPan, targetRear;

    internal void SetTarget(int sampleRate, float lateral, float behind)
    {
        targetPan = float.IsFinite(lateral) ? Math.Clamp(lateral, -1f, 1f) : 0;
        targetRear = float.IsFinite(behind) ? Math.Clamp(behind, 0f, 1f) : 0;
        delay = MathF.Min(126, .00065f * sampleRate);
        smoothing = 1 - MathF.Exp(-1f / (.025f * sampleRate));
    }

    internal void Render(float mono, out float left, out float right)
    {
        pan += (targetPan - pan) * smoothing;
        rear += (targetRear - rear) * smoothing;
        history[cursor] = mono;
        float farLeft = MathF.Max(0, pan), farRight = MathF.Max(0, -pan);
        float l = Delayed(farLeft * delay), r = Delayed(farRight * delay);
        cursor = (cursor + 1) & 127;
        shadowLeft += (l - shadowLeft) * (1 - .75f * MathF.Max(farLeft, rear * .65f));
        shadowRight += (r - shadowRight) * (1 - .75f * MathF.Max(farRight, rear * .65f));
        left = shadowLeft * MathF.Sqrt(MathF.Max(0, .5f * (1 - pan)));
        right = shadowRight * MathF.Sqrt(MathF.Max(0, .5f * (1 + pan)));
    }

    private float Delayed(float samples)
    {
        int whole = (int)samples;
        float first = history[(cursor - whole) & 127];
        return first + (history[(cursor - whole - 1) & 127] - first) * (samples - whole);
    }
}
