namespace VRX.Desktop;

public enum StereoState { Waiting, Active, Flat, Disabled }
public enum DepthPipelineState { Unknown, Gpu, CpuFallback }

// Stable engine status lines, separate from the append-only diagnostic log. A settings
// checkbox records intent; only these engine reports describe what is actually running.
public sealed class StereoHealth
{
    public StereoState Stereo { get; private set; } = StereoState.Waiting;
    public DepthPipelineState Pipeline { get; private set; }
    public string StereoDetails { get; private set; } = "";
    public string PipelineDetails { get; private set; } = "";
    public bool FeaturesUnavailable { get; private set; }
    public string FeatureDetails { get; private set; } = "";
    public bool StereoEffectsUnavailable { get; private set; }
    public string StereoEffectDetails { get; private set; } = "";

    public void Reset()
    {
        Stereo = StereoState.Waiting;
        Pipeline = DepthPipelineState.Unknown;
        StereoDetails = PipelineDetails = FeatureDetails = StereoEffectDetails = "";
        FeaturesUnavailable = StereoEffectsUnavailable = false;
    }

    public bool Consume(string? line)
    {
        if (Payload(line, "Stereo status: ") is { } stereo)
        {
            var (state, detail) = Split(stereo);
            StereoState? next = state switch
            {
                "active" => StereoState.Active, "flat" => StereoState.Flat,
                "disabled" => StereoState.Disabled, "waiting" => StereoState.Waiting, _ => null
            };
            if (next == null) return false;
            Stereo = next.Value;
            StereoDetails = detail;
            return true;
        }
        if (Payload(line, "Depth pipeline: ") is { } pipeline)
        {
            var (state, detail) = Split(pipeline);
            DepthPipelineState? next = state switch
            {
                "GPU" => DepthPipelineState.Gpu, "CPU fallback" => DepthPipelineState.CpuFallback, _ => null
            };
            if (next == null) return false;
            Pipeline = next.Value;
            PipelineDetails = detail;
            return true;
        }
        if (Payload(line, "Depth features: ") is { } features)
        {
            var (state, detail) = Split(features);
            if (state is not ("available" or "unavailable")) return false;
            FeaturesUnavailable = state == "unavailable";
            FeatureDetails = FeaturesUnavailable ? detail : "";
            return true;
        }
        if (Payload(line, "Stereo effects: ") is { } effects)
        {
            var (state, detail) = Split(effects);
            if (state is not ("available" or "unavailable")) return false;
            StereoEffectsUnavailable = state == "unavailable";
            StereoEffectDetails = StereoEffectsUnavailable ? detail : "";
            return true;
        }
        // Also recognize these precise older messages: a session must never advertise
        // working motion steadying/fusion solely because their checkboxes are selected.
        if (Payload(line, "EnsureMotion: ") == "no hardware motion estimation on this GPU; steady depth and model fusion have no effect")
        {
            FeaturesUnavailable = true;
            FeatureDetails = "";
            return true;
        }
        if (Payload(line, "AnchorMain: ") is "FAIL queue/DirectML; model fusion off" or "3 failures; model fusion off")
        {
            FeaturesUnavailable = true;
            FeatureDetails = Loc.Get("DepthFusionUnavailable");
            return true;
        }
        return false;
    }

    private static string? Payload(string? line, string prefix)
    {
        if (string.IsNullOrEmpty(line)) return null;
        if (line.StartsWith(prefix, StringComparison.Ordinal)) return line[prefix.Length..].Trim();
        // Native logs start with [time][thread]. Do not mistake a quoted status in an
        // unrelated message for an actual status update.
        int at = line.IndexOf("] " + prefix, StringComparison.Ordinal);
        return at >= 0 ? line[(at + 2 + prefix.Length)..].Trim() : null;
    }

    private static (string State, string Detail) Split(string payload)
    {
        int separator = payload.IndexOf(';');
        return separator < 0 ? (payload.Trim(), "") : (payload[..separator].Trim(), payload[(separator + 1)..].Trim());
    }
}
