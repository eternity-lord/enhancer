using System.Text.Json;
using System.Xml.Linq;

namespace Enhancer.Desktop.Lab;

/// <summary>
/// Non-destructive, offline color and portrait controls for Lab. Values are deliberately
/// normalized so a preset can be shared as a small JSON file and imported from Adobe Camera
/// Raw-style XMP without carrying the source photograph.
/// </summary>
public sealed class LabAdjustments
{
    public double Exposure { get; set; }
    public double Contrast { get; set; }
    public double Highlights { get; set; }
    public double Shadows { get; set; }
    public double BlackPoint { get; set; }
    public double WhitePoint { get; set; }
    public double Temperature { get; set; }
    public double Tint { get; set; }
    public double Saturation { get; set; }
    public double Vibrance { get; set; }
    public double Clarity { get; set; }
    public double SkinSmoothing { get; set; }
    public double FaceLight { get; set; }
    public double EyeDetail { get; set; }
    public double TeethWhiten { get; set; }
    public double Sharpening { get; set; }
    public double Vignette { get; set; }
    public double Grain { get; set; }
    public double Fade { get; set; }
    public bool Monochrome { get; set; }

    public LabAdjustments Clone() => new()
    {
        Exposure = Exposure,
        Contrast = Contrast,
        Highlights = Highlights,
        Shadows = Shadows,
        BlackPoint = BlackPoint,
        WhitePoint = WhitePoint,
        Temperature = Temperature,
        Tint = Tint,
        Saturation = Saturation,
        Vibrance = Vibrance,
        Clarity = Clarity,
        SkinSmoothing = SkinSmoothing,
        FaceLight = FaceLight,
        EyeDetail = EyeDetail,
        TeethWhiten = TeethWhiten,
        Sharpening = Sharpening,
        Vignette = Vignette,
        Grain = Grain,
        Fade = Fade,
        Monochrome = Monochrome
    };

    public bool NearlyEquals(LabAdjustments other)
    {
        static bool Close(double a, double b) => Math.Abs(a - b) < 0.0005;
        return Close(Exposure, other.Exposure) &&
               Close(Contrast, other.Contrast) &&
               Close(Highlights, other.Highlights) &&
               Close(Shadows, other.Shadows) &&
               Close(BlackPoint, other.BlackPoint) &&
               Close(WhitePoint, other.WhitePoint) &&
               Close(Temperature, other.Temperature) &&
               Close(Tint, other.Tint) &&
               Close(Saturation, other.Saturation) &&
               Close(Vibrance, other.Vibrance) &&
               Close(Clarity, other.Clarity) &&
               Close(SkinSmoothing, other.SkinSmoothing) &&
               Close(FaceLight, other.FaceLight) &&
               Close(EyeDetail, other.EyeDetail) &&
               Close(TeethWhiten, other.TeethWhiten) &&
               Close(Sharpening, other.Sharpening) &&
               Close(Vignette, other.Vignette) &&
               Close(Grain, other.Grain) &&
               Close(Fade, other.Fade) &&
               Monochrome == other.Monochrome;
    }
}

public sealed class LabPresetFile
{
    public int Version { get; set; } = 1;
    public string App { get; set; } = "Enhancer Lab";
    public string Name { get; set; } = "Custom Lab Preset";
    public LabAdjustments Adjustments { get; set; } = new();
}

public sealed record LabPresetDefinition(
    string Id,
    string NameEn,
    string NameFa,
    LabAdjustments Adjustments
)
{
    public string GetName(bool persian) => persian ? NameFa : NameEn;
}

public static class LabPresetCatalog
{
    public static IReadOnlyList<LabPresetDefinition> BuiltIn { get; } =
    [
        new("natural", "Natural Portrait", "پرتره طبیعی", new LabAdjustments
        {
            Contrast = 0.04, Highlights = -0.08, Shadows = 0.10, Vibrance = 0.10,
            Clarity = 0.05, SkinSmoothing = 0.08, FaceLight = 0.06, EyeDetail = 0.05,
            Sharpening = 0.06
        }),
        new("portrait-glow", "Portrait Glow", "درخشش پرتره", new LabAdjustments
        {
            Exposure = 0.12, Contrast = -0.03, Highlights = -0.16, Shadows = 0.18,
            Temperature = 0.10, Saturation = 0.02, Vibrance = 0.18, Clarity = 0.08,
            SkinSmoothing = 0.28, FaceLight = 0.22, EyeDetail = 0.12, TeethWhiten = 0.12,
            Sharpening = 0.05, Vignette = 0.10
        }),
        new("clean-studio", "Clean Studio", "استودیو تمیز", new LabAdjustments
        {
            Exposure = 0.06, Contrast = 0.10, Highlights = -0.10, Shadows = 0.08,
            WhitePoint = 0.03, Temperature = 0.03, Vibrance = 0.12, Clarity = 0.14,
            SkinSmoothing = 0.16, FaceLight = 0.12, EyeDetail = 0.10, Sharpening = 0.10
        }),
        new("golden-hour", "Golden Hour", "ساعت طلایی", new LabAdjustments
        {
            Exposure = 0.08, Contrast = 0.06, Highlights = -0.12, Shadows = 0.12,
            Temperature = 0.30, Tint = 0.04, Saturation = 0.06, Vibrance = 0.22,
            Clarity = 0.06, Vignette = 0.14, Fade = 0.02
        }),
        new("cinematic-teal", "Cinematic Teal & Orange", "سینمایی تیِل و نارنجی", new LabAdjustments
        {
            Exposure = -0.05, Contrast = 0.22, Highlights = -0.16, Shadows = -0.04,
            Temperature = 0.08, Tint = -0.10, Saturation = 0.04, Vibrance = 0.16,
            Clarity = 0.16, Vignette = 0.22, Fade = 0.04
        }),
        new("matte-film", "Matte Film", "فیلم مات", new LabAdjustments
        {
            Exposure = 0.02, Contrast = -0.08, Highlights = -0.12, Shadows = 0.14,
            BlackPoint = 0.08, WhitePoint = -0.04, Temperature = 0.05, Saturation = -0.08,
            Vibrance = 0.04, Clarity = 0.02, Fade = 0.16, Grain = 0.10, Vignette = 0.08
        }),
        new("hdr-pop", "HDR Detail Pop", "پاپ جزئیات HDR", new LabAdjustments
        {
            Contrast = 0.24, Highlights = -0.22, Shadows = 0.20, BlackPoint = -0.04,
            WhitePoint = 0.06, Saturation = 0.04, Vibrance = 0.22, Clarity = 0.26,
            Sharpening = 0.16
        }),
        new("fine-art-bw", "Fine Art Black & White", "سیاه‌وسفید هنری", new LabAdjustments
        {
            Exposure = 0.04, Contrast = 0.18, Highlights = -0.12, Shadows = 0.12,
            Clarity = 0.18, Sharpening = 0.10, Vignette = 0.18, Grain = 0.08,
            Monochrome = true
        })
    ];
}

public static class LabPresetSerializer
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };

    public static string ToJson(string name, LabAdjustments adjustments) =>
        JsonSerializer.Serialize(new LabPresetFile
        {
            Name = string.IsNullOrWhiteSpace(name) ? "Custom Lab Preset" : name,
            Adjustments = adjustments.Clone()
        }, JsonOptions);

    public static LabPresetFile FromJson(string json)
    {
        var preset = JsonSerializer.Deserialize<LabPresetFile>(json, JsonOptions)
            ?? throw new InvalidDataException("The preset file is empty or invalid.");
        preset.Adjustments ??= new LabAdjustments();
        return preset;
    }

    /// <summary>
    /// Imports the common Adobe Camera Raw / Lightroom XMP adjustment names. Unknown fields
    /// are ignored so an XMP exported by Photoshop remains forward-compatible.
    /// </summary>
    public static LabPresetFile FromXmp(string xml, string fallbackName)
    {
        XDocument document = XDocument.Parse(xml, LoadOptions.PreserveWhitespace);
        string? Get(string localName) => document
            .Descendants()
            .Attributes()
            .FirstOrDefault(a => a.Name.LocalName.Equals(localName, StringComparison.OrdinalIgnoreCase))?.Value
            ?? document.Descendants()
                .FirstOrDefault(e => e.Name.LocalName.Equals(localName, StringComparison.OrdinalIgnoreCase))?.Value;

        double Read(string name, double fallback = 0)
        {
            return double.TryParse(Get(name), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out double value) ? value : fallback;
        }

        var a = new LabAdjustments
        {
            Exposure = Math.Clamp(Read("Exposure2012", Read("Exposure")), -3, 3),
            Contrast = Math.Clamp(Read("Contrast2012", Read("Contrast")) / 100.0, -1, 1),
            Highlights = Math.Clamp(Read("Highlights2012", Read("Highlights")) / 100.0, -1, 1),
            Shadows = Math.Clamp(Read("Shadows2012", Read("Shadows")) / 100.0, -1, 1),
            Saturation = Math.Clamp(Read("Saturation") / 100.0, -1, 1),
            Vibrance = Math.Clamp(Read("Vibrance") / 100.0, -1, 1),
            Tint = Math.Clamp(Read("Tint") / 100.0, -1, 1),
            Clarity = Math.Clamp(Read("Clarity2012", Read("Clarity")) / 100.0, -1, 1),
            Sharpening = Math.Clamp(Read("Sharpening") / 150.0, 0, 1),
            Vignette = Math.Clamp(Math.Abs(Read("VignetteAmount")) / 100.0, 0, 1),
            Grain = Math.Clamp(Read("GrainAmount") / 100.0, 0, 1)
        };

        // XMP Temperature is normally Kelvin, while Lab uses a signed normalized slider.
        double kelvin = Read("Temperature");
        a.Temperature = kelvin > 1000
            ? Math.Clamp((kelvin - 5500.0) / 3000.0, -1, 1)
            : Math.Clamp(kelvin / 100.0, -1, 1);

        bool mono = string.Equals(Get("ProcessVersion"), "BlackAndWhite", StringComparison.OrdinalIgnoreCase)
            || Read("ConvertToGrayscale") > 0.5;
        a.Monochrome = mono;

        return new LabPresetFile
        {
            Version = 1,
            App = "Photoshop / Camera Raw XMP import",
            Name = Path.GetFileNameWithoutExtension(fallbackName),
            Adjustments = a
        };
    }

    public static string ToXmp(string name, LabAdjustments a)
    {
        static string Invariant(double value) => value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        int P(double value) => (int)Math.Round(Math.Clamp(value, -1, 1) * 100.0);
        string xmpName = System.Security.SecurityElement.Escape(
            string.IsNullOrWhiteSpace(name) ? "Enhancer Lab Preset" : name
        ) ?? "Enhancer Lab Preset";
        return $"<?xpacket begin=\"﻿\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n" +
               "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n" +
               " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n" +
               "  <rdf:Description xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"" +
               $" crs:PresetName=\"{xmpName}\" crs:Exposure2012=\"{Invariant(a.Exposure)}\"" +
               $" crs:Contrast2012=\"{P(a.Contrast)}\" crs:Highlights2012=\"{P(a.Highlights)}\"" +
               $" crs:Shadows2012=\"{P(a.Shadows)}\" crs:Saturation=\"{P(a.Saturation)}\"" +
               $" crs:Vibrance=\"{P(a.Vibrance)}\" crs:Tint=\"{P(a.Tint)}\"" +
               $" crs:Clarity2012=\"{P(a.Clarity)}\" crs:Sharpening=\"{P(a.Sharpening)}\"" +
               $" crs:VignetteAmount=\"{P(a.Vignette)}\" crs:GrainAmount=\"{P(a.Grain)}\" />\n" +
               " </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>\n";
    }
}
