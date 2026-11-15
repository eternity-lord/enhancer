using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Enhancer.Desktop.Retouch;

/// <summary>
    /// Offline local skin retouch processor. It never reconstructs a face and never changes pixels
    /// outside the painted mask. A resolution-aware local neighborhood reduces blemish-scale
    /// variation while retaining a configurable amount of texture, which keeps ordinary portraits
    /// suitable for daily use.
/// </summary>
public static class SkinRetouchProcessor
{
    public static SkinRetouchResult Apply(
        BitmapSource source,
        SkinRetouchSettings settings,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(settings);

        var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
        int width = converted.PixelWidth;
        int height = converted.PixelHeight;
        int stride = checked(width * 4);
        byte[] input = new byte[checked(stride * height)];
        converted.CopyPixels(input, stride, 0);
        byte[] output = (byte[])input.Clone();

        IReadOnlyList<SkinRetouchStroke> strokes = settings.Strokes ?? new List<SkinRetouchStroke>();
        double[] mask = BuildMask(width, height, strokes, cancellationToken);
        int maskedPixels = mask.Count(value => value > 0.01);
        if (maskedPixels == 0)
        {
            return new SkinRetouchResult(CreateBitmap(width, height, input, stride), 0, 0, 0, 0);
        }

        double strength = Math.Clamp(settings.Strength, 0, 1);
        double texture = Math.Clamp(settings.TexturePreservation, 0, 1);

        // The old implementation used a fixed 2–3 pixel neighborhood. That passed the tiny
        // smoke fixture but is effectively invisible on a 2K/4K portrait: three pixels are only
        // a fraction of a pore. Scale the sampling radius with the actual source resolution and
        // keep it capped so CPU fallback stays predictable.
        int radius = ComputeNeighborhoodRadius(width, height, strength);
        int[] sampleOffsets = BuildSampleOffsets(radius);
        int changedPixels = 0;
        int maxChannelDelta = 0;
        long totalChannelDelta = 0;

        Parallel.For(0, height, new ParallelOptions
        {
            CancellationToken = cancellationToken,
            MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount - 1)
        }, y =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            for (int x = 0; x < width; x++)
            {
                int pixel = y * width + x;
                double scope = mask[pixel];
                if (scope <= 0.001) continue;

                int index = y * stride + x * 4;
                double b = input[index] / 255.0;
                double g = input[index + 1] / 255.0;
                double r = input[index + 2] / 255.0;

                double skinConfidence = SkinConfidence(r, g, b);
                if (settings.SkinToneProtection)
                {
                    // The painted mask remains authoritative. Skin confidence only softens the
                    // effect around hair, clothing and background; it can never turn a selected
                    // area completely off, which was the source of the old "mask but no change" UX.
                    scope *= Math.Max(0.48, skinConfidence);
                }

                double averageR = r;
                double averageG = g;
                double averageB = b;
                double sumR = 0;
                double sumG = 0;
                double sumB = 0;
                foreach (int oy in sampleOffsets)
                {
                    int sy = Math.Clamp(y + oy, 0, height - 1);
                    foreach (int ox in sampleOffsets)
                    {
                        int sx = Math.Clamp(x + ox, 0, width - 1);
                        int sampleIndex = sy * stride + sx * 4;
                        sumB += input[sampleIndex] / 255.0;
                        sumG += input[sampleIndex + 1] / 255.0;
                        sumR += input[sampleIndex + 2] / 255.0;
                    }
                }
                int sampleCount = sampleOffsets.Length * sampleOffsets.Length;
                averageR = sumR / sampleCount;
                averageG = sumG / sampleCount;
                averageB = sumB / sampleCount;

                // Keep part of the high-frequency detail so pores and expression stay natural.
                double retainedDetail = 0.30 + texture * 0.58;
                double targetR = averageR + (r - averageR) * retainedDetail;
                double targetG = averageG + (g - averageG) * retainedDetail;
                double targetB = averageB + (b - averageB) * retainedDetail;
                double blend = Math.Clamp(
                    scope * (0.22 + strength * 0.66) * (1.0 - texture * 0.16),
                    0.0,
                    0.86
                );

                // A restrained local tone-evening pass reduces small blotches without lifting the
                // whole face. It is also useful on low-detail photographs where blur alone is subtle.
                double luma = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
                double averageLuma = (0.2126 * averageR) + (0.7152 * averageG) + (0.0722 * averageB);
                double lift = (averageLuma - luma) * scope * strength * 0.08;
                targetR += lift;
                targetG += lift;
                targetB += lift;

                double outR = Lerp(r, targetR, blend);
                double outG = Lerp(g, targetG, blend);
                double outB = Lerp(b, targetB, blend);
                byte outputB = ToByte(outB);
                byte outputG = ToByte(outG);
                byte outputR = ToByte(outR);
                output[index] = outputB;
                output[index + 1] = outputG;
                output[index + 2] = outputR;
                output[index + 3] = input[index + 3];

                int delta = Math.Abs(outputB - input[index]) +
                            Math.Abs(outputG - input[index + 1]) +
                            Math.Abs(outputR - input[index + 2]);
                if (delta > 0)
                {
                    Interlocked.Increment(ref changedPixels);
                    Interlocked.Add(ref totalChannelDelta, delta);
                    int currentMax;
                    do
                    {
                        currentMax = maxChannelDelta;
                        if (delta <= currentMax) break;
                    }
                    while (Interlocked.CompareExchange(ref maxChannelDelta, delta, currentMax) != currentMax);
                }
            }
        });

        double meanDelta = changedPixels == 0 ? 0 : totalChannelDelta / (double)changedPixels;
        return new SkinRetouchResult(
            CreateBitmap(width, height, output, stride),
            maskedPixels,
            changedPixels,
            maxChannelDelta,
            meanDelta
        );
    }

    /// <summary>
    /// A small deterministic regression check used by the Windows WPF smoke test. It catches the
    /// exact failure mode reported by the user: an orange/painted mask with zero changed pixels.
    /// </summary>
    public static bool RunSelfTest(out SkinRetouchResult result)
    {
        // Use a near-HD fixture rather than a tiny thumbnail. A fixed-pixel blur can pass a
        // thumbnail test while remaining invisible on the real high-resolution photos users edit.
        const int width = 1024;
        const int height = 768;
        int stride = width * 4;
        byte[] pixels = new byte[stride * height];
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int index = y * stride + x * 4;
                double dx = (x - width * 0.5) / (width * 0.5);
                double dy = (y - height * 0.5) / (height * 0.5);
                bool facePatch = dx * dx + dy * dy < 0.48;
                byte r = facePatch ? (byte)Math.Clamp(178 + ((x * 7 + y * 3) % 19), 0, 255) : (byte)48;
                byte g = facePatch ? (byte)Math.Clamp(118 + ((x * 5 + y * 2) % 17), 0, 255) : (byte)58;
                byte b = facePatch ? (byte)Math.Clamp(92 + ((x * 3 + y * 5) % 13), 0, 255) : (byte)76;
                pixels[index] = b;
                pixels[index + 1] = g;
                pixels[index + 2] = r;
                pixels[index + 3] = 255;
            }
        }

        var source = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgra32, null, pixels, stride);
        source.Freeze();
        var settings = new SkinRetouchSettings
        {
            Strength = 0.82,
            TexturePreservation = 0.35,
            SkinToneProtection = true,
            Strokes =
            [
                new SkinRetouchStroke { X = 0.5, Y = 0.5, Radius = 0.32, Opacity = 1.0 }
            ]
        };
        result = Apply(source, settings);
        return result.MaskedPixels > 0 && result.ChangedPixels > 0 && result.MaxChannelDelta >= 2;
    }

    private static double[] BuildMask(
        int width,
        int height,
        IReadOnlyList<SkinRetouchStroke> strokes,
        CancellationToken cancellationToken)
    {
        double[] mask = new double[checked(width * height)];
        if (strokes.Count == 0) return mask;

        // Rasterize each brush only inside its own bounds. The previous implementation scanned
        // every source pixel for every stroke, so a normal drag on a high-resolution image could
        // spend most of the render in mask construction and repeatedly cancel itself.
        int maxCoordinateX = Math.Max(1, width - 1);
        int maxCoordinateY = Math.Max(1, height - 1);
        foreach (SkinRetouchStroke stroke in strokes)
        {
            cancellationToken.ThrowIfCancellationRequested();
            double radius = Math.Clamp(stroke.Radius, 0.002, 1.0);
            double centerX = Math.Clamp(stroke.X, 0, 1);
            double centerY = Math.Clamp(stroke.Y, 0, 1);
            int minX = Math.Max(0, (int)Math.Floor((centerX - radius) * maxCoordinateX));
            int maxX = Math.Min(width - 1, (int)Math.Ceiling((centerX + radius) * maxCoordinateX));
            int minY = Math.Max(0, (int)Math.Floor((centerY - radius) * maxCoordinateY));
            int maxY = Math.Min(height - 1, (int)Math.Ceiling((centerY + radius) * maxCoordinateY));
            double opacity = Math.Clamp(stroke.Opacity, 0, 1);

            for (int y = minY; y <= maxY; y++)
            {
                if ((y & 31) == 0) cancellationToken.ThrowIfCancellationRequested();
                double normalizedY = y / (double)maxCoordinateY;
                double dy = normalizedY - centerY;
                for (int x = minX; x <= maxX; x++)
                {
                    double normalizedX = x / (double)maxCoordinateX;
                    double dx = normalizedX - centerX;
                    double distance = Math.Sqrt(dx * dx + dy * dy);
                    if (distance >= radius) continue;
                    double falloff = 1.0 - distance / radius;
                    double contribution = falloff * falloff * opacity;
                    int index = y * width + x;
                    mask[index] = stroke.Erase
                        ? Math.Max(0, mask[index] - contribution)
                        : Math.Min(1, mask[index] + contribution);
                }
            }
        }
        return mask;
    }

    private static int ComputeNeighborhoodRadius(int width, int height, double strength)
    {
        int shortestSide = Math.Min(width, height);
        int resolutionRadius = (int)Math.Round(shortestSide * 0.0035, MidpointRounding.AwayFromZero);
        int strengthRadius = strength >= 0.72 ? 1 : 0;
        return Math.Clamp(Math.Max(2, resolutionRadius) + strengthRadius, 2, 12);
    }

    private static int[] BuildSampleOffsets(int radius)
    {
        int half = Math.Max(1, radius / 2);
        return [-radius, -half, 0, half, radius];
    }

    private static double SkinConfidence(double r, double g, double b)
    {
        double max = Math.Max(r, Math.Max(g, b));
        double min = Math.Min(r, Math.Min(g, b));
        double saturation = max <= 0.0001 ? 0 : (max - min) / max;
        bool warm = r > g * 0.80 && g > b * 0.72 && r > b * 1.08;
        bool plausible = r > 0.10 && g > 0.06 && b > 0.03 && saturation < 0.90;
        if (!warm || !plausible) return 0;
        double hueBias = Math.Clamp((r - b) / 0.62, 0, 1);
        double saturationFit = 1.0 - Math.Abs(saturation - 0.38) / 0.68;
        return Math.Clamp(hueBias * saturationFit, 0, 1);
    }

    private static BitmapSource CreateBitmap(int width, int height, byte[] pixels, int stride)
    {
        var bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgra32, null);
        bitmap.WritePixels(new Int32Rect(0, 0, width, height), pixels, stride, 0);
        bitmap.Freeze();
        return bitmap;
    }

    private static double Lerp(double a, double b, double t) => a + (b - a) * Math.Clamp(t, 0, 1);
    private static byte ToByte(double value) => (byte)Math.Round(Math.Clamp(value, 0, 1) * 255.0);
}
