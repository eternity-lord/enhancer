# Enhancer v2.0 (Version 2) — Offline Android AI Image Upscaler (4K / 8K)

**Built by Mohammad Irani | ساخته شده توسط محمد ایرانی**

[English](#english) | [فارسی](#فارسی)

---

## English

### What It Is
**Enhancer (Version 2.0)** is a **100% offline** Android image upscaler (`arm64-v8a`, Android 8.0 / API 26+) powered by **Real-ESRGAN** running natively on-device through **ncnn + Vulkan** (with automatic fallback to multi-threaded ARM64 **CPU** when Vulkan is unavailable or encounters a runtime error).

Enhancer declares **no internet permission at all** (`android.permission.INTERNET` is absent and explicitly stripped at manifest merge time). Every photo stays strictly on your phone.

### What's New in 2.0
- **True Full-Resolution Tiled Deep-Zoom Viewer**:
  - Streams visible tiles directly from the real saved 4K/8K output file via `BitmapRegionDecoder` (never scaling a downsampled preview and never decoding a full 8K `Bitmap` into Java heap).
  - Backed by a 64 MB LRU tile cache with frame-safe recycling, `OutOfMemoryError` protection, smooth tile crossfading, focal pinch-zoom, double-tap zoom (`Fit` → `1:1` → `400%`), inertial pan fling, selectable **Nearest / Bilinear** pixel filtering, live zoom `%` + **`1:1`** HUD badge, and synced **Original** (Catmull-Rom bicubic) vs **Enhanced** split comparison.
- **Max Quality Mode** *(toggle, off by default)*:
  - Always runs Real-ESRGAN on the **full-resolution source** (never pre-downscales before inference), then downscales oversized model outputs to the exact target via anti-aliased **streaming row-band Lanczos3** supersampling.
  - Uses **32 px reflection tile prepadding** and a **32 px 2D linear feathered overlap blend** (exact partition of unity) instead of hard border cropping.
- **Optional TTA (`x8 Mode`)**:
  - Runs inference on all 8 dihedral flip/rotate variants per tile, inverts each transform in float, and averages all 8 passes (~8× slower warning shown, cancelable anytime with accurate sub-pass progress).
- **Optional Adaptive Sharpness Control (`0..100`)**:
  - Light halo-clamped 5-tap Gaussian adaptive unsharp mask (radius ~1.0) applied per tile inside the padded float region before blending so tile borders remain completely seamless.
- **Full-Chroma `4:4:4` JPEG (`90..100`, default `98`)**:
  - Configurable JPEG quality slider (`90..100`, default `98`) encoding true **`4:4:4` full chroma** (`SOF0` `1×1` sampling on Y, Cb, Cr — zero `4:2:0` chroma subsampling across the entire `90..100` range), while keeping lossless **PNG**.
- **Per-Image Re-Run ("Re-process with different settings")**:
  - Re-run any finished result directly from the comparison screen with different model, target, Max Quality, TTA, sharpness, or format settings while keeping both the original image and your already-saved result.
- **Version 2.0 & About Screen**:
  - Displays `Version 2.0` (`BuildConfig.VERSION_NAME`) in the header and adds an **About / Info** screen with localized author credit (`Built by Mohammad Irani` / `ساخته شده توسط محمد ایرانی`) and open-source licenses.

### Core Features (Preserved from 1.0)
- **5 Bundled Offline Models**:
  - **Photo (`realesrgan-x4plus`, 4×)** — General photography and natural textures.
  - **Anime / Illustration (`realesrgan-x4plus-anime`, 4×)** — Crisp line art, digital art, and anime.
  - **Fast (`realesr-animevideov3-x2`, `-x3`, `-x4`)** — Compact models for fast 2×, 3×, or 4× upscaling.
- **Flexible Targets (`Native`, `4K`, `8K`)** with aspect-ratio preservation and automatic tile-size halving down to **48 px** on OOM/Vulkan errors.
- **Sequential Batch Queue, Foreground Service, `PARTIAL_WAKE_LOCK` & Thermal Safety** (`PowerManager` severe thermal auto-pause on Android 10+).
- **Full English & Farsi (فارسی) RTL/LTR UI**.

### How to Get the APK from GitHub Actions (No Android Studio Needed)
1. Open the repository on GitHub and go to the **Actions** tab.
2. Select the **Android CI Build** workflow and click the latest green workflow run.
3. Scroll down to **Artifacts** and download **`Enhancer-v2.0-debug`** (unzip it to get **`Enhancer-v2.0-debug.apk`**, then install on any `arm64-v8a` Android 8.0+ phone).

### Honest Limits
- **Plausible synthesis, not optical recovery**: Real-ESRGAN synthesizes plausible sharp edges and textures learned from training data rather than recovering true lost optical detail. Do **not** rely on AI upscaling for forensic evidence, medical imaging, distant faces, or unreadable text/plates.
- **Speed & heat in Max Quality / TTA x8 / 8K**: `Max Quality` on high-megapixel photos and especially `TTA (x8 Mode)` (~8× more passes) take several minutes per image and generate heat. Thermal safety automatically pauses the queue between items if the phone gets severely hot.
- **Memory & storage**: Lossless `PNG` at 8K is slow and creates 40–90+ MB files; `JPEG 98% (4:4:4)` is strongly recommended.

---

## فارسی

### معرفی برنامه (نسخه ۲.۰)
**Enhancer نسخه ۲.۰** (**ساخته شده توسط محمد ایرانی**) یک برنامه اندرویدی **۱۰۰٪ آفلاین** برای ارتقای کیفیت و وضوح تصاویر تا **4K** و **8K** است که با مدل‌های هوش مصنوعی **Real-ESRGAN** و موتور بومی **ncnn + Vulkan** مستقیماً روی پردازنده گرافیکی گوشی (و در صورت نیاز به‌طور خودکار روی **CPU**) اجرا می‌شود. این برنامه **هیچ مجوز اینترنتی ندارد** و تمام پردازش‌ها کاملاً روی خود گوشی انجام می‌شوند.

### قابلیت‌های جدید در نسخه ۲.۰ (What's New in 2.0)
- **نمایشگر بزرگ‌نمایی عمیق با وضوح واقعی (Full-Res Tiled Deep Zoom)**:
  - نمایش مستقیم فایل واقعی خروجی (4K/8K) به‌صورت کاشی‌بندی‌شده با `BitmapRegionDecoder` و حافظه کش هوشمند ۶۴ مگابایتی بدون بارگذاری کل تصویر 8K در رم جاوا.
  - پشتیبانی از بزرگ‌نمایی دو انگشتی، دوبار لمس (`جای‌گیری` ← `1:1` ← `400%`)، حرکت روان با شتاب (Fling)، انتخاب فیلتر **Bilinear / Nearest**، نمایشگر درصد زوم و نشانگر **`1:1`**، و مقایسه هم‌زمان با تصویر **اصلی (Original)** بزرگ‌شده با الگوریتم **Bicubic**.
- **حالت حداکثر کیفیت (Max Quality)**:
  - اجرای مدل روی وضوح کامل تصویر اصلی (بدون کوچک‌سازی اولیه)، ترکیب نرم **۳۲ پیکسلی** مرز کاشی‌ها بدون هیچ خط درز، و تغییر اندازه جریانی با فیلتر دقیق **Lanczos3**.
- **حالت اختیاری TTA (`x8 Mode`)**:
  - اجرای ۸ حالت چرخش و قرینه برای هر کاشی و میانگین‌گیری دقیق برای لبه‌های تمیزتر (همراه با هشدار زمان ~۸ برابر و قابلیت لغو در هر لحظه).
- **کنترل شارپنس تطبیقی (`0..100`)**:
  - فیلتر شارپنس ملایم و بدون هاله (Halo-clamped Unsharp Mask) روی هر کاشی پیش از ترکیب نهایی.
- **خروجی JPEG با رنگ کامل `4:4:4` (کیفیت ۹۰ تا ۱۰۰، پیش‌فرض ۹۸)**:
  - بدون فشرده‌سازی رنگی `4:2:0`، به همراه حفظ گزینه **PNG** بدون افت.
- **پردازش مجدد با تنظیمات متفاوت (Re-process with different settings)**:
  - امکان اجرای دوباره همان تصویر اصلی با مدل، وضوح، حداکثر کیفیت، TTA یا شارپنس جدید مستقیماً از صفحه مقایسه بدون نیاز به انتخاب مجدد عکس و با حفظ فایل قبلی.
- **بخش درباره برنامه (About)**:
  - نمایش `نسخه 2.0` از `BuildConfig`، نام سازنده (**ساخته شده توسط محمد ایرانی**) و فهرست مجوز کتابخانه‌های متن‌باز.

### دریافت فایل نصبی (APK) از GitHub Actions
۱. در صفحه مخزن GitHub وارد بخش **Actions** و گردش‌کار **Android CI Build** شوید.
۲. آخرین اجرای سبز رنگ را باز کرده و از بخش **Artifacts** فایل **`Enhancer-v2.0-debug`** را دانلود کنید (`Enhancer-v2.0-debug.apk`).

---

## Credits & Open-Source Licenses
- **Author**: Built by Mohammad Irani (ساخته شده توسط محمد ایرانی)
- **[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)** — BSD 3-Clause License.
- **[ncnn](https://github.com/Tencent/ncnn)** — BSD 3-Clause License.
- **[stb (`stb_image.h`, `stb_image_write.h`)](https://github.com/nothings/stb)** — Public Domain (Unlicense) / MIT License.
