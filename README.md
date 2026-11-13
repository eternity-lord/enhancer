# Enhancer v3.0 Studio — Offline AI Image Upscaler, Face Restoration & Smart HDR (Windows x64 & Android)

**Built by Mohammad Irani | ساخته شده توسط محمد ایرانی**

[English](#english) | [فارسی](#فارسی)

---

## English

### Overview
**Enhancer v3.0 Studio** is a **100% offline** AI image super-resolution, portrait face restoration, and Smart HDR enhancement suite for **Windows 10/11 (x64)** and **Android (8.0+)** — powered by a shared high-precision **C++17 + ncnn** native engine. Every neural weight is bundled offline; zero photos ever leave your device.

### Windows v3.0 Highlights (`Enhancer.exe` + `enhancer-cli.exe`)
- **Works on ALL Windows PCs — With OR Without a Graphics Card**:
  - **Systems WITH a Graphics Card (NVIDIA, AMD, Intel)**: Hardware-accelerated **Vulkan GPU** compute (`SimpleVK` dynamic runtime loader), automatic **Discrete GPU** selection on dual-GPU laptops, GPU VRAM heap-budget auto-tiling, anti-gray output verification (`ERR_VULKAN_CORRUPTED`), and automatic `Vulkan FP16 ↔ Vulkan FP32 → CPU` fallback.
  - **Systems WITHOUT a Graphics Card (CPU-Only PCs, Office Laptops, VMs)**: Zero dependency on `vulkan-1.dll` at load time — automatically runs optimized **Multi-Core x86-64 CPU inference** (`OpenMP` + runtime `AVX-512` / `AVX2` / `FMA` / `SSE` dispatch) with configurable thread count.
- **7 Studio Enhancement Modes (100% Unlocked Inspector)**:
  - `Prime`, `Gentle`, `Balanced`, `Strong`, `Ultra` (with **Ultra Detail Intensity** & **Size of Changes** multi-scale synthesis), `Digital Art`, and `Old Photo` — every slider and toggle stays unlocked so you can customize any preset freely.
- **Portrait Face Restoration (`SCRFD-2.5G-KPS` + `CodeFormer 512×512`)**:
  - Automatic 5-point landmark detection, Umeyama similarity alignment, sequential memory-safe `CodeFormer` neural restoration, Oklab skin-tone preservation, and seamless elliptical cosine feathering.
- **Light AI (Smart HDR) & Pre-SR Artifact Cleanup**:
  - Edge-preserving Guided Filter shadow/highlight recovery and adaptive chroma vibrance, plus pre-inference JPEG 8×8 deblocking & chroma denoise.
- **8 Bundled Offline Neural SR Models & Up to 16K Output**:
  - `General v3 x4`, `Real-ESRGAN Photo x4+`, `General WDN Heavy Denoise x4`, `LSDIR Compact Crisp x4`, `Digital Art / Anime x4+`, and `Fast Compact x2 / x3 / x4`.
  - Output targets from `Native` (`2×`/`3×`/`4×`), `2K (2560 px)`, `4K UHD (3840 px)`, `8K UHD (7680 px)`, up to **`16K Print Master (15360 px)`** or any custom pixel size, with **300 DPI** (or 72/150/600 DPI) print metadata embedded in both **4:4:4 Full-Chroma JPEG** and **Lossless PNG**.
- **Interactive Desktop Studio UI & Command-Line Tool**:
  - Dark Studio interface with **Drag & Drop** (files & folders), **Batch Queue**, **Pan/Zoom (`Fit`, `100%`, `200%`, `400%`) Interactive Split Before/After Comparison**, **English & فارسی (RTL)** live toggle, **Hardware Diagnostics / Native Self-Test / Seam-Test**, plus **`enhancer-cli.exe`** for scripting and batch pipelines.

### Installing on Windows (10 / 11 x64)
1. Download **`Enhancer-v3.0-Windows-x64-Setup.exe`** (one-click installer, no admin rights or prerequisites required — `.NET 8`, `VC++/OpenMP`, and all 20 neural model files are bundled) or **`Enhancer-v3.0-Windows-x64-Portable.zip`** from **Releases** or **GitHub Actions → Windows CI Build & Installer → Artifacts**.
2. Launch **Enhancer v3.0 Studio**, drop any image or folder onto the window, select a Studio mode, and click **Enhance Selected**.

### Command-Line Usage (`enhancer-cli.exe`)
```powershell
# Inspect detected Vulkan GPUs, CPU threads, and RAM:
.\enhancer-cli.exe --diag

# Run native Vulkan + Multi-Core CPU verification:
.\enhancer-cli.exe --self-test

# Enhance a portrait to 4K 300 DPI in Ultra mode:
.\enhancer-cli.exe -i portrait.jpg -o portrait_4k.jpg --mode ultra --target 4k --dpi 300

# Force Multi-Core CPU mode on a PC without a GPU:
.\enhancer-cli.exe -i photo.jpg -o photo_enhanced.jpg --cpu --threads 8
```

### Android v3.0 Highlights (`Enhancer-v3.0-debug.apk`)
- Universal offline Android package (`arm64-v8a`, `armeabi-v7a`, `x86_64`, Android 8.0+) with zero internet permission, featuring all 7 Studio modes, `SCRFD + CodeFormer` face restoration, `Light AI`, 8 SR models, full-resolution tiled deep-zoom comparison viewer, 4:4:4 JPEG + PNG 300 DPI export, foreground batch queue with thermal protection, and bilingual `English` / `فارسی` UI.

---

## فارسی

### معرفی Enhancer v3.0 Studio (نسخه ویندوز و اندروید)
**Enhancer نسخه ۳.۰** (**ساخته شده توسط محمد ایرانی**) یک استودیوی کامل و **۱۰۰٪ آفلاین** برای افزایش کیفیت و رزولوشن تصاویر، بازسازی تخصصی چهره با هوش مصنوعی و اصلاح نور هوشمند برای **ویندوز ۱۰ و ۱۱ (۶۴ بیتی)** و **اندروید** است. تمام شبکه‌های عصبی داخل برنامه قرار دارند و هیچ تصویری به اینترنت ارسال نمی‌شود.

### ویژگی‌های کلیدی نسخه ویندوز (`Enhancer.exe` و `enhancer-cli.exe`)
- **اجرای کامل روی تمام رایانه‌ها — با کارت گرافیک و بدون کارت گرافیک**:
  - **رایانه‌های دارای کارت گرافیک (NVIDIA، AMD، Intel)**: شتاب‌دهی سخت‌افزاری با **Vulkan GPU**، انتخاب خودکار کارت گرافیک مجزا (Discrete GPU) در لپ‌تاپ‌های دوگرافیکه، تنظیم هوشمند اندازه کاشی بر اساس حافظه گرافیک، و سیستم ضد تصویر خاکستری.
  - **رایانه‌های بدون کارت گرافیک (فقط CPU)**: بدون هیچ وابستگی اجباری به درایور گرافیک یا `vulkan-1.dll`؛ اجرای خودکار و بهینه روی **پردازنده چند‌هسته‌ای (Multi-Core CPU)** با دستورات برداری `AVX2 / AVX-512 / OpenMP` و امکان تنظیم تعداد هسته‌ها.
- **۷ حالت هوش مصنوعی استودیویی بدون هیچ محدودیت قفل‌شده**:
  - شامل حالت‌های `Prime`، `Gentle`، `Balanced`، `Strong`، `Ultra` (مجهز به اسلایدرهای **شدت جزئیات** و **اندازه تغییرات**)، `Digital Art` و `Old Photo`.
- **بازسازی تخصصی چهره (`SCRFD + CodeFormer 512×512`) و نورپردازی هوشمند (`Light AI`)**:
  - تشخیص دقیق اجزای چهره، بازسازی تارترین چهره‌ها با حفظ رنگ طبیعی پوست در فضای رنگی Oklab، و بازیابی هوشمند سایه‌ها و هایلایت‌ها (Smart HDR).
- **۸ مدل شبکه عصبی آفلاین و خروجی تا رزولوشن 16K با استاندارد چاپ 300 DPI**:
  - خروجی با کیفیت‌های `Native`، `2K`، `4K`، `8K` و **`16K (15360 px)`** در فرمت‌های **JPEG 4:4:4** بدون افت رنگ و **PNG** بدون افت کیفیت، همراه با متادیتای واقعی **300 DPI** مخصوص چاپ حرفه‌ای.
- **رابط کاربری مدرن دو زبانه (فارسی راست‌به‌چپ و انگلیسی)**:
  - پشتیبانی از کشیدن و رها کردن (Drag & Drop) عکس و پوشه، صف پردازش دسته‌ای (Batch Queue)، مقایسه تعاملی **قبل / بعد** با بزرگ‌نمایی تا `800%`، و ابزار خط فرمان `enhancer-cli.exe`.
