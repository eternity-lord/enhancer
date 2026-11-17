# Enhancer v3.2 Studio — Offline AI Image Upscaler, Face Restoration & Smart HDR (Windows x64 & Android)

**Built by Mohammad Irani | ساخته شده توسط محمد ایرانی**

[English](#english) | [فارسی](#فارسی)

---

## English

### Overview
**Enhancer v3.2 Studio** is a **100% offline** AI image super-resolution, portrait face restoration, and Smart HDR enhancement suite for **Windows 10/11 (x64)** and **Android (8.0+)** — powered by a shared high-precision **C++17 + ncnn** native engine. Every neural weight is bundled offline; zero photos ever leave your device.

### Windows v3.2 Highlights (`Enhancer.exe` + `enhancer-cli.exe`)
- **Reference-quality default profile**: Ultra + the v3 Real-ESRGAN Photo x4+ model + native output + Max Quality + TTA x8, with conservative identity-safe face detail and no default denoise/HDR color rewrite.
- **Works on ALL Windows PCs — With OR Without a Graphics Card**:
  - **Systems WITH a Graphics Card (NVIDIA, AMD, Intel)**: Hardware-accelerated **Vulkan GPU** compute (`SimpleVK` dynamic runtime loader), automatic **Discrete GPU → Integrated/onboard GPU** selection on hybrid laptops, shared-memory-safe auto-tiling for Intel UHD/Iris and AMD iGPUs, anti-gray output verification (`ERR_VULKAN_CORRUPTED`), and automatic `Vulkan FP16 ↔ Vulkan FP32 → CPU` fallback. The new DXGI inventory also lists legacy adapters such as **GT 730** and **Intel HD Graphics 4600** instead of silently showing “no GPU”; their neural acceleration is enabled when the installed vendor driver exposes Vulkan, otherwise the app keeps a verified multi-core CPU path.
  - **Systems WITHOUT a Graphics Card (CPU-Only PCs, Office Laptops, VMs)**: Zero dependency on `vulkan-1.dll` at load time — automatically runs optimized **Multi-Core x86-64 CPU inference** (`OpenMP` + runtime `AVX-512` / `AVX2` / `FMA` / `SSE` dispatch) with configurable thread count.
- **LAB — professional offline color and tone room**: Exposure, tone/color controls, five-point RGB curves, shadow/highlight color wheels, green face-protection masks, batch preset application, clarity, sharpening, vignette, film grain, fade and black & white; built-in modern portrait/cinematic presets; undo/redo; non-destructive before/after preview; import Adobe Camera Raw/Photoshop-compatible `.xmp` settings and export portable `.enhancerpreset` / JSON presets; high-quality PNG/JPEG export without uploading photos.
- **Dedicated Skin Retouch workspace**: Simple local manual retouch is deliberately separate from Lab. Paint an orange mask over selected skin, adjust strength and texture preservation, verify changed-pixel metrics in Before/After, and save a collision-safe JPEG/PNG copy while the original remains untouched. This is not full-face AI reconstruction.
- **7 Studio Enhancement Modes (100% Unlocked Inspector)**:
  - `Prime`, `Gentle`, `Balanced`, `Strong`, `Ultra` (with **Ultra Detail Intensity** & **Size of Changes** multi-scale synthesis), `Digital Art`, and `Old Photo` — every slider and toggle stays unlocked so you can customize any preset freely.
- **Maximum Portrait Face Restoration (`SCRFD-2.5G-KPS` + `CodeFormer 512×512`)**:
  - Face strength defaults to 100% across face-enabled modes and all SR backbones. Normal portraits use a feature-focused eye/brow/nose/mouth pass; genuinely tiny or soft faces automatically receive stronger anatomical recovery, with Oklab skin-tone preservation and seamless elliptical feathering.
- **Light AI (Smart HDR) & Pre-SR Artifact Cleanup**:
  - Edge-preserving Guided Filter shadow/highlight recovery and adaptive chroma vibrance, plus pre-inference JPEG 8×8 deblocking & chroma denoise.
- **8 Bundled Offline Neural SR Models & Up to 16K Output**:
  - `General v3 x4`, `Real-ESRGAN Photo x4+`, `General WDN Heavy Denoise x4`, `LSDIR Compact Crisp x4`, `Digital Art / Anime x4+`, and `Fast Compact x2 / x3 / x4`.
  - Output targets from `Native` (`2×`/`3×`/`4×`), `2K (2560 px)`, `4K UHD (3840 px)`, `8K UHD (7680 px)`, up to **`16K Print Master (15360 px)`** or any custom pixel size, with **300 DPI** (or 72/150/600 DPI) print metadata embedded in both **4:4:4 Full-Chroma JPEG** and **Lossless PNG**.
- **Interactive Desktop Studio UI & Command-Line Tool**:
  - Dark Studio interface with **Drag & Drop** (files & folders), **Batch Queue**, **Pan/Zoom (`Fit`, `100%`, `200%`, `400%`) Interactive Split Before/After Comparison**, **English & فارسی (RTL)** live toggle, **Hardware Diagnostics / Native Self-Test / Seam-Test**, plus **`enhancer-cli.exe`** for scripting and batch pipelines.

### Installing on Windows (10 / 11 x64)
1. Download **`Enhancer-v3.2-Windows-x64-Setup.exe`** (one-click installer, no admin rights or prerequisites required — `.NET 8`, `VC++/OpenMP`, and all 20 neural model files are bundled) or **`Enhancer-v3.2-Windows-x64-Portable.zip`** from **Releases** or **GitHub Actions → Windows CI Build & Installer → Artifacts**.
2. Launch **Enhancer v3.2 Studio**, drop any image or folder onto the window, select a Studio mode, and click **Enhance Selected**.

### Command-Line Usage (`enhancer-cli.exe`)
```powershell
# Inspect detected Vulkan GPUs, CPU threads, and RAM:
.\enhancer-cli.exe --diag

# Run native Vulkan + Multi-Core CPU verification:
.\enhancer-cli.exe --self-test

# Start with the highest-detail identity-safe profile (native Photo x4, TTA x8, JPEG 100%):
.\enhancer-cli.exe -i portrait.jpg -o portrait_native.jpg --mode ultra --target native --dpi 300

# Enhance a portrait to a fixed 4K 300 DPI target when required:
.\enhancer-cli.exe -i portrait.jpg -o portrait_4k.jpg --mode ultra --target 4k --dpi 300

# Force Multi-Core CPU mode on a PC without a GPU:
.\enhancer-cli.exe -i photo.jpg -o photo_enhanced.jpg --cpu --threads 8

# Use Intel/AMD integrated (onboard) Vulkan graphics explicitly when available:
.\enhancer-cli.exe -i photo.jpg -o photo_igpu.jpg --gpu integrated --tile 128
```

### Android v3.2 Highlights (`Enhancer-v3.2-universal.apk`)
- **Reference-quality defaults**: new installs start in Ultra with the v3 Real-ESRGAN Photo x4+ model, native x4 output, Max Quality full-resolution inference, TTA x8 ensemble processing, micro-detail recovery, JPEG 100% 4:4:4 output, and lossless PNG when exact pixels are required. Existing model choices remain unchanged.
- **Faster without quality shortcuts**: tile/compositor scratch is reused, duplicate JNI progress callbacks are coalesced, CPU/OpenMP scheduling is bounded to the selected ncnn worker pool, the streaming resizer reuses its row-window pointers, and PNG uses a faster lossless encoder path.
- **Maximum-detail targets**: Native x4 is the starting profile shown in the reference comparison; 2K, 4K, 8K, and 16K Print Master (15360 px) remain available. Output buffers are streamed/tiled to keep memory bounded up to the supported 2 GiB delivery limit.
- **Identity-safe faces**: SCRFD + CodeFormer is still available, but the default face residual is conservative, color-matched, and channel-delta-limited so eyelashes and skin detail can improve without redrawing facial geometry.
- Universal offline Android package (`arm64-v8a`, `armeabi-v7a`, `x86_64`, Android 8.0+) with zero internet permission, all 7 Studio modes, `SCRFD + CodeFormer` face restoration, `Light AI`, 8 SR models, full-resolution tiled deep-zoom comparison viewer, 300 DPI metadata, foreground batch queue with thermal protection, and bilingual `English` / `فارسی` UI.
- Download the verified APK and SHA-256 file from the [Enhancer v3.2.0 Android release](https://github.com/eternity-lord/enhancer/releases/tag/v3.2.0).

---

## فارسی

### معرفی Enhancer v3.2 Studio (نسخه ویندوز و اندروید)
**Enhancer نسخه ۳.۲** (**ساخته شده توسط محمد ایرانی**) یک استودیوی کامل و **۱۰۰٪ آفلاین** برای افزایش کیفیت و رزولوشن تصاویر، بازسازی تخصصی چهره با هوش مصنوعی و اصلاح نور هوشمند برای **ویندوز ۱۰ و ۱۱ (۶۴ بیتی)** و **اندروید** است. تمام شبکه‌های عصبی داخل برنامه قرار دارند و هیچ تصویری به اینترنت ارسال نمی‌شود.

### ویژگی‌های کلیدی نسخه ویندوز (`Enhancer.exe` و `enhancer-cli.exe`)
- **پروفایل پیش‌فرض با بالاترین جزئیات و حفظ هویت چهره**: حالت Ultra، مدل Real-ESRGAN Photo x4+ نسخهٔ ۳، خروجی Native x4، Max Quality، TTA x8 و ترمیم محافظه‌کارانه چهره بدون بازطراحی هندسه صورت.
- **اجرای کامل روی تمام رایانه‌ها — با کارت گرافیک و بدون کارت گرافیک**:
  - **رایانه‌های دارای کارت گرافیک (NVIDIA، AMD، Intel)**: شتاب‌دهی سخت‌افزاری با **Vulkan GPU**، انتخاب خودکار کارت مجزا و در صورت نامعتبر بودن آن استفاده از گرافیک آنبورد، تنظیم هوشمند کاشی بر اساس VRAM یا حافظه اشتراکی، و سیستم ضد تصویر خاکستری. فهرست جدید DXGI کارت‌های قدیمی مانند **GT 730** و **Intel HD Graphics 4600** را هم شناسایی و در Diagnostics نمایش می‌دهد؛ شتاب‌دهی عصبی آن‌ها به پشتیبانی درایور Vulkan همان مدل وابسته است و در غیر این صورت مسیر CPU چند‌هسته‌ای به‌صورت مطمئن فعال می‌شود. از منوی Hardware و گزینه `--gpu integrated` هم می‌توان گرافیک آنبورد Vulkan را مستقیم انتخاب کرد.
  - **رایانه‌های بدون کارت گرافیک (فقط CPU)**: بدون هیچ وابستگی اجباری به درایور گرافیک یا `vulkan-1.dll`؛ اجرای خودکار و بهینه روی **پردازنده چند‌هسته‌ای (Multi-Core CPU)** با دستورات برداری `AVX2 / AVX-512 / OpenMP` و امکان تنظیم تعداد هسته‌ها.
- **LAB — اتاق حرفه‌ای رنگ و تون کاملاً آفلاین**: کنترل نور و تون/رنگ، منحنی‌های RGB پنج‌نقطه‌ای، چرخ‌های رنگ سایه/روشنایی، ماسک سبز محافظ چهره، اعمال دسته‌ای پریست، وضوح، شارپنس، وینیت، دانه فیلم، Fade و سیاه‌وسفید؛ پریست‌های مدرن پرتره/سینمایی؛ Undo/Redo؛ پیش‌نمایش غیرمخرب قبل/بعد؛ ورود تنظیمات `.xmp` سازگار با Photoshop/Adobe Camera Raw و خروجی پریست `.enhancerpreset` / JSON؛ ذخیره PNG/JPEG با کیفیت بالا.
- **فضای مستقل روتوش پوست**: روتوش دستی ساده و موضعی عمداً از LAB جدا شده است. روی ناحیه انتخابی ماسک نارنجی بکشید، شدت و حفظ بافت را تنظیم کنید، نتیجه و تعداد پیکسل‌های تغییرکرده را در Before/After ببینید و یک کپی JPEG/PNG بدون تغییر فایل اصلی ذخیره کنید. این بخش بازسازی کامل چهره با هوش مصنوعی نیست.
- **۷ حالت هوش مصنوعی استودیویی بدون هیچ محدودیت قفل‌شده**:
  - شامل حالت‌های `Prime`، `Gentle`، `Balanced`، `Strong`، `Ultra` (مجهز به اسلایدرهای **شدت جزئیات** و **اندازه تغییرات**)، `Digital Art` و `Old Photo`.
- **بازسازی فوق‌قوی چهره (`SCRFD + CodeFormer 512×512`) و نورپردازی هوشمند (`Light AI`)**:
  - شدت چهره در حالت‌های دارای بازسازی روی ۱۰۰٪ قرار دارد؛ عکس‌های معمولی با تمرکز روی چشم، ابرو، بینی و لب پردازش می‌شوند و چهره‌های واقعاً کوچک یا تار به‌صورت تطبیقی بازسازی گسترده‌تری می‌گیرند، با حفظ رنگ طبیعی پوست در فضای Oklab.
- **۸ مدل شبکه عصبی آفلاین و خروجی تا رزولوشن 16K با استاندارد چاپ 300 DPI**:
  - خروجی با کیفیت‌های `Native`، `2K`، `4K`، `8K` و **`16K (15360 px)`** در فرمت‌های **JPEG 4:4:4** بدون افت رنگ و **PNG** بدون افت کیفیت، همراه با متادیتای واقعی **300 DPI** مخصوص چاپ حرفه‌ای.
- **رابط کاربری مدرن دو زبانه (فارسی راست‌به‌چپ و انگلیسی)**:
  - پشتیبانی از کشیدن و رها کردن (Drag & Drop) عکس و پوشه، صف پردازش دسته‌ای (Batch Queue)، مقایسه تعاملی **قبل / بعد** با بزرگ‌نمایی تا `800%`، و ابزار خط فرمان `enhancer-cli.exe`.
