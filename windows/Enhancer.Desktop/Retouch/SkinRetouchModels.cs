using System.Windows.Media.Imaging;

namespace Enhancer.Desktop.Retouch;

/// <summary>
/// A normalized brush stroke for the dedicated Skin Retouch workspace. Coordinates stay
/// resolution-independent, so the same non-destructive edit can be saved or re-opened at any zoom.
/// </summary>
public sealed class SkinRetouchStroke
{
    public double X { get; set; }
    public double Y { get; set; }
    public double Radius { get; set; } = 0.08;
    public double Opacity { get; set; } = 0.95;
    public bool Erase { get; set; }

    public SkinRetouchStroke Clone() => new()
    {
        X = X,
        Y = Y,
        Radius = Radius,
        Opacity = Opacity,
        Erase = Erase
    };
}

/// <summary>
/// Settings for simple, local, deterministic skin retouch. It is deliberately separate from
/// LabAdjustments so color grading and skin retouch cannot silently share an execution path.
/// </summary>
public sealed class SkinRetouchSettings
{
    public double Strength { get; set; } = 0.65;
    public double TexturePreservation { get; set; } = 0.42;
    public bool SkinToneProtection { get; set; } = true;
    public List<SkinRetouchStroke> Strokes { get; set; } = new();

    public SkinRetouchSettings Clone() => new()
    {
        Strength = Strength,
        TexturePreservation = TexturePreservation,
        SkinToneProtection = SkinToneProtection,
        Strokes = Strokes?.Select(stroke => stroke.Clone()).ToList() ?? new()
    };
}

public sealed record SkinRetouchResult(
    BitmapSource Bitmap,
    int MaskedPixels,
    int ChangedPixels,
    int MaxChannelDelta,
    double MeanChannelDelta
)
{
    public bool HasVisibleChange => ChangedPixels > 0 && MaxChannelDelta >= 2;
}
