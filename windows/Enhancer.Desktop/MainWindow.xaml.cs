using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Enhancer.Desktop.Localization;
using Enhancer.Desktop.Models;
using Enhancer.Desktop.Native;
using Microsoft.Win32;

namespace Enhancer.Desktop;

public partial class MainWindow : Window
{
    private static readonly HashSet<string> SupportedExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"
    };

    private readonly ObservableCollection<QueueImageItem> _queue = [];
    private readonly List<ComputeDeviceOption> _deviceOptions = [];
    private readonly string _modelsDir;
    private readonly UserPreferences _prefs;

    private bool _initializing = true;
    private bool _isProcessing;
    private bool _batchCancelRequested;
    private EnhanceModeKind _activeMode = EnhanceModeKind.Balanced;

    // Interactive Canvas Pan/Zoom/Split State
    private double _stageBaseWidth;
    private double _stageBaseHeight;
    private double _zoomFactor = 1.0;
    private bool _autoFitZoom = true;
    private double _splitRatio = 0.5;
    private bool _draggingSplit;
    private bool _panningCanvas;
    private Point _panStartMouse;
    private Point _panStartTranslate;

    public MainWindow(string[] startupArgs)
    {
        InitializeComponent();
        _modelsDir = NativeEngine.LocateModelsDirectory();
        _prefs = UserPreferences.Load();

        LstQueue.ItemsSource = _queue;

        PopulateDropdownsAndHardware();
        ApplyPreferencesToUi();
        _initializing = false;

        UpdateModeButtonsVisual();
        UpdateSliderValueLabels();
        UpdateDimensionPlanPreview();

        // Load any files/folders passed via Windows shell / command line
        var initialPaths = startupArgs.Where(p => !p.StartsWith("--", StringComparison.Ordinal)).ToArray();
        if (initialPaths.Length > 0)
        {
            AddPathsToQueue(initialPaths);
        }

        // Run asynchronous FP16 hardware calibration in background if user hasn't manually overridden it
        _ = RunStartupFp16CalibrationAsync();
    }

    private void PopulateDropdownsAndHardware()
    {
        var gpus = NativeEngine.EnumerateGpus();
        var (threads, _) = NativeEngine.GetCpuThreads();
        var (availMb, _) = NativeEngine.GetSystemMemoryMb();

        _deviceOptions.Clear();
        if (gpus.Count > 0)
        {
            var primary = gpus.FirstOrDefault(g => g.IsRecommended) ?? gpus[0];
            TxtHardwareBadge.Text = $"Vulkan GPU Ready • {primary.Name} ({primary.HeapBudgetMb} MB VRAM) + {threads}-Thread CPU";
            HardwarePillBorder.Background = new SolidColorBrush(Color.FromRgb(15, 41, 34));
            HardwarePillBorder.BorderBrush = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            HardwareDot.Fill = new SolidColorBrush(Color.FromRgb(16, 185, 129));

            _deviceOptions.Add(new ComputeDeviceOption(false, -1, $"Auto — Best GPU ({primary.Name}) with Smart CPU Fallback"));
            foreach (var g in gpus)
            {
                _deviceOptions.Add(new ComputeDeviceOption(false, g.Index, $"[GPU {g.Index}] {g.Name} ({g.TypeLabel}, {g.HeapBudgetMb} MB)"));
            }
            _deviceOptions.Add(new ComputeDeviceOption(true, -1, $"Multi-Core CPU Only ({threads} Threads — FP32 High Precision)"));
        }
        else
        {
            // PC WITHOUT A GRAPHICS CARD: highlight full Multi-Core CPU readiness!
            TxtHardwareBadge.Text = $"Multi-Core CPU Engine Ready • {threads} Threads • {availMb:N0} MB RAM (No GPU Required)";
            HardwarePillBorder.Background = new SolidColorBrush(Color.FromRgb(30, 27, 75));
            HardwarePillBorder.BorderBrush = new SolidColorBrush(Color.FromRgb(129, 140, 248));
            HardwareDot.Fill = new SolidColorBrush(Color.FromRgb(56, 189, 248));

            _deviceOptions.Add(new ComputeDeviceOption(true, -1, $"Multi-Core CPU Precision Engine ({threads} Threads — No Graphics Card Needed)"));
        }

        CmbComputeDevice.ItemsSource = _deviceOptions.Select(d => d.Label).ToList();
        CmbComputeDevice.SelectedIndex = 0;

        CmbCpuThreads.ItemsSource = ThreadOption.All.Select(t => t.Label).ToList();
        CmbCpuThreads.SelectedIndex = 0;

        CmbDpi.ItemsSource = DpiOption.All.Select(d => d.Label).ToList();
        CmbDpi.SelectedIndex = 2; // 300 DPI

        RebuildLocalizedCombos();
    }

    private void RebuildLocalizedCombos()
    {
        bool isFa = Strings.Instance.IsPersian;

        int modelIdx = Math.Max(0, CmbModel.SelectedIndex);
        CmbModel.ItemsSource = SrModelOption.All.Select(m => m.GetLabel(isFa)).ToList();
        CmbModel.SelectedIndex = Math.Clamp(modelIdx, 0, SrModelOption.All.Count - 1);

        int targetIdx = Math.Max(0, CmbTargetRes.SelectedIndex);
        CmbTargetRes.ItemsSource = TargetPresetOption.All.Select(t => isFa ? t.LabelFa : t.LabelEn).ToList();
        CmbTargetRes.SelectedIndex = Math.Clamp(targetIdx, 0, TargetPresetOption.All.Count - 1);

        int tileIdx = Math.Max(0, CmbTileSize.SelectedIndex);
        CmbTileSize.ItemsSource = TileOption.All.Select(t => isFa ? t.LabelFa : t.LabelEn).ToList();
        CmbTileSize.SelectedIndex = Math.Clamp(tileIdx, 0, TileOption.All.Count - 1);

        UpdateOutputFolderDisplay();
    }

    private void ApplyPreferencesToUi()
    {
        Strings.Instance.IsPersian = string.Equals(_prefs.Language, "fa", StringComparison.OrdinalIgnoreCase);
        RebuildLocalizedCombos();

        _activeMode = Enum.IsDefined(typeof(EnhanceModeKind), _prefs.EnhanceMode)
            ? (EnhanceModeKind)_prefs.EnhanceMode
            : EnhanceModeKind.Balanced;

        int modelIndex = SrModelOption.All
            .Select((m, idx) => (m, idx))
            .FirstOrDefault(x => x.m.Slug == _prefs.ModelSlug).idx;
        CmbModel.SelectedIndex = modelIndex;

        int targetIndex = TargetPresetOption.All
            .Select((t, idx) => (t, idx))
            .FirstOrDefault(x => x.t.LongSide == _prefs.TargetPresetLongSide).idx;
        if (_prefs.TargetPresetLongSide != 0 && targetIndex == 0) targetIndex = 2; // 4K default
        CmbTargetRes.SelectedIndex = targetIndex;
        TxtCustomLongSide.Text = Math.Clamp(_prefs.CustomLongSide, 256, 32768).ToString();
        PanelCustomLongSide.Visibility = TargetPresetOption.All[targetIndex].IsCustom
            ? Visibility.Visible
            : Visibility.Collapsed;

        ChkEnhanceFaces.IsChecked = _prefs.EnhanceFaces;
        SldFaceStrength.Value = Math.Clamp(_prefs.FaceStrength, 0, 100);
        ChkLightAi.IsChecked = _prefs.LightAi;
        SldLightAiStrength.Value = Math.Clamp(_prefs.LightAiStrength, 0, 100);
        SldDenoise.Value = Math.Clamp(_prefs.DenoiseStrength, 0, 100);
        SldUltraIntensity.Value = Math.Clamp(_prefs.UltraIntensity, 0, 100);
        SldUltraSize.Value = Math.Clamp(_prefs.UltraSize, 0, 100);
        SldSharpness.Value = Math.Clamp(_prefs.Sharpness, 0, 100);

        ChkMaxQuality.IsChecked = _prefs.MaxQuality;
        ChkTtaX8.IsChecked = _prefs.TtaX8;
        ChkDisableFp16.IsChecked = _prefs.DisableFp16;

        int devIdx = _deviceOptions.FindIndex(d => d.ForceCpu == _prefs.ForceCpu && d.GpuIndex == _prefs.GpuIndex);
        CmbComputeDevice.SelectedIndex = devIdx >= 0 ? devIdx : 0;

        int threadIdx = ThreadOption.All
            .Select((t, idx) => (t, idx))
            .FirstOrDefault(x => x.t.Threads == _prefs.NumThreads).idx;
        CmbCpuThreads.SelectedIndex = threadIdx;

        int tileIdx = TileOption.All
            .Select((t, idx) => (t, idx))
            .FirstOrDefault(x => x.t.TilePx == _prefs.TileSize).idx;
        CmbTileSize.SelectedIndex = tileIdx;

        CmbOutputFormat.SelectedIndex = _prefs.OutputFormat == 1 ? 1 : 0;
        SldJpegQuality.Value = Math.Clamp(_prefs.JpegQuality, 90, 100);

        int dpiIdx = DpiOption.All
            .Select((d, idx) => (d, idx))
            .FirstOrDefault(x => x.d.Dpi == _prefs.Dpi).idx;
        CmbDpi.SelectedIndex = (_prefs.Dpi != 72 && dpiIdx == 0) ? 2 : dpiIdx;

        UpdateOutputFolderDisplay();
    }

    private void SaveUiToPreferences()
    {
        if (_initializing) return;
        _prefs.Language = Strings.Instance.IsPersian ? "fa" : "en";
        _prefs.EnhanceMode = (int)_activeMode;
        _prefs.ModelSlug = GetSelectedModel().Slug;
        var targetOpt = TargetPresetOption.All[Math.Clamp(CmbTargetRes.SelectedIndex, 0, TargetPresetOption.All.Count - 1)];
        _prefs.TargetPresetLongSide = targetOpt.LongSide;
        if (int.TryParse(TxtCustomLongSide.Text, out int customPx))
        {
            _prefs.CustomLongSide = Math.Clamp(customPx, 256, 32768);
        }
        _prefs.EnhanceFaces = ChkEnhanceFaces.IsChecked == true;
        _prefs.FaceStrength = (int)Math.Round(SldFaceStrength.Value);
        _prefs.LightAi = ChkLightAi.IsChecked == true;
        _prefs.LightAiStrength = (int)Math.Round(SldLightAiStrength.Value);
        _prefs.DenoiseStrength = (int)Math.Round(SldDenoise.Value);
        _prefs.UltraIntensity = (int)Math.Round(SldUltraIntensity.Value);
        _prefs.UltraSize = (int)Math.Round(SldUltraSize.Value);
        _prefs.Sharpness = (int)Math.Round(SldSharpness.Value);
        _prefs.MaxQuality = ChkMaxQuality.IsChecked == true;
        _prefs.TtaX8 = ChkTtaX8.IsChecked == true;

        var dev = GetSelectedComputeDevice();
        _prefs.ForceCpu = dev.ForceCpu;
        _prefs.GpuIndex = dev.GpuIndex;
        _prefs.NumThreads = ThreadOption.All[Math.Clamp(CmbCpuThreads.SelectedIndex, 0, ThreadOption.All.Count - 1)].Threads;
        _prefs.TileSize = TileOption.All[Math.Clamp(CmbTileSize.SelectedIndex, 0, TileOption.All.Count - 1)].TilePx;
        _prefs.DisableFp16 = ChkDisableFp16.IsChecked == true;
        _prefs.OutputFormat = CmbOutputFormat.SelectedIndex == 1 ? 1 : 0;
        _prefs.JpegQuality = (int)Math.Round(SldJpegQuality.Value);
        _prefs.Dpi = DpiOption.All[Math.Clamp(CmbDpi.SelectedIndex, 0, DpiOption.All.Count - 1)].Dpi;
        _prefs.Save();
    }

    private async Task RunStartupFp16CalibrationAsync()
    {
        try
        {
            var (vkAvail, recDisableFp16, _) = await Task.Run(() => NativeEngine.CalibrateFp16Policy(_modelsDir));
            if (vkAvail && !_prefs.DisableFp16UserSet)
            {
                _initializing = true;
                ChkDisableFp16.IsChecked = recDisableFp16;
                _initializing = false;
                SaveUiToPreferences();
            }
        }
        catch
        {
        }
    }

    // ==================== LANGUAGE & DIAGNOSTICS ====================

    private void BtnToggleLanguage_Click(object sender, RoutedEventArgs e)
    {
        _initializing = true;
        Strings.Instance.IsPersian = !Strings.Instance.IsPersian;
        RebuildLocalizedCombos();
        _initializing = false;
        UpdateModeButtonsVisual();
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void BtnDiagnostics_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new DiagnosticsWindow(_modelsDir) { Owner = this };
        dlg.ShowDialog();
    }

    // ==================== 7 STUDIO MODES & INSPECTOR EVENTS ====================

    private void BtnModePreset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button btn || btn.Tag is not string tagStr || !int.TryParse(tagStr, out int code))
            return;

        _activeMode = (EnhanceModeKind)Math.Clamp(code, 0, 6);
        var preset = ModePresetSpec.Get(_activeMode);

        _initializing = true;
        int mIdx = SrModelOption.All
            .Select((m, idx) => (m, idx))
            .FirstOrDefault(x => x.m.Slug == preset.DefaultModelSlug).idx;
        CmbModel.SelectedIndex = mIdx;
        ChkEnhanceFaces.IsChecked = preset.EnhanceFaces;
        SldFaceStrength.Value = preset.FaceStrengthPct;
        ChkLightAi.IsChecked = preset.LightAi;
        SldLightAiStrength.Value = preset.LightAiStrengthPct;
        SldDenoise.Value = preset.DenoisePct;
        SldUltraIntensity.Value = preset.UltraIntensityPct;
        SldUltraSize.Value = preset.UltraSizePct;
        SldSharpness.Value = preset.SharpnessPct;
        _initializing = false;

        UpdateModeButtonsVisual();
        UpdateSliderValueLabels();
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void UpdateModeButtonsVisual()
    {
        Button[] buttons =
        [
            BtnModePrime, BtnModeGentle, BtnModeBalanced, BtnModeStrong,
            BtnModeUltra, BtnModeDigitalArt, BtnModeOldPhoto
        ];
        for (int i = 0; i < buttons.Length; i++)
        {
            bool active = i == (int)_activeMode;
            buttons[i].Style = (Style)FindResource(active ? "PrimaryStudioButton" : "SecondaryStudioButton");
        }
        CardUltraControls.BorderBrush = new SolidColorBrush(
            _activeMode == EnhanceModeKind.Ultra
                ? Color.FromRgb(129, 140, 248)
                : Color.FromRgb(38, 52, 73)
        );
    }

    private void AnySettingSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsLoaded) return;
        UpdateSliderValueLabels();
        SaveUiToPreferences();
    }

    private void AnyCheck_Changed(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void ChkDisableFp16_Changed(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        _prefs.DisableFp16UserSet = true;
        SaveUiToPreferences();
    }

    private void CmbModel_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void CmbTargetRes_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        var opt = TargetPresetOption.All[Math.Clamp(CmbTargetRes.SelectedIndex, 0, TargetPresetOption.All.Count - 1)];
        PanelCustomLongSide.Visibility = opt.IsCustom ? Visibility.Visible : Visibility.Collapsed;
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void TxtCustomLongSide_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void AnyCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded || _initializing) return;
        UpdateDimensionPlanPreview();
        SaveUiToPreferences();
    }

    private void UpdateSliderValueLabels()
    {
        if (TxtValUltraIntensity == null) return;
        TxtValUltraIntensity.Text = $"{(int)Math.Round(SldUltraIntensity.Value)}%";
        TxtValUltraSize.Text = $"{(int)Math.Round(SldUltraSize.Value)}%";
        TxtValFaceStrength.Text = $"{(int)Math.Round(SldFaceStrength.Value)}%";
        TxtValLightAi.Text = $"{(int)Math.Round(SldLightAiStrength.Value)}%";
        TxtValDenoise.Text = $"{(int)Math.Round(SldDenoise.Value)}%";
        TxtValSharpness.Text = $"{(int)Math.Round(SldSharpness.Value)}%";
        TxtValJpegQuality.Text = $"{(int)Math.Round(SldJpegQuality.Value)}%";
    }

    // ==================== QUEUE, DRAG-DROP & SAMPLE IMAGE ====================

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop)
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void Window_Drop(object sender, DragEventArgs e)
    {
        if (e.Data.GetDataPresent(DataFormats.FileDrop) &&
            e.Data.GetData(DataFormats.FileDrop) is string[] droppedPaths)
        {
            AddPathsToQueue(droppedPaths);
        }
    }

    private void BtnAddImages_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Title = "Select Images to Enhance",
            Multiselect = true,
            Filter = "Supported Images (*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff)|*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff|All Files (*.*)|*.*"
        };
        if (dlg.ShowDialog(this) == true)
        {
            AddPathsToQueue(dlg.FileNames);
        }
    }

    private void BtnAddFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog
        {
            Title = "Select Folder of Images to Add"
        };
        if (dlg.ShowDialog(this) == true && Directory.Exists(dlg.FolderName))
        {
            AddPathsToQueue([dlg.FolderName]);
        }
    }

    private void BtnLoadSample_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string sampleDir = Path.Combine(Path.GetTempPath(), "EnhancerStudioSamples");
            Directory.CreateDirectory(sampleDir);
            string samplePath = Path.Combine(sampleDir, "Studio_Sample_TestChart_480x320.png");

            int w = 480, h = 320;
            byte[] bgra = new byte[w * h * 4];
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int idx = (y * w + x) * 4;
                    double nx = (double)x / w;
                    double ny = (double)y / h;

                    // Rich sunset sky + geometric architectural grid + fine test rings
                    byte r = (byte)Math.Clamp(30 + nx * 185 + Math.Sin(nx * 24) * 18, 0, 255);
                    byte g = (byte)Math.Clamp(45 + (1 - ny) * 145 + Math.Cos(ny * 20) * 18, 0, 255);
                    byte b = (byte)Math.Clamp(95 + ny * 130, 0, 255);

                    double dx = x - w * 0.5;
                    double dy = y - h * 0.5;
                    double ring = Math.Sqrt(dx * dx + dy * dy);
                    if (Math.Abs(ring % 22 - 11) < 1.6)
                    {
                        r = 245; g = 248; b = 255;
                    }

                    bgra[idx + 0] = b;
                    bgra[idx + 1] = g;
                    bgra[idx + 2] = r;
                    bgra[idx + 3] = 255;
                }
            }

            var wb = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, bgra, w * 4);
            using (var fs = new FileStream(samplePath, FileMode.Create, FileAccess.Write))
            {
                var enc = new PngBitmapEncoder();
                enc.Frames.Add(BitmapFrame.Create(wb));
                enc.Save(fs);
            }

            AddPathsToQueue([samplePath]);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "Sample Generation", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void AddPathsToQueue(IEnumerable<string> rawPaths)
    {
        var filesToAdd = new List<string>();
        foreach (string p in rawPaths)
        {
            if (File.Exists(p) && SupportedExtensions.Contains(Path.GetExtension(p)))
            {
                filesToAdd.Add(Path.GetFullPath(p));
            }
            else if (Directory.Exists(p))
            {
                foreach (string f in Directory.EnumerateFiles(p, "*.*", SearchOption.TopDirectoryOnly))
                {
                    if (SupportedExtensions.Contains(Path.GetExtension(f)))
                    {
                        filesToAdd.Add(Path.GetFullPath(f));
                    }
                }
            }
        }

        QueueImageItem? firstAdded = null;
        foreach (string fullPath in filesToAdd)
        {
            if (_queue.Any(q => string.Equals(q.FilePath, fullPath, StringComparison.OrdinalIgnoreCase)))
                continue;

            int w = 0, h = 0;
            try
            {
                using var fs = new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                var dec = BitmapDecoder.Create(fs, BitmapCreateOptions.DelayCreation, BitmapCacheOption.None);
                w = dec.Frames[0].PixelWidth;
                h = dec.Frames[0].PixelHeight;
            }
            catch
            {
            }

            var item = new QueueImageItem
            {
                FilePath = fullPath,
                SrcWidth = w,
                SrcHeight = h,
                StatusText = Strings.Instance.IsPersian ? "آماده" : "Ready"
            };
            _queue.Add(item);
            firstAdded ??= item;
        }

        TxtQueueCount.Text = _queue.Count.ToString();
        if (LstQueue.SelectedItem == null && firstAdded != null)
        {
            LstQueue.SelectedItem = firstAdded;
        }
    }

    private void BtnClearQueue_Click(object sender, RoutedEventArgs e)
    {
        if (_isProcessing) return;
        _queue.Clear();
        TxtQueueCount.Text = "0";
        EmptyDropOverlay.Visibility = Visibility.Visible;
        PanZoomCanvas.Visibility = Visibility.Collapsed;
        BadgeBefore.Visibility = Visibility.Collapsed;
        BadgeAfter.Visibility = Visibility.Collapsed;
        UpdateDimensionPlanPreview();
    }

    private void BtnChooseOutputFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Choose Custom Output Folder" };
        if (dlg.ShowDialog(this) == true && Directory.Exists(dlg.FolderName))
        {
            _prefs.CustomOutputFolder = dlg.FolderName;
            UpdateOutputFolderDisplay();
            _prefs.Save();
        }
    }

    private void BtnResetOutputFolder_Click(object sender, RoutedEventArgs e)
    {
        _prefs.CustomOutputFolder = string.Empty;
        UpdateOutputFolderDisplay();
        _prefs.Save();
    }

    private void UpdateOutputFolderDisplay()
    {
        if (TxtOutputFolderDisplay == null) return;
        TxtOutputFolderDisplay.Text = string.IsNullOrWhiteSpace(_prefs.CustomOutputFolder)
            ? Strings.Instance.SameAsInputLabel
            : _prefs.CustomOutputFolder;
    }

    private void BtnOpenOutputExplorer_Click(object sender, RoutedEventArgs e)
    {
        var item = LstQueue.SelectedItem as QueueImageItem ?? _queue.FirstOrDefault(q => q.OutputPath != null);
        if (item?.OutputPath != null && File.Exists(item.OutputPath))
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{item.OutputPath}\"") { UseShellExecute = true });
        }
        else if (!string.IsNullOrWhiteSpace(_prefs.CustomOutputFolder) && Directory.Exists(_prefs.CustomOutputFolder))
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{_prefs.CustomOutputFolder}\"") { UseShellExecute = true });
        }
    }

    private void LstQueue_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LstQueue.SelectedItem is not QueueImageItem item) return;
        DisplayQueueItemOnCanvas(item);
        UpdateDimensionPlanPreview();
    }

    // ==================== INTERACTIVE BEFORE/AFTER SPLIT + PAN/ZOOM ====================

    private void DisplayQueueItemOnCanvas(QueueImageItem item)
    {
        try
        {
            var (srcBmp, _, w, h) = NativeEngine.DecodeImageWithExif(item.FilePath);
            item.SrcWidth = w;
            item.SrcHeight = h;

            BitmapSource afterBmp = srcBmp;
            bool hasEnhanced = false;
            if (!string.IsNullOrEmpty(item.OutputPath) && File.Exists(item.OutputPath))
            {
                afterBmp = NativeEngine.LoadDisplayBitmapNoLock(item.OutputPath);
                hasEnhanced = true;
            }

            _stageBaseWidth = hasEnhanced ? afterBmp.PixelWidth : srcBmp.PixelWidth;
            _stageBaseHeight = hasEnhanced ? afterBmp.PixelHeight : srcBmp.PixelHeight;

            CompareStage.Width = _stageBaseWidth;
            CompareStage.Height = _stageBaseHeight;
            ImgBefore.Width = _stageBaseWidth;
            ImgBefore.Height = _stageBaseHeight;
            ImgAfter.Width = _stageBaseWidth;
            ImgAfter.Height = _stageBaseHeight;

            ImgBefore.Source = srcBmp;
            ImgAfter.Source = afterBmp;

            TxtAfterBadgeLabel.Text = hasEnhanced
                ? $"{Strings.Instance.AfterBadge} ({afterBmp.PixelWidth}×{afterBmp.PixelHeight})"
                : (Strings.Instance.IsPersian ? "پیش‌نمایش ورودی" : "ORIGINAL PREVIEW");

            EmptyDropOverlay.Visibility = Visibility.Collapsed;
            PanZoomCanvas.Visibility = Visibility.Visible;
            BadgeBefore.Visibility = Visibility.Visible;
            BadgeAfter.Visibility = Visibility.Visible;

            ApplyFitZoom();
            UpdateSplitGeometry();
        }
        catch (Exception ex)
        {
            TxtStatusHeadline.Text = $"Failed to decode preview: {ex.Message}";
        }
    }

    private void ApplyFitZoom()
    {
        if (_stageBaseWidth <= 0 || _stageBaseHeight <= 0 || ViewportHost.ActualWidth <= 20 || ViewportHost.ActualHeight <= 20)
            return;

        _autoFitZoom = true;
        double pad = 36.0;
        double sx = Math.Max(40, ViewportHost.ActualWidth - pad) / _stageBaseWidth;
        double sy = Math.Max(40, ViewportHost.ActualHeight - pad) / _stageBaseHeight;
        _zoomFactor = Math.Clamp(Math.Min(sx, sy), 0.05, 4.0);

        StageScale.ScaleX = _zoomFactor;
        StageScale.ScaleY = _zoomFactor;
        StageTranslate.X = (ViewportHost.ActualWidth - _stageBaseWidth * _zoomFactor) * 0.5;
        StageTranslate.Y = (ViewportHost.ActualHeight - _stageBaseHeight * _zoomFactor) * 0.5;
        TxtZoomReadout.Text = $"Fit ({(int)Math.Round(_zoomFactor * 100)}%)";
        UpdateSplitGeometry();
    }

    private void SetExplicitZoom(double factor)
    {
        if (_stageBaseWidth <= 0 || _stageBaseHeight <= 0) return;
        _autoFitZoom = false;
        _zoomFactor = Math.Clamp(factor, 0.1, 8.0);
        StageScale.ScaleX = _zoomFactor;
        StageScale.ScaleY = _zoomFactor;
        StageTranslate.X = (ViewportHost.ActualWidth - _stageBaseWidth * _zoomFactor) * 0.5;
        StageTranslate.Y = (ViewportHost.ActualHeight - _stageBaseHeight * _zoomFactor) * 0.5;
        TxtZoomReadout.Text = $"{(int)Math.Round(_zoomFactor * 100)}%";
        UpdateSplitGeometry();
    }

    private void BtnZoomFit_Click(object sender, RoutedEventArgs e) => ApplyFitZoom();
    private void BtnZoom100_Click(object sender, RoutedEventArgs e) => SetExplicitZoom(1.0);
    private void BtnZoom200_Click(object sender, RoutedEventArgs e) => SetExplicitZoom(2.0);
    private void BtnZoom400_Click(object sender, RoutedEventArgs e) => SetExplicitZoom(4.0);

    private void ViewportHost_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_autoFitZoom && PanZoomCanvas.Visibility == Visibility.Visible)
        {
            ApplyFitZoom();
        }
    }

    private void ViewportHost_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (PanZoomCanvas.Visibility != Visibility.Visible || _stageBaseWidth <= 0) return;
        _autoFitZoom = false;

        Point mouseInHost = e.GetPosition(ViewportHost);
        double oldZoom = _zoomFactor;
        double mult = e.Delta > 0 ? 1.18 : (1.0 / 1.18);
        _zoomFactor = Math.Clamp(_zoomFactor * mult, 0.1, 8.0);

        // Zoom anchored smoothly around mouse cursor
        double relX = (mouseInHost.X - StageTranslate.X) / oldZoom;
        double relY = (mouseInHost.Y - StageTranslate.Y) / oldZoom;
        StageScale.ScaleX = _zoomFactor;
        StageScale.ScaleY = _zoomFactor;
        StageTranslate.X = mouseInHost.X - relX * _zoomFactor;
        StageTranslate.Y = mouseInHost.Y - relY * _zoomFactor;
        TxtZoomReadout.Text = $"{(int)Math.Round(_zoomFactor * 100)}%";
        UpdateSplitGeometry();
    }

    private void ViewportHost_MouseDown(object sender, MouseButtonEventArgs e)
    {
        if (PanZoomCanvas.Visibility != Visibility.Visible) return;

        if (e.RightButton == MouseButtonState.Pressed || e.MiddleButton == MouseButtonState.Pressed ||
            (e.LeftButton == MouseButtonState.Pressed && Keyboard.IsKeyDown(Key.Space)))
        {
            _panningCanvas = true;
            _panStartMouse = e.GetPosition(ViewportHost);
            _panStartTranslate = new Point(StageTranslate.X, StageTranslate.Y);
            ViewportHost.CaptureMouse();
            return;
        }

        if (e.LeftButton == MouseButtonState.Pressed)
        {
            _draggingSplit = true;
            ViewportHost.CaptureMouse();
            UpdateSplitFromMouse(e.GetPosition(CompareStage));
        }
    }

    private void ViewportHost_MouseMove(object sender, MouseEventArgs e)
    {
        if (_panningCanvas)
        {
            Point cur = e.GetPosition(ViewportHost);
            StageTranslate.X = _panStartTranslate.X + (cur.X - _panStartMouse.X);
            StageTranslate.Y = _panStartTranslate.Y + (cur.Y - _panStartMouse.Y);
            return;
        }

        if (_draggingSplit)
        {
            UpdateSplitFromMouse(e.GetPosition(CompareStage));
        }
    }

    private void ViewportHost_MouseUp(object sender, MouseButtonEventArgs e)
    {
        if (_panningCanvas || _draggingSplit)
        {
            _panningCanvas = false;
            _draggingSplit = false;
            ViewportHost.ReleaseMouseCapture();
        }
    }

    private void UpdateSplitFromMouse(Point posInStage)
    {
        if (_stageBaseWidth <= 0) return;
        _splitRatio = Math.Clamp(posInStage.X / _stageBaseWidth, 0.0, 1.0);
        SldSplit.Value = _splitRatio * 100.0;
        UpdateSplitGeometry();
    }

    private void SldSplit_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!IsLoaded) return;
        _splitRatio = Math.Clamp(SldSplit.Value / 100.0, 0.0, 1.0);
        UpdateSplitGeometry();
    }

    private void UpdateSplitGeometry()
    {
        if (_stageBaseWidth <= 0 || _stageBaseHeight <= 0) return;
        double splitX = _stageBaseWidth * _splitRatio;
        ImgBefore.Clip = new RectangleGeometry(new Rect(0, 0, splitX, _stageBaseHeight));

        double invZoom = 1.0 / Math.Max(0.1, _zoomFactor);
        SplitLine.StrokeThickness = 2.0 * invZoom;
        SplitLine.X1 = splitX;
        SplitLine.X2 = splitX;
        SplitLine.Y1 = 0;
        SplitLine.Y2 = _stageBaseHeight;

        double knobSize = 34.0 * invZoom;
        SplitHandleKnob.Width = knobSize;
        SplitHandleKnob.Height = knobSize;
        SplitHandleKnob.CornerRadius = new CornerRadius(knobSize * 0.5);
        Canvas.SetLeft(SplitHandleKnob, splitX - knobSize * 0.5);
        Canvas.SetTop(SplitHandleKnob, _stageBaseHeight * 0.5 - knobSize * 0.5);
    }

    // ==================== LIVE DIMENSION PLAN PREVIEW ====================

    private void UpdateDimensionPlanPreview()
    {
        if (TxtDimensionPlan == null) return;
        var item = LstQueue.SelectedItem as QueueImageItem ?? _queue.FirstOrDefault();
        if (item == null || item.SrcWidth <= 0 || item.SrcHeight <= 0)
        {
            TxtDimensionPlan.Text = Strings.Instance.IsPersian
                ? "یک تصویر اضافه کنید تا ابعاد دقیق خروجی هوش مصنوعی و سایز چاپ (DPI) نمایش داده شود."
                : "Load or drop an image to preview exact AI Super-Resolution & print dimensions.";
            return;
        }

        var model = GetSelectedModel();
        int targetLong = GetSelectedTargetLongSide();
        bool maxQ = ChkMaxQuality.IsChecked == true;
        int dpi = DpiOption.All[Math.Clamp(CmbDpi.SelectedIndex, 0, DpiOption.All.Count - 1)].Dpi;

        DimensionPlanInfo plan = NativeEngine.ComputeDimensions(
            item.SrcWidth,
            item.SrcHeight,
            model.Scale,
            targetLong,
            maxQ
        );

        double printWIn = (double)plan.FinalWidth / dpi;
        double printHIn = (double)plan.FinalHeight / dpi;

        TxtDimensionPlan.Text =
            $"Input: {plan.SrcWidth}×{plan.SrcHeight}  →  AI Pass ({model.Scale}×): {plan.ModelOutWidth}×{plan.ModelOutHeight}  →  " +
            $"Final Output: {plan.FinalWidth}×{plan.FinalHeight} px  •  {dpi} DPI ({printWIn:F1}\" × {printHIn:F1}\" Print)";
    }

    // ==================== ENHANCE EXECUTION (SINGLE & BATCH) ====================

    private async void BtnEnhanceSelected_Click(object sender, RoutedEventArgs e)
    {
        if (_isProcessing) return;
        var item = LstQueue.SelectedItem as QueueImageItem ?? _queue.FirstOrDefault();
        if (item == null)
        {
            BtnAddImages_Click(sender, e);
            return;
        }
        await ProcessItemsAsync([item]);
    }

    private async void BtnEnhanceBatch_Click(object sender, RoutedEventArgs e)
    {
        if (_isProcessing) return;
        if (_queue.Count == 0)
        {
            BtnAddImages_Click(sender, e);
            return;
        }
        await ProcessItemsAsync(_queue.ToList());
    }

    private void BtnCancelJob_Click(object sender, RoutedEventArgs e)
    {
        _batchCancelRequested = true;
        NativeEngine.RequestCancel();
        TxtStatusHeadline.Text = "Cancelling active enhancement pass...";
    }

    private async Task ProcessItemsAsync(IReadOnlyList<QueueImageItem> items)
    {
        _isProcessing = true;
        _batchCancelRequested = false;
        BtnEnhanceSelected.IsEnabled = false;
        BtnEnhanceBatch.IsEnabled = false;
        BtnCancelJob.IsEnabled = true;

        var reqTemplate = SnapshotRequestFromUi();
        int doneCount = 0;

        try
        {
            foreach (var item in items)
            {
                if (_batchCancelRequested) break;

                LstQueue.SelectedItem = item;
                item.IsProcessing = true;
                item.IsFailed = false;
                item.StatusText = "Decoding...";
                PrgEnhance.Value = 1;

                string outPath = BuildOutputFilePath(item.FilePath, reqTemplate.OutputFormat == 1 ? ".png" : ".jpg");
                var jobReq = reqTemplate with { OutputFilePath = outPath };

                var sw = Stopwatch.StartNew();
                UpscaleJobOutcome outcome;

                try
                {
                    var (_, rgbaPixels, srcW, srcH) = NativeEngine.DecodeImageWithExif(item.FilePath);
                    item.SrcWidth = srcW;
                    item.SrcHeight = srcH;

                    TxtStatusHeadline.Text = $"Enhancing {item.FileName} ({doneCount + 1}/{items.Count})...";

                    outcome = await Task.Run(() =>
                        NativeEngine.RunUpscaleRgba(rgbaPixels, srcW, srcH, jobReq, (pct, backend, tileSz) =>
                        {
                            Dispatcher.BeginInvoke(() =>
                            {
                                PrgEnhance.Value = pct;
                                item.StatusText = $"{pct}%";
                                TxtTelemetryDetail.Text = string.IsNullOrWhiteSpace(backend)
                                    ? $"Tile {tileSz} px"
                                    : $"{backend} • Tile {tileSz} px";
                            });
                        })
                    );
                }
                catch (Exception ex)
                {
                    outcome = new UpscaleJobOutcome(-99, 0, 0, false, 0, 0, 0, "", "", ex.Message);
                }

                sw.Stop();
                item.IsProcessing = false;

                if (outcome.IsSuccess)
                {
                    doneCount++;
                    item.IsDone = true;
                    item.OutputPath = outPath;
                    item.OutWidth = outcome.OutWidth;
                    item.OutHeight = outcome.OutHeight;
                    item.StatusText = $"Done ({sw.Elapsed.TotalSeconds:F1}s)";
                    PrgEnhance.Value = 100;

                    string faceInfo = outcome.FacesRestored > 0 ? $" • {outcome.FacesRestored} Face(s) Restored" : "";
                    TxtStatusHeadline.Text =
                        $"Saved {Path.GetFileName(outPath)} ({outcome.OutWidth}×{outcome.OutHeight}, {jobReq.Dpi} DPI) in {sw.Elapsed.TotalSeconds:F1}s{faceInfo}";
                    TxtTelemetryDetail.Text = outcome.BackendUsed;

                    DisplayQueueItemOnCanvas(item);
                }
                else if (outcome.IsCancelled || _batchCancelRequested)
                {
                    item.StatusText = "Cancelled";
                    TxtStatusHeadline.Text = "Enhancement cancelled by user.";
                    PrgEnhance.Value = 0;
                    break;
                }
                else
                {
                    item.IsFailed = true;
                    item.StatusText = "Failed";
                    TxtStatusHeadline.Text = $"Failed on {item.FileName}: {outcome.ErrorDetail}";
                    MessageBox.Show(
                        this,
                        $"Enhancement failed for {item.FileName}:\n\n{outcome.ErrorDetail}",
                        "Enhancer v3.0 Engine",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning
                    );
                }
            }
        }
        finally
        {
            _isProcessing = false;
            BtnEnhanceSelected.IsEnabled = true;
            BtnEnhanceBatch.IsEnabled = true;
            BtnCancelJob.IsEnabled = false;
        }
    }

    private UpscaleJobRequest SnapshotRequestFromUi()
    {
        var model = GetSelectedModel();
        var dev = GetSelectedComputeDevice();
        int threads = ThreadOption.All[Math.Clamp(CmbCpuThreads.SelectedIndex, 0, ThreadOption.All.Count - 1)].Threads;
        int tile = TileOption.All[Math.Clamp(CmbTileSize.SelectedIndex, 0, TileOption.All.Count - 1)].TilePx;
        int dpi = DpiOption.All[Math.Clamp(CmbDpi.SelectedIndex, 0, DpiOption.All.Count - 1)].Dpi;
        int fmt = CmbOutputFormat.SelectedIndex == 1 ? 1 : 0;

        return new UpscaleJobRequest(
            ModelsDirectory: _modelsDir,
            ParamRelativePath: model.ParamRel,
            BinRelativePath: model.BinRel,
            OutputFilePath: string.Empty,
            ModelScale: model.Scale,
            TargetLongSide: GetSelectedTargetLongSide(),
            TileSize: tile,
            DisableFp16: ChkDisableFp16.IsChecked == true,
            MaxQuality: ChkMaxQuality.IsChecked == true,
            TtaX8: ChkTtaX8.IsChecked == true,
            SafeMode: false,
            ForceCpu: dev.ForceCpu,
            GpuIndex: dev.GpuIndex,
            NumThreads: threads,
            Sharpness: (int)Math.Round(SldSharpness.Value),
            EnhanceMode: (int)_activeMode,
            EnhanceFaces: ChkEnhanceFaces.IsChecked == true,
            FaceStrength: (float)(SldFaceStrength.Value / 100.0),
            LightAi: ChkLightAi.IsChecked == true,
            LightAiStrength: (float)(SldLightAiStrength.Value / 100.0),
            DenoiseStrength: (float)(SldDenoise.Value / 100.0),
            UltraIntensity: (float)(SldUltraIntensity.Value / 100.0),
            UltraSize: (float)(SldUltraSize.Value / 100.0),
            Dpi: dpi,
            OutputFormat: fmt,
            JpegQuality: (int)Math.Round(SldJpegQuality.Value)
        );
    }

    private string BuildOutputFilePath(string inputPath, string ext)
    {
        string folder = !string.IsNullOrWhiteSpace(_prefs.CustomOutputFolder) && Directory.Exists(_prefs.CustomOutputFolder)
            ? _prefs.CustomOutputFolder
            : (Path.GetDirectoryName(inputPath) ?? Environment.GetFolderPath(Environment.SpecialFolder.MyPictures));

        Directory.CreateDirectory(folder);
        string stem = Path.GetFileNameWithoutExtension(inputPath);
        string modeTag = _activeMode.ToString();
        string candidate = Path.Combine(folder, $"{stem}_Enhancer_v3_{modeTag}{ext}");
        int counter = 2;
        while (File.Exists(candidate))
        {
            candidate = Path.Combine(folder, $"{stem}_Enhancer_v3_{modeTag}_{counter}{ext}");
            counter++;
        }
        return candidate;
    }

    private SrModelOption GetSelectedModel() =>
        SrModelOption.All[Math.Clamp(CmbModel.SelectedIndex, 0, SrModelOption.All.Count - 1)];

    private ComputeDeviceOption GetSelectedComputeDevice() =>
        _deviceOptions[Math.Clamp(CmbComputeDevice.SelectedIndex, 0, _deviceOptions.Count - 1)];

    private int GetSelectedTargetLongSide()
    {
        var opt = TargetPresetOption.All[Math.Clamp(CmbTargetRes.SelectedIndex, 0, TargetPresetOption.All.Count - 1)];
        if (!opt.IsCustom) return opt.LongSide;
        return int.TryParse(TxtCustomLongSide.Text, out int px) ? Math.Clamp(px, 256, 32768) : 4096;
    }
}
