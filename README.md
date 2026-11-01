# Enhancer — Offline Android AI Image Upscaler (4K / 8K)

[English](#english) | [فارسی](#فارسی)

---

## English

### What It Is
**Enhancer** is a **100% offline** Android image upscaler (`arm64-v8a`, Android 8.0 / API 26+) powered by **Real-ESRGAN** running natively on-device through **ncnn + Vulkan** (with automatic fallback to multi-threaded ARM64 **CPU** when Vulkan is unavailable or encounters a runtime error).

 Enhancer declares **no internet permission at all** (`android.permission.INTERNET` is absent and explicitly stripped at manifest merge time). Every photo stays strictly on your phone.

### Key Features
- **5 Bundled Offline Models**:
  - **Photo (`realesrgan-x4plus`, 4×)** — General photography and natural textures.
  - **Anime / Illustration (`realesrgan-x4plus-anime`, 4×)** — Crisp line art, digital art, and anime.
  - **Fast (`realesr-animevideov3-x2`, `-x3`, `-x4`)** — Compact models for fast 2×, 3×, or 4× upscaling.
- **Flexible Targets (`Native`, `4K`, `8K`)**:
  - **Native (`xN`)**: Exact model scale with no pre/post resize.
  - **4K (`3840 px` long side)** & **8K (`7680 px` long side)** preserving aspect ratio:
    - If `source_long * m >= target`: area/box-filters the source down to `target / m` first and runs the model once (with a UI notice that AI detail gain is limited on already-large sources).
    - If `source_long * m < target`: runs the model at native resolution, then upscales to the exact target using a custom **streaming row-by-row Lanczos3** resizer (~737 KB rolling row cache).
- **Mandatory Seam-Free Tiling & Auto-Recovery**:
  - Processes tiles with **10 px prepadding** and discards the padded border on output so tile boundaries have zero visible seams.
  - Default tile size **200 px** (`Auto`), or manual `256 / 192 / 128 / 64 px`. On allocation or Vulkan failure, automatically halves the tile size down to **48 px** and retries; if Vulkan still fails, seamlessly switches to **CPU** and reports which backend ran.
- **Native-First Memory Architecture**:
  - Never allocates a Java `Bitmap` for 4K/8K outputs. Source pixels are read via `AndroidBitmap_lockPixels`, processed in native `uint8_t` RGB buffers, and streamed directly to a detached `MediaStore` file descriptor (`ParcelFileDescriptor.detachFd()`) via `stb_image_write` (`JPEG` quality 95 default, or lossless `PNG`).
- **Batch Queue, Foreground Service & Thermal Safety**:
  - Multi-image selection via Android Photo Picker or `ACTION_OPEN_DOCUMENT`.
  - Per-image resolution preview and `ActivityManager` RAM estimate + low-RAM warnings.
  - Runs in a Foreground Service with a `PARTIAL_WAKE_LOCK` and live progress notification so processing continues with the screen off.
  - Monitors `PowerManager` thermal status (Android 10+) and pauses the queue automatically on **Severe** thermal throttling.
- **Before / After Comparison & Bilingual RTL/LTR UI**:
  - Interactive draggable Before/After split slider with pinch-to-zoom (rendered from downscaled decodes, never loading the full 8K bitmap into Java heap), plus **Share** and **Open in Gallery** buttons.
  - Full **English** and **Farsi (فارسی)** localization with automatic RTL layout support.

### How to Get the APK from GitHub Actions (No Android Studio Needed)
1. Push your branch/commit to GitHub (or open the repository on GitHub).
2. Go to the **Actions** tab and select the **Android CI Build** workflow.
3. Click the latest green workflow run (or click **Run workflow** via `workflow_dispatch`).
4. Scroll down to the **Artifacts** section at the bottom of the run summary and download **`app-debug`** (unzip `app-debug.zip` to get `app-debug.apk`, then install it on any `arm64-v8a` Android 8.0+ phone).

### Usage
1. Tap **Pick Photos** (Photo Picker) or **Browse Files** (`ACTION_OPEN_DOCUMENT`) to add one or multiple images.
2. Choose your **AI Model**, **Target Resolution** (`Native`, `4K`, or `8K`), **Output Format** (`JPEG 95%` recommended, or `PNG`), **Tile Size** (`Auto` recommended), and enable **Disable fp16** only if your GPU exhibits color artifacts.
3. Review per-image output dimensions and estimated RAM usage, then tap **Start Upscaling**.
4. Grant notification permission (Android 13+) if prompted so you can monitor tile-by-tile progress while the screen is off.
5. Enhanced images are saved automatically to **`Pictures/Enhancer/<original>_<target>_<model>.jpg|png`**. Tap **Compare & Share** on any completed item to inspect Before/After details, open in Gallery, or share.

### Honest Limits
- **Plausible synthesis, not optical recovery**: Real-ESRGAN is a generative super-resolution network. It **invents plausible sharp edges and textures** learned from training data rather than recovering true lost optical information. Do **not** rely on AI upscaling for forensic evidence, medical imaging, small faces at a distance, or illegible license plates/text.
- **Speed & battery/heat at 8K**: Upscaling to 8K (`7680 px`) with `realesrgan-x4plus` requires hundreds of heavy convolution tiles and **takes several minutes per photo** depending on your mobile GPU (Adreno / Mali / Xclipse) — and significantly longer if falling back to CPU. Use the **Fast (`animevideov3`)** models or **4K** target for much faster turnaround.
- **Memory & storage**: An uncompressed 8K RGB buffer uses ~100–135 MB of native RAM. Devices with limited free RAM should prefer `4K`, `JPEG`, and smaller tile sizes (`128` or `64`). Lossless `PNG` encoding at 8K is slow and produces 40–90+ MB files.
- **Architecture**: Built exclusively for 64-bit ARM (`arm64-v8a`) Android 8.0+ devices.

---

## فارسی

### معرفی برنامه
**Enhancer** یک برنامه اندرویدی **کاملاً آفلاین** برای افزایش کیفیت و وضوح تصاویر تا **4K** و **8K** است که با استفاده از مدل‌های هوش مصنوعی **Real-ESRGAN** و موتور بومی **ncnn + Vulkan** مستقیماً روی پردازنده گرافیکی گوشی (و در صورت عدم پشتیبانی Vulkan، به‌صورت خودکار روی **CPU**) اجرا می‌شود.

این برنامه **هیچ مجوز اینترنتی ندارد** (`INTERNET` به‌طور کامل حذف شده است) و تمام مراحل پردازش بدون ارسال هیچ داده‌ای به بیرون، روی دستگاه شما انجام می‌گیرد.

### دریافت فایل نصبی (APK) از GitHub Actions (بدون نیاز به Android Studio)
۱. وارد صفحه مخزن در GitHub شوید و به زبانه **Actions** بروید.
۲. گردش‌کار **Android CI Build** و آخرین اجرای موفق (سبز رنگ) را باز کنید (یا دکمه **Run workflow** را بزنید).
۳. در پایین صفحه در بخش **Artifacts**، بسته **`app-debug`** را دانلود کرده، فایل `app-debug.apk` را از حالت فشرده (`.zip`) خارج کنید و روی گوشی اندروید ۸ به بالا (`arm64-v8a`) نصب نمایید.

### راهنمای استفاده
۱. با دکمه **انتخاب تصاویر** یا **مرور فایل‌ها** یک یا چند عکس را به صف اضافه کنید.
۲. **مدل هوش مصنوعی** (`عکس واقعی x4plus`، `انیمه x4plus-anime`، یا مدل‌های `سریع ۲/۳/۴ برابر`)، **وضوح خروجی** (`اصلی مدل`، `4K` یا `8K`)، **فرمت ذخیره** (`JPEG 95%` پیشنهادی یا `PNG`) و **اندازه کاشی (Tile)** را تنظیم کنید. در صورتی که پردازنده گرافیکی گوشی شما دچار به‌هم‌ریختگی رنگ شد، گزینه **غیرفعال‌سازی دقت fp16** را روشن کنید.
۳. وضوح خروجی و **رم تخمینی** هر تصویر را بررسی کرده و روی **شروع پردازش** بزنید.
۴. پردازش در قالب یک سرویس پیش‌زمینه (Foreground Service) همراه با قفل بیداری (`WakeLock`) انجام می‌شود تا حتی با خاموش شدن صفحه نمایش ادامه یابد. همچنین در صورت داغ شدن شدید دستگاه (`PowerManager`)، صف به‌طور خودکار متوقف می‌شود تا گوشی خنک شود.
۵. خروجی‌ها در مسیر **`Pictures/Enhancer`** با نام `<original>_<target>_<model>.jpg|png` ذخیره می‌شوند و با دکمه **مقایسه و اشتراک‌گذاری** می‌توانید پیش‌نمایش قبل و بعد را با خط‌کش کشویی مقایسه کنید.

### محدودیت‌های واقعی و صادقانه
- **بازآفرینی جزئیات محتمل، نه بازیابی نوری واقعی**: هوش مصنوعی Real-ESRGAN جزئیات از دست‌رفته واقعی دوربین را برنمی‌گرداند، بلکه بافت‌ها و لبه‌های **محتمل و باورپذیر** تولید می‌کند. بنابراین برای متون بسیار ریز و ناخوانا، چهره‌های خیلی دور، مدارک پزشکی یا مستندات قانونی مناسب نیست.
- **زمان پردازش و مصرف انرژی در 8K**: پردازش در وضوح **8K** (به‌ویژه با مدل سنگین `x4plus`) بسته به تراشه گوشی **چندین دقیقه به ازای هر عکس** زمان می‌برد و در حالت CPU کندتر خواهد بود. برای سرعت بسیار بالاتر از وضوح **4K** یا مدل‌های **سریع (`animevideov3`)** استفاده کنید.
- **مصرف رم و حجم فایل PNG**: بافر بومی یک تصویر 8K حدود ۱۰۰ تا ۱۳۵ مگابایت رم نیاز دارد. در گوشی‌های با رم کم، از وضوح `4K`، فرمت `JPEG` و اندازه کاشی کوچک‌تر (`128` یا `64`) استفاده کنید. ذخیره `PNG` در 8K بسیار کند و حجیم (۴۰ تا ۹۰ مگابایت) است.

---

## Credits & Third-Party Licenses
- **[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)** — Copyright (c) Xintao Wang / Tencent ARC Lab. Licensed under the **BSD 3-Clause License**.
- **[ncnn](https://github.com/Tencent/ncnn)** — Copyright (c) Tencent. Licensed under the **BSD 3-Clause License**.
- **[stb (`stb_image.h`, `stb_image_write.h`)](https://github.com/nothings/stb)** — Written by Sean Barrett and contributors. Available under **Public Domain (Unlicense)** or **MIT License**.
