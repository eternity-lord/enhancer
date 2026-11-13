using System.Text;
using System.Windows;
using Enhancer.Desktop.Native;

namespace Enhancer.Desktop;

public partial class DiagnosticsWindow : Window
{
    private readonly string _modelsDir;
    private bool _busy;

    public DiagnosticsWindow(string modelsDir)
    {
        InitializeComponent();
        _modelsDir = modelsDir;
        PopulateHardwareSummary();
    }

    private void PopulateHardwareSummary()
    {
        var sb = new StringBuilder();
        var (threads, big) = NativeEngine.GetCpuThreads();
        var (availMb, totalMb) = NativeEngine.GetSystemMemoryMb();
        var gpus = NativeEngine.EnumerateGpus();

        sb.AppendLine("=== Enhancer v3.0 Studio for Windows (x64) — Hardware Report ===");
        sb.AppendLine($"Models Directory     : {_modelsDir}");
        sb.AppendLine($"System Physical RAM  : {availMb:N0} MB free / {totalMb:N0} MB total");
        sb.AppendLine($"CPU Logical Threads  : {threads} total ({big} performance cores)");
        sb.AppendLine($"Vulkan GPUs Detected : {gpus.Count}");

        if (gpus.Count == 0)
        {
            sb.AppendLine("  -> No Vulkan GPU detected on this PC. Multi-Core CPU Precision Engine (OpenMP + AVX2/SSE) is ACTIVE and ready.");
        }
        else
        {
            foreach (var g in gpus)
            {
                string rec = g.IsRecommended ? " [Auto-Selected Primary]" : "";
                sb.AppendLine($"  [GPU {g.Index}]{rec} {g.Name} ({g.TypeLabel}, VRAM Budget: {g.HeapBudgetMb} MB, Vendor: 0x{g.VendorId:X4})");
            }
        }

        sb.AppendLine();
        sb.AppendLine("--- Native Runtime Summary ---");
        sb.AppendLine(NativeEngine.GetDiagnosticsSummary());
        TxtReport.Text = sb.ToString();
    }

    private void BtnRefreshDiag_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        PopulateHardwareSummary();
        TxtStatus.Text = "Hardware diagnostics refreshed.";
    }

    private async void BtnRunSelfTest_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        SetBusy(true, false);
        TxtStatus.Text = "Running Native Self-Test (256x256 -> 1024x1024 on Vulkan & Multi-Core CPU)...";

        try
        {
            SelfTestOutcome res = await Task.Run(() => NativeEngine.RunSelfTest(_modelsDir));
            TxtReport.Text = res.FullLog;
            TxtStatus.Text = res.CpuPassed
                ? $"Self-Test Complete — CPU: PASS ({res.CpuMs} ms) | Vulkan: {(res.VulkanPassed ? $"PASS ({res.VulkanMs} ms)" : "N/A or Fallback")}"
                : "Self-Test reported an issue — inspect log above.";
        }
        finally
        {
            SetBusy(false, false);
        }
    }

    private async void BtnRunSeamTest_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        SetBusy(true, true);
        TxtStatus.Text = "Starting Tile Seam Verification...";

        try
        {
            SeamTestOutcome res = await Task.Run(() =>
                NativeEngine.RunSeamTest(_modelsDir, (done, total, label) =>
                {
                    Dispatcher.BeginInvoke(() =>
                    {
                        TxtStatus.Text = $"Seam-Test [{done}/{total}]: {label}";
                    });
                })
            );
            TxtReport.Text = res.Summary + Environment.NewLine + Environment.NewLine + res.FormattedTable;
            TxtStatus.Text = res.OverallPass ? "Seam-Test PASSED (Zero visible tile seams)." : res.Summary;
        }
        finally
        {
            SetBusy(false, false);
        }
    }

    private void BtnCancelSeam_Click(object sender, RoutedEventArgs e)
    {
        NativeEngine.CancelSeamTest();
        TxtStatus.Text = "Cancelling Seam-Test...";
    }

    private void BtnCopyLog_Click(object sender, RoutedEventArgs e)
    {
        Clipboard.SetText(TxtReport.Text ?? string.Empty);
        TxtStatus.Text = "Copied diagnostics report to clipboard.";
    }

    private void BtnClose_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) NativeEngine.CancelSeamTest();
        Close();
    }

    private void SetBusy(bool busy, bool seamRunning)
    {
        _busy = busy;
        BtnRefreshDiag.IsEnabled = !busy;
        BtnRunSelfTest.IsEnabled = !busy;
        BtnRunSeamTest.IsEnabled = !busy;
        BtnCancelSeam.IsEnabled = seamRunning;
    }
}
