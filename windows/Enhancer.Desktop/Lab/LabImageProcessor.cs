using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Enhancer.Desktop.Lab;

public static class LabImageProcessor
{
    public static BitmapSource Apply(BitmapSource source, LabAdjustments adjustments, CancellationToken cancellationToken = default)
    {
        var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
        int width = converted.PixelWidth;
        int height = converted.PixelHeight;
        int stride = checked(width * 4);
        byte[] input = new byte[checked(stride * height)];
        converted.CopyPixels(input, stride, 0);
        byte[] output = new byte[input.Length];

        double exposureMul = Math.Pow(2.0, Math.Clamp(adjustments.Exposure, -3.0, 3.0));
        double contrast = Math.Clamp(adjustments.Contrast, -1.0, 1.0);
        double contrastFactor = (259.0 * (contrast * 255.0 + 255.0)) /
                                (255.0 * (259.0 - contrast * 255.0));
        double black = Math.Clamp(adjustments.BlackPoint, -1.0, 1.0) * 0.20;
        double white = 1.0 - Math.Clamp(adjustments.WhitePoint, -1.0, 1.0) * 0.20;
        double temperature = Math.Clamp(adjustments.Temperature, -1.0, 1.0);
        double tint = Math.Clamp(adjustments.Tint, -1.0, 1.0);
        double saturation = Math.Clamp(adjustments.Saturation, -1.0, 1.0);
        double vibrance = Math.Clamp(adjustments.Vibrance, -1.0, 1.0);
        double highlights = Math.Clamp(adjustments.Highlights, -1.0, 1.0);
        double shadows = Math.Clamp(adjustments.Shadows, -1.0, 1.0);
        double sharpening = Math.Clamp(adjustments.Sharpening, 0.0, 1.0);
        double vignette = Math.Clamp(adjustments.Vignette, 0.0, 1.0);
        double grain = Math.Clamp(adjustments.Grain, 0.0, 1.0);
        double fade = Math.Clamp(adjustments.Fade, 0.0, 1.0);
        double curveBlack = Math.Clamp(adjustments.CurveBlack, -1.0, 1.0);
        double curveQuarter = Math.Clamp(adjustments.CurveQuarter, -1.0, 1.0);
        double curveMid = Math.Clamp(adjustments.CurveMid, -1.0, 1.0);
        double curveThreeQuarter = Math.Clamp(adjustments.CurveThreeQuarter, -1.0, 1.0);
        double curveWhite = Math.Clamp(adjustments.CurveWhite, -1.0, 1.0);
        double shadowHue = Math.Clamp(adjustments.ShadowHue, -1.0, 1.0);
        double shadowSaturation = Math.Clamp(adjustments.ShadowSaturation, 0.0, 1.0);
        double highlightHue = Math.Clamp(adjustments.HighlightHue, -1.0, 1.0);
        double highlightSaturation = Math.Clamp(adjustments.HighlightSaturation, 0.0, 1.0);
        IReadOnlyList<LabMaskStroke> maskStrokes = adjustments.MaskStrokes ?? new List<LabMaskStroke>();

        // Lab is color/tone only. Protection strokes are retained for identity-safe clarity and
        // sharpening; all legacy orange retouch strokes are ignored and handled by SkinRetouchWindow.
        bool needsNeighborhood = clarity > 0.001 || sharpening > 0.001;
        const int neighborhoodRadius = 1;

        Parallel.For(0, height, new ParallelOptions
        {
            CancellationToken = cancellationToken,
            MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount - 1)
        }, y =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            for (int x = 0; x < width; x++)
            {
                int index = (y * stride) + (x * 4);
                double b = input[index] / 255.0;
                double g = input[index + 1] / 255.0;
                double r = input[index + 2] / 255.0;
                double normalizedX = x / Math.Max(1.0, width - 1.0);
                double normalizedY = y / Math.Max(1.0, height - 1.0);
                double protection = adjustments.FaceProtection && maskStrokes.Count > 0
                    ? ProtectionMask(maskStrokes, normalizedX, normalizedY)
                    : 0.0;
                double unprotected = 1.0 - protection;

                double avgR = r;
                double avgG = g;
                double avgB = b;
                if (needsNeighborhood)
                {
                    int samples = 0;
                    double sumR = 0, sumG = 0, sumB = 0;
                    for (int oy = -neighborhoodRadius; oy <= neighborhoodRadius; oy++)
                    {
                        int sy = Math.Clamp(y + oy, 0, height - 1);
                        for (int ox = -neighborhoodRadius; ox <= neighborhoodRadius; ox++)
                        {
                            int sx = Math.Clamp(x + ox, 0, width - 1);
                            int si = (sy * stride) + (sx * 4);
                            sumB += input[si] / 255.0;
                            sumG += input[si + 1] / 255.0;
                            sumR += input[si + 2] / 255.0;
                            samples++;
                        }
                    }
                    avgR = sumR / samples;
                    avgG = sumG / samples;
                    avgB = sumB / samples;
                }

                double luminance = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
                double shadowMask = SmoothStep(0.85, 0.05, luminance);
                double highlightMask = SmoothStep(0.35, 0.95, luminance);

                r *= exposureMul;
                g *= exposureMul;
                b *= exposureMul;

                r += shadows * shadowMask * 0.20;
                g += shadows * shadowMask * 0.20;
                b += shadows * shadowMask * 0.20;
                r -= highlights * highlightMask * 0.18;
                g -= highlights * highlightMask * 0.18;
                b -= highlights * highlightMask * 0.18;

                r = ((r * 255.0 - 128.0) * contrastFactor + 128.0) / 255.0;
                g = ((g * 255.0 - 128.0) * contrastFactor + 128.0) / 255.0;
                b = ((b * 255.0 - 128.0) * contrastFactor + 128.0) / 255.0;

                r = (r - black) / Math.Max(0.20, white - black);
                g = (g - black) / Math.Max(0.20, white - black);
                b = (b - black) / Math.Max(0.20, white - black);

                // Temperature and tint are small channel-biased shifts, like a photo editor,
                // rather than a destructive hue rotation.
                r += temperature * 0.075;
                g += tint * 0.035 - temperature * 0.012;
                b -= temperature * 0.075;

                RgbToHsv(r, g, b, out double hue, out double hsvSaturation, out double hsvValue);
                hsvSaturation = Math.Clamp(
                    hsvSaturation * (1.0 + saturation) + vibrance * (1.0 - hsvSaturation) * 0.65,
                    0.0,
                    1.0
                );
                HsvToRgb(hue, hsvSaturation, hsvValue, out r, out g, out b);

                // Apply an RGB tone curve and two restrained color-wheel grades. The curve is
                // interpolated between five control points, while wheel color is weighted by
                // luminance so shadows and highlights can be graded independently.
                r = ApplyToneCurve(r, curveBlack, curveQuarter, curveMid, curveThreeQuarter, curveWhite);
                g = ApplyToneCurve(g, curveBlack, curveQuarter, curveMid, curveThreeQuarter, curveWhite);
                b = ApplyToneCurve(b, curveBlack, curveQuarter, curveMid, curveThreeQuarter, curveWhite);
                double gradeLuma = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
                double shadowGrade = 1.0 - SmoothStep(0.12, 0.62, gradeLuma);
                double highlightGrade = SmoothStep(0.42, 0.92, gradeLuma);
                ApplyColorWheel(ref r, ref g, ref b, shadowHue, shadowSaturation, shadowGrade);
                ApplyColorWheel(ref r, ref g, ref b, highlightHue, highlightSaturation, highlightGrade);

                if (needsNeighborhood)
                {
                    double highPassR = r - avgR;
                    double highPassG = g - avgG;
                    double highPassB = b - avgB;
                    double detail = ((clarity * 0.32) + (sharpening * 0.22)) *
                                    (0.15 + unprotected * 0.85);
                    r += highPassR * detail;
                    g += highPassG * detail;
                    b += highPassB * detail;
                }

                if (adjustments.Monochrome)
                {
                    double gray = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
                    r = g = b = gray;
                }

                if (fade > 0.001)
                {
                    double lifted = fade * 0.12;
                    r = r * (1.0 - fade * 0.08) + lifted;
                    g = g * (1.0 - fade * 0.08) + lifted;
                    b = b * (1.0 - fade * 0.08) + lifted;
                }

                if (vignette > 0.001)
                {
                    double nx = (x / Math.Max(1.0, width - 1.0) * 2.0) - 1.0;
                    double ny = (y / Math.Max(1.0, height - 1.0) * 2.0) - 1.0;
                    double edge = Math.Clamp((nx * nx + ny * ny) * 0.5, 0, 1);
                    double factor = 1.0 - edge * vignette * 0.48;
                    r *= factor;
                    g *= factor;
                    b *= factor;
                }

                if (grain > 0.001)
                {
                    // Stable hash grain means re-rendering a slider does not make the preview flicker.
                    uint hash = unchecked((uint)(x * 374761393 + y * 668265263));
                    hash = (hash ^ (hash >> 13)) * 1274126177u;
                    double noise = ((hash & 0xFFFF) / 65535.0 - 0.5) * grain * 0.08;
                    r += noise;
                    g += noise;
                    b += noise;
                }

                output[index] = ToByte(b);
                output[index + 1] = ToByte(g);
                output[index + 2] = ToByte(r);
                output[index + 3] = 255;
            }
        });

        var bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgra32, null);
        bitmap.WritePixels(new Int32Rect(0, 0, width, height), output, stride, 0);
        bitmap.Freeze();
        return bitmap;
    }

    public static string Describe(BitmapSource source)
    {
        var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
        int stride = converted.PixelWidth * 4;
        byte[] pixels = new byte[stride * converted.PixelHeight];
        converted.CopyPixels(pixels, stride, 0);
        long[] bins = new long[8];
        double sum = 0;
        for (int i = 0; i < pixels.Length; i += 4)
        {
            double luma = (0.2126 * pixels[i + 2]) + (0.7152 * pixels[i + 1]) + (0.0722 * pixels[i]);
            sum += luma;
            bins[Math.Clamp((int)(luma / 256.0 * bins.Length), 0, bins.Length - 1)]++;
        }
        double mean = sum / Math.Max(1, converted.PixelWidth * converted.PixelHeight);
        string bars = string.Join(" ", bins.Select(v => new string('▮', (int)Math.Clamp(v * 16 / Math.Max(1, bins.Max()), 1, 8))));
        return $"Histogram  {bars}   Mean Luma: {mean:0.0}   {converted.PixelWidth}×{converted.PixelHeight}px";
    }

    private static double SmoothStep(double edge0, double edge1, double value)
    {
        double t = Math.Clamp((value - edge0) / (edge1 - edge0), 0, 1);
        return t * t * (3 - 2 * t);
    }

    private static double ProtectionMask(IReadOnlyList<LabMaskStroke> strokes, double x, double y)
    {
        double mask = 0;
        foreach (LabMaskStroke stroke in strokes)
        {
            // Retouch strokes from legacy Lab presets are not part of the Lab pipeline.
            if (stroke.Retouch) continue;
            double radius = Math.Clamp(stroke.Radius, 0.002, 1.0);
            double dx = x - Math.Clamp(stroke.X, 0, 1);
            double dy = y - Math.Clamp(stroke.Y, 0, 1);
            double distance = Math.Sqrt((dx * dx) + (dy * dy));
            if (distance >= radius) continue;
            double falloff = 1.0 - (distance / radius);
            double contribution = falloff * falloff * Math.Clamp(stroke.Opacity, 0, 1);
            mask = stroke.Erase
                ? Math.Max(0, mask - contribution)
                : Math.Min(1, mask + contribution);
        }
        return mask;
    }

    private static double ApplyToneCurve(
        double value,
        double black,
        double quarter,
        double mid,
        double threeQuarter,
        double white)
    {
        double x = Math.Clamp(value, 0, 1);
        double delta;
        if (x < 0.25)
        {
            delta = Lerp(black, quarter, x / 0.25);
        }
        else if (x < 0.5)
        {
            delta = Lerp(quarter, mid, (x - 0.25) / 0.25);
        }
        else if (x < 0.75)
        {
            delta = Lerp(mid, threeQuarter, (x - 0.5) / 0.25);
        }
        else
        {
            delta = Lerp(threeQuarter, white, (x - 0.75) / 0.25);
        }
        return Math.Clamp(x + delta * 0.32, 0, 1);
    }

    private static void ApplyColorWheel(
        ref double r,
        ref double g,
        ref double b,
        double hueShift,
        double saturation,
        double weight)
    {
        if (weight <= 0.001 || saturation <= 0.001) return;
        RgbToHsv(r, g, b, out double hue, out double valueSaturation, out double value);
        double shiftedHue = hue + hueShift * 0.50 * weight;
        double targetSaturation = Math.Clamp(valueSaturation + saturation * 0.55 * weight, 0, 1);
        HsvToRgb(shiftedHue, targetSaturation, value, out double tr, out double tg, out double tb);
        double blend = Math.Clamp(saturation * weight * 0.42, 0, 1);
        r = Lerp(r, tr, blend);
        g = Lerp(g, tg, blend);
        b = Lerp(b, tb, blend);
    }

    private static double Lerp(double a, double b, double t) => a + (b - a) * Math.Clamp(t, 0, 1);

    private static byte ToByte(double value) => (byte)Math.Round(Math.Clamp(value, 0, 1) * 255.0);

    private static void RgbToHsv(double r, double g, double b, out double h, out double s, out double v)
    {
        r = Math.Clamp(r, 0, 1);
        g = Math.Clamp(g, 0, 1);
        b = Math.Clamp(b, 0, 1);
        double max = Math.Max(r, Math.Max(g, b));
        double min = Math.Min(r, Math.Min(g, b));
        double delta = max - min;
        v = max;
        s = max <= 0.00001 ? 0 : delta / max;
        if (delta <= 0.00001)
        {
            h = 0;
        }
        else if (max == r)
        {
            h = ((g - b) / delta) % 6.0;
        }
        else if (max == g)
        {
            h = ((b - r) / delta) + 2.0;
        }
        else
        {
            h = ((r - g) / delta) + 4.0;
        }
        h = (h / 6.0) % 1.0;
        if (h < 0) h += 1.0;
    }

    private static void HsvToRgb(double h, double s, double v, out double r, out double g, out double b)
    {
        h = ((h % 1.0) + 1.0) % 1.0;
        s = Math.Clamp(s, 0, 1);
        v = Math.Clamp(v, 0, 1);
        double c = v * s;
        double x = c * (1.0 - Math.Abs((h * 6.0) % 2.0 - 1.0));
        double m = v - c;
        double rr;
        double gg;
        double bb;
        if (h < 1.0 / 6.0)
        {
            rr = c; gg = x; bb = 0;
        }
        else if (h < 2.0 / 6.0)
        {
            rr = x; gg = c; bb = 0;
        }
        else if (h < 3.0 / 6.0)
        {
            rr = 0; gg = c; bb = x;
        }
        else if (h < 4.0 / 6.0)
        {
            rr = 0; gg = x; bb = c;
        }
        else if (h < 5.0 / 6.0)
        {
            rr = x; gg = 0; bb = c;
        }
        else
        {
            rr = c; gg = 0; bb = x;
        }
        r = rr + m;
        g = gg + m;
        b = bb + m;
    }
}
