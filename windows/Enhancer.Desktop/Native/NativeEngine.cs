using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Enhancer.Desktop.Native;

[StructLayout(LayoutKind.Sequential)]
internal struct EnhancerCJobConfig
{
    public IntPtr ModelsDirUtf8;
    public IntPtr ParamRelUtf8;
    public IntPtr BinRelUtf8;
    public IntPtr OutputPathUtf8;
    public int ModelScale;
    public int TargetLongSide;
    public int TileSize;
    public int DisableFp16;
    public int MaxQuality;
    public int TtaX8;
    public int SafeMode;
    public int ForceCpu;
    public int GpuIndex;
    public int NumThreads;
    public int Sharpness;
    public int EnhanceMode;
    public int EnhanceFaces;
    public float FaceStrength;
    public int LightAi;
    public float LightAiStrength;
    public float DenoiseStrength;
    public float UltraIntensity;
    public float UltraSize;
    public int Dpi;
    public int OutputFormat;
    public int JpegQuality;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EnhancerCResult
{
    public int StatusCode;
    public int OutWidth;
    public int OutHeight;
    public int UsedCpuFallback;
    public int FinalTileSize;
    public int FacesDetected;
    public int FacesRestored;
    public fixed byte BackendUsed[256];
    public fixed byte FallbackSummary[512];
    public fixed byte ErrorDetail[2048];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EnhancerCGpuInfo
{
    public int Index;
    public uint VendorId;
    public uint Type;
    public uint HeapBudgetMb;
    public int IsRecommended;
    public fixed byte Name[256];
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void EnhancerProgressDelegate(
    int progressPct,
    IntPtr backendUtf8,
    int tileSize,
    IntPtr userData
);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void EnhancerSeamProgressDelegate(
    int completedCases,
    int totalCases,
    IntPtr currentLabelUtf8,
    IntPtr userData
);

public sealed record HardwareGpuDevice(
    int Index,
    string Name,
    uint VendorId,
    uint DeviceType,
    uint HeapBudgetMb,
    bool IsRecommended
)
{
    public string TypeLabel => DeviceType switch
    {
        0 => "Discrete GPU",
        1 => "Integrated GPU",
        2 => "Virtual GPU",
        _ => "Software / Other"
    };
}

public sealed record DimensionPlanInfo(
    int SrcWidth,
    int SrcHeight,
    int WorkWidth,
    int WorkHeight,
    int ModelOutWidth,
    int ModelOutHeight,
    int FinalWidth,
    int FinalHeight,
    bool PreDownscaled,
    bool PostUpscaled,
    bool PostDownscaled
);

public sealed record UpscaleJobRequest(
    string ModelsDirectory,
    string ParamRelativePath,
    string BinRelativePath,
    string OutputFilePath,
    int ModelScale,
    int TargetLongSide,
    int TileSize,
    bool DisableFp16,
    bool MaxQuality,
    bool TtaX8,
    bool SafeMode,
    bool ForceCpu,
    int GpuIndex,
    int NumThreads,
    int Sharpness,
    int EnhanceMode,
    bool EnhanceFaces,
    float FaceStrength,
    bool LightAi,
    float LightAiStrength,
    float DenoiseStrength,
    float UltraIntensity,
    float UltraSize,
    int Dpi,
    int OutputFormat,
    int JpegQuality
);

public sealed record UpscaleJobOutcome(
    int StatusCode,
    int OutWidth,
    int OutHeight,
    bool UsedCpuFallback,
    int FinalTileSize,
    int FacesDetected,
    int FacesRestored,
    string BackendUsed,
    string FallbackSummary,
    string ErrorDetail
)
{
    public bool IsSuccess => StatusCode == 0;
    public bool IsCancelled => StatusCode == -1;
}

public sealed record SelfTestOutcome(
    bool OverallSuccess,
    bool VulkanPassed,
    long VulkanMs,
    bool CpuPassed,
    long CpuMs,
    string FullLog
);

public sealed record SeamTestOutcome(
    int StatusCode,
    bool OverallPass,
    string Summary,
    string FormattedTable
);

public static class NativeEngine
{
    private const string DllName = "enhancer_engine";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int enhancer_get_cpu_thread_count(out int outBigCores);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_get_system_memory_mb(out int outAvailMb, out int outTotalMb);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int enhancer_enumerate_gpus(
        [Out] EnhancerCGpuInfo[]? outGpus,
        int maxGpus
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_get_diagnostics_summary(byte[] outUtf8, int outCap);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int enhancer_calibrate_fp16_policy(
        IntPtr modelsDirUtf8,
        out int outVulkanAvailable,
        out int outRecommendDisableFp16,
        byte[] outReasonUtf8,
        int outReasonCap
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_compute_dimensions(
        int srcW,
        int srcH,
        int modelScale,
        int targetLongSide,
        int maxQuality,
        [Out] int[] outPlan9
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_reset_cancel();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_request_cancel();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_cancel_seam_test();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_upscale_rgba(
        IntPtr srcRgba,
        int srcW,
        int srcH,
        int srcStrideBytes,
        ref EnhancerCJobConfig jobCfg,
        EnhancerProgressDelegate? progressFn,
        IntPtr userData,
        out EnhancerCResult outResult
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_upscale_file(
        IntPtr inputPathUtf8,
        ref EnhancerCJobConfig jobCfg,
        EnhancerProgressDelegate? progressFn,
        IntPtr userData,
        out int outSrcW,
        out int outSrcH,
        out EnhancerCResult outResult
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int enhancer_run_self_test(
        IntPtr modelsDirUtf8,
        out int outVulkanPassed,
        out long outVulkanMs,
        out int outCpuPassed,
        out long outCpuMs,
        byte[] outFullLogUtf8,
        int outLogCap
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int enhancer_run_seam_test(
        IntPtr modelsDirUtf8,
        EnhancerSeamProgressDelegate? progressFn,
        IntPtr userData,
        out int outOverallPass,
        byte[] outSummaryUtf8,
        int outSummaryCap,
        byte[] outTableUtf8,
        int outTableCap
    );

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void enhancer_shutdown();

    public static string LocateModelsDirectory()
    {
        string baseDir = AppContext.BaseDirectory;
        string[] candidates =
        [
            baseDir,
            Path.Combine(baseDir, "models"),
            Directory.GetCurrentDirectory(),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "app", "src", "main", "assets")),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "app", "src", "main", "assets"))
        ];

        foreach (string candidate in candidates)
        {
            if (File.Exists(Path.Combine(candidate, "models", "realesrgan-x4plus.param")))
            {
                return candidate;
            }
            if (File.Exists(Path.Combine(candidate, "realesrgan-x4plus.param")))
            {
                return candidate;
            }
        }
        return baseDir;
    }

    public static (int TotalThreads, int BigCores) GetCpuThreads()
    {
        int total = enhancer_get_cpu_thread_count(out int big);
        return (Math.Max(1, total), Math.Max(1, big));
    }

    public static (int AvailableMb, int TotalMb) GetSystemMemoryMb()
    {
        enhancer_get_system_memory_mb(out int avail, out int total);
        return (avail, total);
    }

    public static unsafe IReadOnlyList<HardwareGpuDevice> EnumerateGpus()
    {
        EnhancerCGpuInfo[] buf = new EnhancerCGpuInfo[16];
        int count = enhancer_enumerate_gpus(buf, buf.Length);
        if (count <= 0)
        {
            return Array.Empty<HardwareGpuDevice>();
        }

        int n = Math.Min(count, buf.Length);
        var list = new List<HardwareGpuDevice>(n);
        for (int i = 0; i < n; i++)
        {
            fixed (byte* pName = buf[i].Name)
            {
                string name = ReadNullTerminatedUtf8(pName, 256);
                list.Add(new HardwareGpuDevice(
                    buf[i].Index,
                    string.IsNullOrWhiteSpace(name) ? $"GPU {buf[i].Index}" : name,
                    buf[i].VendorId,
                    buf[i].Type,
                    buf[i].HeapBudgetMb,
                    buf[i].IsRecommended != 0
                ));
            }
        }
        return list;
    }

    public static string GetDiagnosticsSummary()
    {
        byte[] buf = new byte[4096];
        enhancer_get_diagnostics_summary(buf, buf.Length);
        return DecodeUtf8Buffer(buf);
    }

    public static (bool VulkanAvailable, bool RecommendDisableFp16, string Reason) CalibrateFp16Policy(string modelsDir)
    {
        IntPtr pDir = AllocUtf8(modelsDir);
        try
        {
            byte[] reasonBuf = new byte[1024];
            enhancer_calibrate_fp16_policy(
                pDir,
                out int vkAvail,
                out int recDisable,
                reasonBuf,
                reasonBuf.Length
            );
            return (vkAvail != 0, recDisable != 0, DecodeUtf8Buffer(reasonBuf));
        }
        finally
        {
            Marshal.FreeHGlobal(pDir);
        }
    }

    public static DimensionPlanInfo ComputeDimensions(
        int srcW,
        int srcH,
        int modelScale,
        int targetLongSide,
        bool maxQuality
    )
    {
        int[] p = new int[9];
        enhancer_compute_dimensions(srcW, srcH, modelScale, targetLongSide, maxQuality ? 1 : 0, p);
        return new DimensionPlanInfo(
            p[0], p[1],
            p[2], p[3],
            p[4], p[5],
            p[6], p[7],
            (p[8] & 1) != 0,
            (p[8] & 2) != 0,
            (p[8] & 4) != 0
        );
    }

    public static void RequestCancel() => enhancer_request_cancel();
    public static void CancelSeamTest() => enhancer_cancel_seam_test();
    public static void Shutdown() => enhancer_shutdown();

    /// <summary>
    /// Decodes any Windows Imaging Component supported image (.jpg, .png, .webp, .bmp, .tif, .heic)
    /// honoring EXIF orientation, and returns both a frozen WPF BitmapSource for UI display
    /// and a contiguous RGBA8888 byte buffer for the native C++ engine.
    /// </summary>
    public static (BitmapSource PreviewBitmap, byte[] RgbaPixels, int Width, int Height) DecodeImageWithExif(string filePath)
    {
        using var stream = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        var decoder = BitmapDecoder.Create(
            stream,
            BitmapCreateOptions.PreservePixelFormat | BitmapCreateOptions.IgnoreColorProfile,
            BitmapCacheOption.OnLoad
        );
        BitmapFrame frame = decoder.Frames[0];
        BitmapSource oriented = ApplyExifOrientation(frame);

        var bgra = new FormatConvertedBitmap();
        bgra.BeginInit();
        bgra.Source = oriented;
        bgra.DestinationFormat = PixelFormats.Bgra32;
        bgra.EndInit();
        bgra.Freeze();

        int w = bgra.PixelWidth;
        int h = bgra.PixelHeight;
        int stride = checked(w * 4);
        byte[] pixels = new byte[checked(stride * h)];
        bgra.CopyPixels(pixels, stride, 0);

        // Convert BGRA32 -> RGBA8888 in-place for native C++ pipeline (unpremultiplied straight bytes)
        for (int i = 0; i < pixels.Length; i += 4)
        {
            byte b = pixels[i];
            byte r = pixels[i + 2];
            pixels[i] = r;
            pixels[i + 2] = b;
            pixels[i + 3] = 255;
        }

        return (bgra, pixels, w, h);
    }

    public static BitmapSource LoadDisplayBitmapNoLock(string filePath)
    {
        using var stream = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        var bmp = new BitmapImage();
        bmp.BeginInit();
        bmp.CacheOption = BitmapCacheOption.OnLoad;
        bmp.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
        bmp.StreamSource = stream;
        bmp.EndInit();
        bmp.Freeze();
        return bmp;
    }

    private static BitmapSource ApplyExifOrientation(BitmapFrame frame)
    {
        try
        {
            if (frame.Metadata is BitmapMetadata meta &&
                meta.ContainsQuery("System.Photo.Orientation"))
            {
                object? raw = meta.GetQuery("System.Photo.Orientation");
                if (raw is ushort orientation && orientation > 1)
                {
                    Transform? transform = orientation switch
                    {
                        2 => new ScaleTransform(-1, 1),
                        3 => new RotateTransform(180),
                        4 => new ScaleTransform(1, -1),
                        5 => new TransformGroup
                        {
                            Children = [new RotateTransform(90), new ScaleTransform(-1, 1)]
                        },
                        6 => new RotateTransform(90),
                        7 => new TransformGroup
                        {
                            Children = [new RotateTransform(270), new ScaleTransform(-1, 1)]
                        },
                        8 => new RotateTransform(270),
                        _ => null
                    };

                    if (transform != null)
                    {
                        var tb = new TransformedBitmap(frame, transform);
                        tb.Freeze();
                        return tb;
                    }
                }
            }
        }
        catch
        {
            // Ignore malformed EXIF metadata
        }
        return frame;
    }

    public static UpscaleJobOutcome RunUpscaleRgba(
        byte[] rgbaPixels,
        int width,
        int height,
        UpscaleJobRequest req,
        Action<int, string, int>? onProgress
    )
    {
        IntPtr pModels = AllocUtf8(req.ModelsDirectory);
        IntPtr pParam = AllocUtf8(req.ParamRelativePath);
        IntPtr pBin = AllocUtf8(req.BinRelativePath);
        IntPtr pOut = AllocUtf8(req.OutputFilePath);

        EnhancerProgressDelegate? cb = null;
        if (onProgress != null)
        {
            cb = (pct, backendPtr, tileSz, _) =>
            {
                string backend = backendPtr != IntPtr.Zero
                    ? Marshal.PtrToStringUTF8(backendPtr) ?? string.Empty
                    : string.Empty;
                onProgress(pct, backend, tileSz);
            };
        }

        GCHandle pin = GCHandle.Alloc(rgbaPixels, GCHandleType.Pinned);
        try
        {
            var cfg = BuildNativeJobConfig(req, pModels, pParam, pBin, pOut);
            enhancer_reset_cancel();
            enhancer_upscale_rgba(
                pin.AddrOfPinnedObject(),
                width,
                height,
                width * 4,
                ref cfg,
                cb,
                IntPtr.Zero,
                out EnhancerCResult nativeRes
            );
            GC.KeepAlive(cb);
            return MapOutcome(nativeRes);
        }
        finally
        {
            if (pin.IsAllocated) pin.Free();
            Marshal.FreeHGlobal(pModels);
            Marshal.FreeHGlobal(pParam);
            Marshal.FreeHGlobal(pBin);
            Marshal.FreeHGlobal(pOut);
        }
    }

    public static SelfTestOutcome RunSelfTest(string modelsDir)
    {
        IntPtr pDir = AllocUtf8(modelsDir);
        try
        {
            byte[] logBuf = new byte[32768];
            int rc = enhancer_run_self_test(
                pDir,
                out int vkPass,
                out long vkMs,
                out int cpuPass,
                out long cpuMs,
                logBuf,
                logBuf.Length
            );
            return new SelfTestOutcome(
                rc == 0 && cpuPass != 0,
                vkPass != 0,
                vkMs,
                cpuPass != 0,
                cpuMs,
                DecodeUtf8Buffer(logBuf)
            );
        }
        finally
        {
            Marshal.FreeHGlobal(pDir);
        }
    }

    public static SeamTestOutcome RunSeamTest(
        string modelsDir,
        Action<int, int, string>? onProgress
    )
    {
        IntPtr pDir = AllocUtf8(modelsDir);
        EnhancerSeamProgressDelegate? cb = null;
        if (onProgress != null)
        {
            cb = (done, total, lblPtr, _) =>
            {
                string lbl = lblPtr != IntPtr.Zero
                    ? Marshal.PtrToStringUTF8(lblPtr) ?? string.Empty
                    : string.Empty;
                onProgress(done, total, lbl);
            };
        }

        try
        {
            byte[] sumBuf = new byte[4096];
            byte[] tblBuf = new byte[65536];
            int rc = enhancer_run_seam_test(
                pDir,
                cb,
                IntPtr.Zero,
                out int overallPass,
                sumBuf,
                sumBuf.Length,
                tblBuf,
                tblBuf.Length
            );
            GC.KeepAlive(cb);
            return new SeamTestOutcome(
                rc,
                overallPass != 0,
                DecodeUtf8Buffer(sumBuf),
                DecodeUtf8Buffer(tblBuf)
            );
        }
        finally
        {
            Marshal.FreeHGlobal(pDir);
        }
    }

    private static EnhancerCJobConfig BuildNativeJobConfig(
        UpscaleJobRequest req,
        IntPtr pModels,
        IntPtr pParam,
        IntPtr pBin,
        IntPtr pOut
    ) => new()
    {
        ModelsDirUtf8 = pModels,
        ParamRelUtf8 = pParam,
        BinRelUtf8 = pBin,
        OutputPathUtf8 = pOut,
        ModelScale = req.ModelScale,
        TargetLongSide = req.TargetLongSide,
        TileSize = req.TileSize,
        DisableFp16 = req.DisableFp16 ? 1 : 0,
        MaxQuality = req.MaxQuality ? 1 : 0,
        TtaX8 = req.TtaX8 ? 1 : 0,
        SafeMode = req.SafeMode ? 1 : 0,
        ForceCpu = req.ForceCpu ? 1 : 0,
        GpuIndex = req.GpuIndex,
        NumThreads = req.NumThreads,
        Sharpness = req.Sharpness,
        EnhanceMode = req.EnhanceMode,
        EnhanceFaces = req.EnhanceFaces ? 1 : 0,
        FaceStrength = req.FaceStrength,
        LightAi = req.LightAi ? 1 : 0,
        LightAiStrength = req.LightAiStrength,
        DenoiseStrength = req.DenoiseStrength,
        UltraIntensity = req.UltraIntensity,
        UltraSize = req.UltraSize,
        Dpi = req.Dpi,
        OutputFormat = req.OutputFormat,
        JpegQuality = req.JpegQuality
    };

    private static unsafe UpscaleJobOutcome MapOutcome(EnhancerCResult r)
    {
        string backend = ReadNullTerminatedUtf8(r.BackendUsed, 256);
        string fallback = ReadNullTerminatedUtf8(r.FallbackSummary, 512);
        string error = ReadNullTerminatedUtf8(r.ErrorDetail, 2048);
        return new UpscaleJobOutcome(
            r.StatusCode,
            r.OutWidth,
            r.OutHeight,
            r.UsedCpuFallback != 0,
            r.FinalTileSize,
            r.FacesDetected,
            r.FacesRestored,
            backend,
            fallback,
            error
        );
    }

    private static IntPtr AllocUtf8(string? text)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(text ?? string.Empty);
        IntPtr ptr = Marshal.AllocHGlobal(bytes.Length + 1);
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
        Marshal.WriteByte(ptr, bytes.Length, 0);
        return ptr;
    }

    private static string DecodeUtf8Buffer(byte[] buf)
    {
        int len = Array.IndexOf<byte>(buf, 0);
        if (len < 0) len = buf.Length;
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    private static unsafe string ReadNullTerminatedUtf8(byte* ptr, int cap)
    {
        int len = 0;
        while (len < cap && ptr[len] != 0) len++;
        return len == 0 ? string.Empty : Encoding.UTF8.GetString(ptr, len);
    }
}
