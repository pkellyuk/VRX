using System.Text.Json.Serialization;

namespace Automaton.Tools.Models;

/// <summary>
/// Parameters for the computer-use tools.
///
/// Nothing here takes a coordinate. Every position is a grid label from the current
/// view, which is the whole point of the design: the model compares what it sees
/// against labelled markers instead of estimating pixels it cannot measure.
/// </summary>
public class ComputerUseBeginParameters
{
    [JsonPropertyName("reason")]
    public string? Reason { get; set; }

    /// <summary>Optional desktop selector accepted for compatibility with existing calls.</summary>
    [JsonPropertyName("machine")]
    public string? Machine { get; set; }
}

public class ComputerUseEndParameters
{
}

public class ComputerUseScreensParameters
{
}

public class ComputerUseGridParameters
{
    /// <summary>
    /// What to look at: "screen", "monitor:N", or two labels of the current view like
    /// "B2:C4". Ignored when <see cref="Around"/> is given.
    /// </summary>
    [JsonPropertyName("region")]
    public string? Region { get; set; }

    /// <summary>
    /// A label of the current view to zoom into. Crops one cell centred on it, so the
    /// magnification per step equals the column count.
    /// </summary>
    [JsonPropertyName("around")]
    public string? Around { get; set; }

    [JsonPropertyName("rows")]
    public int? Rows { get; set; }

    [JsonPropertyName("columns")]
    public int? Columns { get; set; }
}

public class ComputerUseZoomOutParameters
{
}

public class ComputerUseScreenshotParameters
{
    [JsonPropertyName("region")]
    public string? Region { get; set; }

    /// <summary>
    /// Marks where the pointer actually is, for checking an aim before committing
    /// to it. Off unless asked for.
    /// </summary>
    [JsonPropertyName("showPointer")]
    public bool ShowPointer { get; set; }
}

public class ComputerUseClickParameters
{
    [JsonPropertyName("label")]
    public string? Label { get; set; }

    [JsonPropertyName("button")]
    public string? Button { get; set; }

    [JsonPropertyName("count")]
    public int? Count { get; set; }

    /// <summary>Text to find and click in one call, instead of a label.</summary>
    [JsonPropertyName("text")]
    public string? Text { get; set; }

    /// <summary>Where to look for the text; the whole screen if omitted.</summary>
    [JsonPropertyName("region")]
    public string? Region { get; set; }
}

public class ComputerUseMoveParameters
{
    [JsonPropertyName("label")]
    public string? Label { get; set; }
}

public class ComputerUseDragParameters
{
    [JsonPropertyName("from")]
    public string? From { get; set; }

    [JsonPropertyName("to")]
    public string? To { get; set; }

    [JsonPropertyName("button")]
    public string? Button { get; set; }
}

public class ComputerUseScrollParameters
{
    /// <summary>Where to scroll. Defaults to the centre of the current view.</summary>
    [JsonPropertyName("label")]
    public string? Label { get; set; }

    [JsonPropertyName("direction")]
    public string? Direction { get; set; }

    /// <summary>Wheel notches. One notch is what a physical wheel click sends.</summary>
    [JsonPropertyName("amount")]
    public int? Amount { get; set; }
}

public class ComputerUseTypeParameters
{
    [JsonPropertyName("text")]
    public string? Text { get; set; }
}

public class ComputerUseKeyParameters
{
    [JsonPropertyName("keys")]
    public string? Keys { get; set; }
}

public class ComputerUseFindTextParameters
{
    /// <summary>The text to find, as it appears on screen.</summary>
    [JsonPropertyName("text")]
    public string? Text { get; set; }

    /// <summary>Where to look. Defaults to the whole screen.</summary>
    [JsonPropertyName("region")]
    public string? Region { get; set; }

    /// <summary>Also return an image with the matches boxed and labelled.</summary>
    [JsonPropertyName("show")]
    public bool Show { get; set; }
}

public class ComputerUseFocusWindowParameters
{
    /// <summary>A window's title or part of it, or its process name.</summary>
    [JsonPropertyName("title")]
    public string? Title { get; set; }
}

public class ComputerUsePeekParameters
{
    /// <summary>The grid label or text match to look at.</summary>
    [JsonPropertyName("label")]
    public string? Label { get; set; }

    /// <summary>Width and height of the square shown, in screen pixels.</summary>
    [JsonPropertyName("size")]
    public int? Size { get; set; }
}

public class ComputerUseClickAtParameters
{
    /// <summary>"0.42,0.71" as fractions of the last image, or "1620,1530" screen pixels.</summary>
    [JsonPropertyName("at")]
    public string? At { get; set; }

    /// <summary>What the fractions are of; the last image the model was shown if omitted.</summary>
    [JsonPropertyName("region")]
    public string? Region { get; set; }

    /// <summary>Look at the point instead of clicking it.</summary>
    [JsonPropertyName("verify")]
    public bool Verify { get; set; }

    [JsonPropertyName("button")]
    public string? Button { get; set; }

    [JsonPropertyName("count")]
    public int? Count { get; set; }
}
