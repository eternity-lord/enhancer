using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Enhancer.Desktop.Retouch;

/// <summary>
/// Offline local skin retouch processor. It never reconstructs a face and never changes pixels
/// outside the painted mask. A small neighborhood is used to reduce blemish-scale variation while
/// retaining a configurable amount of texture, which keeps ordinary portraits suitable for daily use.
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
        int radius = strength >= 0.72 ? 3 : 2;
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
                int samples = 0;
                double sumR = 0;
                double sumG = 0;
                double sumB = 0;
                for (int oy = -radius; oy <= radius; oy++)
                {
                    int sy = Math.Clamp(y + oy, 0, height - 1);
                    for (int ox = -radius; ox <= radius; ox++)
                    {
                        int sx = Math.Clamp(x + ox, 0, width - 1);
                        int sampleIndex = sy * stride + sx * 4;
                        sumB += input[sampleIndex] / 255.0;
                        sumG += input[sampleIndex + 1] / 255.0;
                        sumR += input[sampleIndex + 2] / 255.0;
                        samples++;
                    }
                }
                if (samples > 0)
                {
                    averageR = sumR / samples;
                    averageG = sumG / samples;
                    averageB = sumB / samples;
                }

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
                output[index + 3] = 255;

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
        const int width = 72;
        const int height = 56;
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

        Parallel.For(0, height, new ParallelOptions
        {
            CancellationToken = cancellationToken,
            MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount - 1)
        }, y =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            double normalizedY = y / (double)Math.Max(1, height - 1);
            for (int x = 0; x < width; x++)
            {
                double normalizedX = x / (double)Math.Max(1, width - 1);
                double value = 0;
                foreach (SkinRetouchStroke stroke in strokes)
                {
                    double radius = Math.Clamp(stroke.Radius, 0.002, 1.0);
                    double dx = normalizedX - Math.Clamp(stroke.X, 0, 1);
                    double dy = normalizedY - Math.Clamp(stroke.Y, 0, 1);
                    double distance = Math.Sqrt(dx * dx + dy * dy);
                    if (distance >= radius) continue;
                    double falloff = 1.0 - distance / radius;
                    double contribution = falloff * falloff * Math.Clamp(stroke.Opacity, 0, 1);
                    value = stroke.Erase
                        ? Math.Max(0, value - contribution)
                        : Math.Min(1, value + contribution);
                }
                mask[y * width + x] = value;
            }
        });
        return mask;
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
