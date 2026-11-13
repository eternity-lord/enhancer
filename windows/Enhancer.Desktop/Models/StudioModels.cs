using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;

namespace Enhancer.Desktop.Models;

public enum EnhanceModeKind
{
    Prime = 0,
    Gentle = 1,
    Balanced = 2,
    Strong = 3,
    Ultra = 4,
    DigitalArt = 5,
    OldPhoto = 6
}

public sealed record ModePresetSpec(
    EnhanceModeKind Kind,
    string DefaultModelSlug,
    bool EnhanceFaces,
    int FaceStrengthPct,
    bool LightAi,
    int LightAiStrengthPct,
    int DenoisePct,
    int UltraIntensityPct,
    int UltraSizePct,
    int SharpnessPct
)
{
    public static readonly IReadOnlyList<ModePresetSpec> All =
    [
        new(EnhanceModeKind.Prime,      "photo-x4",    true,  68, false, 35, 10, 50, 40, 8),
        new(EnhanceModeKind.Gentle,     "lsdir-x4",    false, 60, false, 30, 0,  40, 35, 5),
        new(EnhanceModeKind.Balanced,   "general-v3",  true,  70, true,  45, 18, 55, 45, 12),
        new(EnhanceModeKind.Strong,     "general-wdn", true,  80, true,  60, 60, 60, 50, 18),
        new(EnhanceModeKind.Ultra,      "photo-x4",    true,  75, true,  50, 15, 65, 45, 20),
        new(EnhanceModeKind.DigitalArt, "anime-x4",    false, 60, false, 30, 12, 50, 40, 14),
        new(EnhanceModeKind.OldPhoto,   "photo-x4",    true,  82, true,  40, 35, 55, 45, 10)
    ];

    public static ModePresetSpec Get(EnhanceModeKind kind) =>
        All.FirstOrDefault(m => m.Kind == kind) ?? All[2];
}

public sealed record SrModelOption(
    string Slug,
    string DisplayNameEn,
    string DisplayNameFa,
    string ParamRel,
    string BinRel,
    int Scale
)
{
    public static readonly IReadOnlyList<SrModelOption> All =
    [
        new("general-v3",  "General v3 x4 — Everyday Photos & Portraits (Default)", "عمومی v3 (4x) — عکس‌های روزمره و پرتره",        "models/realesr-general-x4v3.param",     "models/realesr-general-x4v3.bin",     4),
        new("photo-x4",    "Real-ESRGAN Photo x4+ — Maximum Natural Texture",       "فوتورئال x4+ — بیشترین جزئیات طبیعی",           "models/realesrgan-x4plus.param",        "models/realesrgan-x4plus.bin",        4),
        new("general-wdn", "General WDN x4 — Heavy Denoise & Low-Light Rescue",     "حذف نویز قوی WDN (4x) — نور کم و فشرده",        "models/realesr-general-wdn-x4v3.param", "models/realesr-general-wdn-x4v3.bin", 4),
        new("lsdir-x4",    "LSDIR Compact x4 — Clean Crisp Edges & Text",           "فشرده LSDIR (4x) — لبه‌های شارپ و متن",         "models/lsdir-compact-x4.param",         "models/lsdir-compact-x4.bin",         4),
        new("anime-x4",    "Digital Art & Anime x4+ — Illustration & CGI",          "هنر دیجیتال و انیمه x4+ — طراحی و گرافیک",      "models/realesrgan-x4plus-anime.param",  "models/realesrgan-x4plus-anime.bin",  4),
        new("compact-x2",  "Fast Compact x2 — Rapid 2× Upscale (CPU Friendly)",     "فشرده سریع 2x — مناسب پردازنده (CPU)",          "models/realesr-animevideov3-x2.param",  "models/realesr-animevideov3-x2.bin",  2),
        new("compact-x3",  "Fast Compact x3 — Rapid 3× Upscale (CPU Friendly)",     "فشرده سریع 3x — مناسب پردازنده (CPU)",          "models/realesr-animevideov3-x3.param",  "models/realesr-animevideov3-x3.bin",  3),
        new("compact-x4",  "Fast Compact x4 — Rapid 4× Upscale (CPU Friendly)",     "فشرده سریع 4x — مناسب پردازنده (CPU)",          "models/realesr-animevideov3-x4.param",  "models/realesr-animevideov3-x4.bin",  4)
    ];

    public string GetLabel(bool isFa) => isFa ? DisplayNameFa : DisplayNameEn;
}

public sealed record TargetPresetOption(int LongSide, string LabelEn, string LabelFa, bool IsCustom = false)
{
    public static readonly IReadOnlyList<TargetPresetOption> All =
    [
        new(0,     "Native Model Scale (Exact 2× / 3× / 4×)", "مقیاس اصلی مدل (2x / 3x / 4x)"),
        new(2560,  "2K QHD — 2560 px Long Side",              "کیفیت 2K — ضلع بزرگ ۲۵۶۰ پیکسل"),
        new(3840,  "4K UHD — 3840 px Long Side (Default)",    "کیفیت 4K UHD — ضلع بزرگ ۳۸۴۰ پیکسل"),
        new(7680,  "8K UHD — 7680 px Long Side",              "کیفیت 8K UHD — ضلع بزرگ ۷۶۸۰ پیکسل"),
        new(15360, "16K Print Master — 15360 px Long Side",   "چاپ حرفه‌ای 16K — ضلع بزرگ ۱۵۳۶۰ پیکسل"),
        new(-1,    "Custom Long Side (px)...",                "اندازه دلخواه (پیکسل)...", IsCustom: true)
    ];
}

public sealed record ComputeDeviceOption(bool ForceCpu, int GpuIndex, string Label);

public sealed record TileOption(int TilePx, string LabelEn, string LabelFa)
{
    public static readonly IReadOnlyList<TileOption> All =
    [
        new(0,   "Auto (Smart VRAM / RAM Balance)", "خودکار (تنظیم هوشمند حافظه)"),
        new(64,  "64 px — Ultra Safe (Low Memory)", "۶۴ پیکسل — بسیار امن (حافظه کم)"),
        new(128, "128 px — Conservative",           "۱۲۸ پیکسل — مصرف حافظه پایین"),
        new(192, "192 px — Standard",               "۱۹۲ پیکسل — استاندارد"),
        new(256, "256 px — Desktop Balanced",       "۲۵۶ پیکسل — متعادل دسکتاپ"),
        new(384, "384 px — Fast GPU (6+ GB VRAM)",  "۳۸۴ پیکسل — گرافیک سریع (+6GB)"),
        new(512, "512 px — Max Speed (10+ GB VRAM)","۵۱۲ پیکسل — حداکثر سرعت (+10GB)")
    ];
}

public sealed record ThreadOption(int Threads, string Label)
{
    public static readonly IReadOnlyList<ThreadOption> All =
    [
        new(0,  "Auto (Optimal Cores)"),
        new(2,  "2 Threads"),
        new(4,  "4 Threads"),
        new(6,  "6 Threads"),
        new(8,  "8 Threads"),
        new(12, "12 Threads"),
        new(16, "16 Threads"),
        new(24, "24 Threads"),
        new(32, "32 Threads")
    ];
}

public sealed record DpiOption(int Dpi, string Label)
{
    public static readonly IReadOnlyList<DpiOption> All =
    [
        new(72,  "72 DPI — Web / Screen"),
        new(150, "150 DPI — Standard Document"),
        new(300, "300 DPI — Professional Print (Default)"),
        new(600, "600 DPI — Fine Art Archival")
    ];
}

public sealed class QueueImageItem : INotifyPropertyChanged
{
    private string _statusText = "Ready";
    private string? _outputPath;
    private int _outWidth;
    private int _outHeight;
    private bool _isProcessing;
    private bool _isDone;
    private bool _isFailed;

    public required string FilePath { get; init; }
    public string FileName => Path.GetFileName(FilePath);
    public int SrcWidth { get; set; }
    public int SrcHeight { get; set; }

    public string DimensionsBadge => SrcWidth > 0 && SrcHeight > 0
        ? (_outWidth > 0 ? $"{SrcWidth}×{SrcHeight} → {_outWidth}×{_outHeight}" : $"{SrcWidth}×{SrcHeight}")
        : "Image";

    public string StatusText
    {
        get => _statusText;
        set { _statusText = value; OnPropertyChanged(); }
    }

    public string? OutputPath
    {
        get => _outputPath;
        set { _outputPath = value; OnPropertyChanged(); }
    }

    public int OutWidth
    {
        get => _outWidth;
        set { _outWidth = value; OnPropertyChanged(); OnPropertyChanged(nameof(DimensionsBadge)); }
    }

    public int OutHeight
    {
        get => _outHeight;
        set { _outHeight = value; OnPropertyChanged(); OnPropertyChanged(nameof(DimensionsBadge)); }
    }

    public bool IsProcessing
    {
        get => _isProcessing;
        set { _isProcessing = value; OnPropertyChanged(); }
    }

    public bool IsDone
    {
        get => _isDone;
        set { _isDone = value; OnPropertyChanged(); }
    }

    public bool IsFailed
    {
        get => _isFailed;
        set { _isFailed = value; OnPropertyChanged(); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? prop = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(prop));
}

public sealed class UserPreferences
{
    public string Language { get; set; } = "en";
    public int EnhanceMode { get; set; } = (int)EnhanceModeKind.Balanced;
    public string ModelSlug { get; set; } = "general-v3";
    public int TargetPresetLongSide { get; set; } = 3840;
    public int CustomLongSide { get; set; } = 4096;
    public bool EnhanceFaces { get; set; } = true;
    public int FaceStrength { get; set; } = 70;
    public bool LightAi { get; set; } = true;
    public int LightAiStrength { get; set; } = 45;
    public int DenoiseStrength { get; set; } = 18;
    public int UltraIntensity { get; set; } = 65;
    public int UltraSize { get; set; } = 45;
    public int Sharpness { get; set; } = 12;
    public bool MaxQuality { get; set; } = false;
    public bool TtaX8 { get; set; } = false;
    public bool ForceCpu { get; set; } = false;
    public int GpuIndex { get; set; } = -1;
    public int NumThreads { get; set; } = 0;
    public int TileSize { get; set; } = 0;
    public bool DisableFp16 { get; set; } = false;
    public bool DisableFp16UserSet { get; set; } = false;
    public int OutputFormat { get; set; } = 0;
    public int JpegQuality { get; set; } = 98;
    public int Dpi { get; set; } = 300;
    public string CustomOutputFolder { get; set; } = "";

    private static string SettingsFilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "EnhancerStudio",
        "settings.v3.json"
    );

    public static UserPreferences Load()
    {
        try
        {
            string path = SettingsFilePath;
            if (File.Exists(path))
            {
                string json = File.ReadAllText(path);
                return JsonSerializer.Deserialize<UserPreferences>(json) ?? new UserPreferences();
            }
        }
        catch
        {
        }
        return new UserPreferences();
    }

    public void Save()
    {
        try
        {
            string path = SettingsFilePath;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string json = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(path, json);
        }
        catch
        {
        }
    }
}
