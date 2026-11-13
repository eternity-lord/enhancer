using System.ComponentModel;
using System.Windows;

namespace Enhancer.Desktop.Localization;

public sealed class Strings : INotifyPropertyChanged
{
    public static Strings Instance { get; } = new();

    private bool _isPersian;

    public bool IsPersian
    {
        get => _isPersian;
        set
        {
            if (_isPersian == value) return;
            _isPersian = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(string.Empty));
        }
    }

    public FlowDirection WindowFlow => _isPersian ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
    public string LangButtonLabel => _isPersian ? "English (EN)" : "فارسی (FA)";

    public string AppTitle => _isPersian ? "Enhancer v3.0 Studio — نسخه ویندوز" : "Enhancer v3.0 Studio";
    public string AppSubtitle => _isPersian
        ? "افزایش کیفیت هوش مصنوعی، بازسازی چهره CodeFormer و نورپردازی هوشمند — کاملاً آفلاین"
        : "Offline AI Super-Resolution, CodeFormer Face Restoration & Smart HDR — Windows x64";
    public string CreditBadge => _isPersian ? "ساخته شده توسط محمد ایرانی" : "Built by Mohammad Irani";
    public string DiagnosticsBtn => _isPersian ? "عیب‌یابی و تست موتور" : "Diagnostics & Tests";

    // Left Queue Panel
    public string QueueHeader => _isPersian ? "صف تصاویر و فایل‌های ورودی" : "INPUT QUEUE & BATCH";
    public string AddImagesBtn => _isPersian ? "+ افزودن عکس" : "+ Add Photos";
    public string AddFolderBtn => _isPersian ? "+ افزودن پوشه" : "+ Add Folder";
    public string DemoImageBtn => _isPersian ? "نمونه تست" : "Sample Chart";
    public string ClearQueueBtn => _isPersian ? "پاکسازی لیست" : "Clear Queue";
    public string OutputDirHeader => _isPersian ? "پوشه ذخیره‌سازی خروجی" : "OUTPUT DESTINATION";
    public string SameAsInputLabel => _isPersian ? "کنار فایل اصلی (پسوند _Enhancer_v3)" : "Same folder as input (_Enhancer_v3)";
    public string ChooseFolderBtn => _isPersian ? "انتخاب پوشه..." : "Choose...";
    public string ResetFolderBtn => _isPersian ? "پیش‌فرض" : "Auto";
    public string OpenOutputBtn => _isPersian ? "نمایش فایل خروجی در پوشه" : "Show Output in Explorer";

    // Center Canvas
    public string DropTitle => _isPersian
        ? "تصاویر یا پوشه خود را اینجا رها کنید"
        : "Drop Photos or Folders Anywhere Here";
    public string DropSubtitle => _isPersian
        ? "پشتیبانی از JPG، PNG، WebP، BMP، TIFF • اجرا روی کارت گرافیک Vulkan و پردازنده بدون نیاز به گرافیک"
        : "Supports JPG, PNG, WebP, BMP, TIFF • Runs on Vulkan GPUs & GPU-less Multi-Core CPUs";
    public string BeforeBadge => _isPersian ? "قبل (اصلی)" : "BEFORE";
    public string AfterBadge => _isPersian ? "بعد (هوش مصنوعی)" : "AFTER";
    public string SplitSliderLabel => _isPersian ? "مقایسه قبل / بعد:" : "Split Wipe:";
    public string FitBtn => _isPersian ? "جای‌گیری" : "Fit";

    // Action Bar
    public string EnhanceSelectedBtn => _isPersian ? "افزایش کیفیت تصویر انتخابی" : "Enhance Selected";
    public string EnhanceBatchBtn => _isPersian ? "پردازش همه صف" : "Enhance All Queue";
    public string CancelBtn => _isPersian ? "لغو پردازش" : "Cancel";

    // Right Inspector Sections
    public string ModesSectionTitle => _isPersian ? "۱. حالت هوش مصنوعی (۷ حالت استودیویی)" : "1. AI STUDIO MODE (7 PRESETS)";
    public string ModePrime => _isPersian ? "Prime (طبیعی)" : "Prime";
    public string ModeGentle => _isPersian ? "Gentle (ملایم)" : "Gentle";
    public string ModeBalanced => _isPersian ? "Balanced (متعادل)" : "Balanced";
    public string ModeStrong => _isPersian ? "Strong (قوی)" : "Strong";
    public string ModeUltra => _isPersian ? "Ultra (فوق‌العاده)" : "Ultra";
    public string ModeDigitalArt => _isPersian ? "Digital Art (هنری)" : "Digital Art";
    public string ModeOldPhoto => _isPersian ? "Old Photo (قدیمی)" : "Old Photo";

    public string UltraIntensityLabel => _isPersian ? "شدت جزئیات Ultra (Multi-Scale)" : "Ultra Detail Intensity";
    public string UltraSizeLabel => _isPersian ? "اندازه تغییرات (Size of Changes)" : "Ultra Size of Changes";

    public string PortraitLightSection => _isPersian ? "۲. بازسازی چهره و نورپردازی هوشمند" : "2. PORTRAIT FACES & SMART HDR";
    public string EnhanceFacesCheck => _isPersian ? "بازسازی تخصصی چهره (SCRFD + CodeFormer 512×512)" : "Enhance Faces (SCRFD + CodeFormer 512×512)";
    public string FaceStrengthLabel => _isPersian ? "قدرت بازسازی چهره" : "Face Restoration Strength";
    public string LightAiCheck => _isPersian ? "اصلاح نور و رنگ هوشمند (Light AI Smart HDR)" : "Light AI (Smart HDR Exposure & Color)";
    public string LightAiStrengthLabel => _isPersian ? "شدت Light AI" : "Light AI Strength";
    public string DenoiseLabel => _isPersian ? "حذف نویز و فشردگی JPEG قبل از پردازش" : "Pre-SR JPEG Cleanup & Denoise";
    public string SharpnessLabel => _isPersian ? "شارپنس لبه‌ها (بدون هاله)" : "Adaptive Edge Sharpness";

    public string ModelResSection => _isPersian ? "۳. شبکه عصبی و رزولوشن خروجی (تا 16K)" : "3. NEURAL MODEL & RESOLUTION (UP TO 16K)";
    public string NeuralModelLabel => _isPersian ? "مدل شبکه عصبی Super-Resolution" : "Super-Resolution Neural Network";
    public string TargetResLabel => _isPersian ? "رزولوشن هدف" : "Target Output Resolution";
    public string CustomPxLabel => _isPersian ? "ضلع بزرگ دلخواه (px):" : "Custom Long Side (px):";
    public string MaxQualityCheck => _isPersian ? "حداکثر کیفیت (Supersampling کامل + ترکیب ۳۲ پیکسلی)" : "Max Quality (Full Supersampling + 32px Feather)";
    public string TtaCheck => _isPersian ? "پردازش ۸ جهته (8× TTA — دقت فوق‌العاده، ۸ برابر زمان)" : "8× TTA (8-Way Test-Time Augmentation)";

    public string HardwareSection => _isPersian ? "۴. سخت‌افزار رایانه (کارت گرافیک / بدون گرافیک)" : "4. PC HARDWARE (VULKAN GPU & NO-GPU CPU)";
    public string ComputeDeviceLabel => _isPersian ? "موتور پردازش" : "Compute Engine";
    public string CpuThreadsLabel => _isPersian ? "تعداد هسته‌های پردازنده (CPU Threads)" : "CPU OpenMP Threads";
    public string TileSizeLabel => _isPersian ? "اندازه کاشی پردازش (Tile Size)" : "Tile Size (VRAM / Speed)";
    public string DisableFp16Check => _isPersian ? "دقت ۳۲ بیتی کامل روی گرافیک (FP32 Storage)" : "Force 32-bit FP32 Storage on GPU";

    public string ExportSection => _isPersian ? "۵. فرمت خروجی و استاندارد چاپ (DPI)" : "5. EXPORT FORMAT & PRINT DPI";
    public string FormatLabel => _isPersian ? "فرمت تصویر" : "Image Format";
    public string JpegQualityLabel => _isPersian ? "کیفیت JPEG (کروماتیک کامل 4:4:4)" : "JPEG Quality (Full 4:4:4 Chroma)";
    public string PrintDpiLabel => _isPersian ? "تراکم چاپ (DPI Metadata)" : "Print DPI Metadata";

    public event PropertyChangedEventHandler? PropertyChanged;
}
