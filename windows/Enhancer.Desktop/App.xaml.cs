using System.IO;
using System.Windows;
using Enhancer.Desktop.Models;
using Enhancer.Desktop.Native;

namespace Enhancer.Desktop;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(
                $"Unexpected error:\n{args.Exception.Message}",
                "Enhancer v3.0 Studio",
                MessageBoxButton.OK,
                MessageBoxImage.Warning
            );
            args.Handled = true;
        };

        if (e.Args.Any(a => string.Equals(a, "--smoke-test", StringComparison.OrdinalIgnoreCase)))
        {
            int exitCode = RunHeadlessSmokeTest();
            Shutdown(exitCode);
            return;
        }

        var window = new MainWindow(e.Args);
        MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        try
        {
            NativeEngine.Shutdown();
        }
        catch
        {
        }
        base.OnExit(e);
    }

    private static int RunHeadlessSmokeTest()
    {
        try
        {
            string modelsDir = NativeEngine.LocateModelsDirectory();
            var (threads, bigCores) = NativeEngine.GetCpuThreads();
            var (availMb, totalMb) = NativeEngine.GetSystemMemoryMb();
            var gpus = NativeEngine.EnumerateGpus();
            string diag = NativeEngine.GetDiagnosticsSummary();

            Console.WriteLine($"[SmokeTest] ModelsDir={modelsDir}");
            Console.WriteLine($"[SmokeTest] CPU={threads} ({bigCores} big), RAM={availMb}/{totalMb} MB, GPUs={gpus.Count}");
            Console.WriteLine($"[SmokeTest] Diag={diag}");

            // Synthesize a 64x48 test image in RGBA8888 and run Ultra + Light AI + 300 DPI JPEG 4:4:4
            int w = 64;
            int h = 48;
            byte[] rgba = new byte[w * h * 4];
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int idx = (y * w + x) * 4;
                    rgba[idx + 0] = (byte)(40 + (x * 3) % 180);
                    rgba[idx + 1] = (byte)(70 + (y * 3) % 160);
                    rgba[idx + 2] = 140;
                    rgba[idx + 3] = 255;
                }
            }

            string outFile = Path.Combine(Path.GetTempPath(), $"enhancer_wpf_smoke_{Guid.NewGuid():N}.jpg");
            var req = new UpscaleJobRequest(
                ModelsDirectory: modelsDir,
                ParamRelativePath: "models/realesr-animevideov3-x4.param",
                BinRelativePath: "models/realesr-animevideov3-x4.bin",
                OutputFilePath: outFile,
                ModelScale: 4,
                TargetLongSide: 0,
                TileSize: 128,
                DisableFp16: false,
                MaxQuality: false,
                TtaX8: false,
                SafeMode: false,
                ForceCpu: gpus.Count == 0,
                GpuIndex: -1,
                NumThreads: Math.Min(threads, 4),
                Sharpness: 16,
                EnhanceMode: (int)EnhanceModeKind.Ultra,
                EnhanceFaces: true,
                FaceStrength: 0.70f,
                LightAi: true,
                LightAiStrength: 0.45f,
                DenoiseStrength: 0.15f,
                UltraIntensity: 0.65f,
                UltraSize: 0.45f,
                Dpi: 300,
                OutputFormat: 0,
                JpegQuality: 98
            );

            UpscaleJobOutcome outcome = NativeEngine.RunUpscaleRgba(rgba, w, h, req, null);
            if (!outcome.IsSuccess || outcome.OutWidth != 256 || outcome.OutHeight != 192 || !File.Exists(outFile))
            {
                Console.Error.WriteLine($"[SmokeTest FAILED] code={outcome.StatusCode}, err={outcome.ErrorDetail}");
                return 2;
            }

            byte[] encoded = File.ReadAllBytes(outFile);
            File.Delete(outFile);
            if (encoded.Length < 1024)
            {
                Console.Error.WriteLine("[SmokeTest FAILED] Encoded JPEG unexpectedly small.");
                return 3;
            }

            Console.WriteLine($"[SmokeTest PASS] {w}x{h} -> {outcome.OutWidth}x{outcome.OutHeight} via {outcome.BackendUsed} ({encoded.Length} bytes)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[SmokeTest EXCEPTION] {ex}");
            return 1;
        }
    }
}
