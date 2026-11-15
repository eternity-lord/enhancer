using System.Collections.ObjectModel;
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

namespace Enhancer.Desktop.Lab;

public partial class LabWindow : Window
{
    private static readonly HashSet<string> SupportedExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"
    };

    private readonly ObservableCollection<string> _presetLabels = [];
    private readonly Stack<LabAdjustments> _undo = new();
    private readonly Stack<LabAdjustments> _redo = new();
    private readonly string? _initialImagePath;
    private string? _sourceImagePath;
    private CancellationTokenSource? _renderCancellation;
    private BitmapSource? _original;
    private BitmapSource? _current;
    private LabAdjustments _adjustments = new();
    private bool _suppressChanges;
    private bool _showOriginal;
    private int _renderVersion;

    public LabWindow(string? imagePath = null)
    {
        InitializeComponent();
        _initialImagePath = imagePath;
        CmbPreset.ItemsSource = _presetLabels;
        foreach (var preset in LabPresetCatalog.BuiltIn)
        {
            _presetLabels.Add(preset.GetName(Strings.Instance.IsPersian));
        }
        CmbPreset.SelectedIndex = -1;

        if (!string.IsNullOrWhiteSpace(imagePath))
        {
            LoadImage(imagePath);
        }
        else
        {
            UpdateValueLabels();
        }
    }

    private void LoadImage(string path)
    {
        try
        {
            if (!File.Exists(path))
            {
                throw new FileNotFoundException("Image file not found.", path);
            }
            var decoded = NativeEngine.DecodeImageWithExif(path);
            _sourceImagePath = path;
            _original = decoded.PreviewBitmap;
            _current = _original;
            _adjustments = new LabAdjustments();
            _undo.Clear();
            _redo.Clear();
            _showOriginal = false;
            TxtImageName.Text = Path.GetFileName(path);
            ImgPreview.Source = _current;
            ImgPreview.Width = double.NaN;
            ImgPreview.Height = double.NaN;
            ImgPreview.LayoutTransform = Transform.Identity;
            TxtPreviewMode.Text = "EDITED PREVIEW";
            UpdateValueLabels();
            UpdateHistogram();
            ApplyFitToPreview();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not open the image:\n\n{ex.Message}", "Enhancer Lab", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void LabWindow_Loaded(object sender, RoutedEventArgs e)
    {
        ApplyFitToPreview();
    }

    private void BtnOpenImage_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Images|*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff|All files|*.*",
            Multiselect = false,
            Title = "Open image in Enhancer Lab"
        };
        if (dialog.ShowDialog(this) == true)
        {
            LoadImage(dialog.FileName);
        }
    }

    private async void BtnAiPortrait_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(_sourceImagePath) || !File.Exists(_sourceImagePath))
        {
            BtnOpenImage_Click(sender, e);
            if (string.IsNullOrWhiteSpace(_sourceImagePath) || !File.Exists(_sourceImagePath)) return;
        }

        string outputPath = Path.Combine(Path.GetTempPath(), $"enhancer_lab_ai_{Guid.NewGuid():N}.jpg");
        try
        {
            TxtPreviewMode.Text = "RUNNING OFFLINE AI PORTRAIT RESTORE...";
            var decoded = NativeEngine.DecodeImageWithExif(_sourceImagePath!);
            var request = new UpscaleJobRequest(
                ModelsDirectory: NativeEngine.LocateModelsDirectory(),
                ParamRelativePath: "models/lsdir-photo-x4.param",
                BinRelativePath: "models/lsdir-photo-x4.bin",
                OutputFilePath: outputPath,
                ModelScale: 4,
                TargetLongSide: 0,
                TileSize: 128,
                DisableFp16: false,
                MaxQuality: true,
                TtaX8: false,
                SafeMode: false,
                ForceCpu: false,
                GpuIndex: -1,
                NumThreads: 0,
                Sharpness: 10,
                EnhanceMode: (int)EnhanceModeKind.Ultra,
                EnhanceFaces: true,
                FaceStrength: 0.45f,
                LightAi: true,
                LightAiStrength: 0.18f,
                DenoiseStrength: 0.08f,
                UltraIntensity: 0.45f,
                UltraSize: 0.12f,
                Dpi: 300,
                OutputFormat: 0,
                JpegQuality: 100
            );

            UpscaleJobOutcome outcome = await Task.Run(() =>
                NativeEngine.RunUpscaleRgba(decoded.RgbaPixels, decoded.Width, decoded.Height, request,
                    (pct, backend, tile) => Dispatcher.BeginInvoke(() =>
                        TxtPreviewMode.Text = $"AI PORTRAIT {pct}% • {backend} • tile {tile}")));

            if (!outcome.IsSuccess || !File.Exists(outputPath))
            {
                throw new InvalidOperationException(string.IsNullOrWhiteSpace(outcome.ErrorDetail)
                    ? "The native portrait pipeline did not produce an output."
                    : outcome.ErrorDetail);
            }

            BitmapSource aiBase = NativeEngine.LoadDisplayBitmapNoLock(outputPath);
            _original = aiBase;
            _current = aiBase;
            _adjustments = new LabAdjustments();
            _undo.Clear();
            _redo.Clear();
            _showOriginal = false;
            _suppressChanges = true;
            try
            {
                CmbPreset.SelectedIndex = -1;
                SetAdjustments(new LabAdjustments());
            }
            finally
            {
                _suppressChanges = false;
            }
            TxtImageName.Text = $"{Path.GetFileName(_sourceImagePath)} • AI 4× Portrait";
            ImgPreview.Source = _current;
            TxtPreviewMode.Text = $"AI PORTRAIT READY • {outcome.FacesRestored} face(s) restored";
            UpdateHistogram();
            ApplyFitToPreview();
        }
        catch (Exception ex)
        {
            TxtPreviewMode.Text = "AI PORTRAIT FAILED";
            MessageBox.Show(this, $"AI portrait restore failed:\n\n{ex.Message}", "Enhancer Lab", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            try
            {
                if (File.Exists(outputPath)) File.Delete(outputPath);
            }
            catch
            {
            }
        }
    }

    private void CmbPreset_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressChanges || CmbPreset.SelectedIndex < 0 || CmbPreset.SelectedIndex >= LabPresetCatalog.BuiltIn.Count)
        {
            return;
        }
        PushUndo();
        SetAdjustments(LabPresetCatalog.BuiltIn[CmbPreset.SelectedIndex].Adjustments.Clone());
    }

    private void SliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_suppressChanges || !IsLoaded) return;
        PushUndo();
        _adjustments = ReadAdjustmentsFromUi();
        CmbPreset.SelectedIndex = -1;
        UpdateValueLabels();
        ScheduleRender();
    }

    private void CheckChanged(object sender, RoutedEventArgs e)
    {
        if (_suppressChanges || !IsLoaded) return;
        PushUndo();
        _adjustments = ReadAdjustmentsFromUi();
        CmbPreset.SelectedIndex = -1;
        ScheduleRender();
    }

    private void PushUndo()
    {
        if (_original == null) return;
        if (_undo.Count == 0 || !_undo.Peek().NearlyEquals(_adjustments))
        {
            _undo.Push(_adjustments.Clone());
            _redo.Clear();
        }
    }

    private void SetAdjustments(LabAdjustments value)
    {
        _adjustments = value.Clone();
        _suppressChanges = true;
        try
        {
            SldExposure.Value = _adjustments.Exposure;
            SldContrast.Value = _adjustments.Contrast;
            SldHighlights.Value = _adjustments.Highlights;
            SldShadows.Value = _adjustments.Shadows;
            SldBlackPoint.Value = _adjustments.BlackPoint;
            SldWhitePoint.Value = _adjustments.WhitePoint;
            SldTemperature.Value = _adjustments.Temperature;
            SldTint.Value = _adjustments.Tint;
            SldSaturation.Value = _adjustments.Saturation;
            SldVibrance.Value = _adjustments.Vibrance;
            SldClarity.Value = _adjustments.Clarity;
            SldSkinSmoothing.Value = _adjustments.SkinSmoothing;
            SldFaceLight.Value = _adjustments.FaceLight;
            SldEyeDetail.Value = _adjustments.EyeDetail;
            SldTeethWhiten.Value = _adjustments.TeethWhiten;
            SldSharpening.Value = _adjustments.Sharpening;
            SldVignette.Value = _adjustments.Vignette;
            SldGrain.Value = _adjustments.Grain;
            SldFade.Value = _adjustments.Fade;
            ChkMonochrome.IsChecked = _adjustments.Monochrome;
        }
        finally
        {
            _suppressChanges = false;
        }
        UpdateValueLabels();
        ScheduleRender();
    }

    private LabAdjustments ReadAdjustmentsFromUi() => new()
    {
        Exposure = SldExposure.Value,
        Contrast = SldContrast.Value,
        Highlights = SldHighlights.Value,
        Shadows = SldShadows.Value,
        BlackPoint = SldBlackPoint.Value,
        WhitePoint = SldWhitePoint.Value,
        Temperature = SldTemperature.Value,
        Tint = SldTint.Value,
        Saturation = SldSaturation.Value,
        Vibrance = SldVibrance.Value,
        Clarity = SldClarity.Value,
        SkinSmoothing = SldSkinSmoothing.Value,
        FaceLight = SldFaceLight.Value,
        EyeDetail = SldEyeDetail.Value,
        TeethWhiten = SldTeethWhiten.Value,
        Sharpening = SldSharpening.Value,
        Vignette = SldVignette.Value,
        Grain = SldGrain.Value,
        Fade = SldFade.Value,
        Monochrome = ChkMonochrome.IsChecked == true
    };

    private void UpdateValueLabels()
    {
        if (TxtExposure == null) return;
        TxtExposure.Text = Signed(SldExposure.Value, " EV");
        TxtContrast.Text = Percent(SldContrast.Value);
        TxtHighlights.Text = Percent(SldHighlights.Value);
        TxtShadows.Text = Percent(SldShadows.Value);
        TxtBlackPoint.Text = Percent(SldBlackPoint.Value);
        TxtWhitePoint.Text = Percent(SldWhitePoint.Value);
        TxtTemperature.Text = Percent(SldTemperature.Value);
        TxtTint.Text = Percent(SldTint.Value);
        TxtSaturation.Text = Percent(SldSaturation.Value);
        TxtVibrance.Text = Percent(SldVibrance.Value);
        TxtClarity.Text = Percent(SldClarity.Value);
        TxtSkinSmoothing.Text = Percent(SldSkinSmoothing.Value);
        TxtFaceLight.Text = Percent(SldFaceLight.Value);
        TxtEyeDetail.Text = Percent(SldEyeDetail.Value);
        TxtTeethWhiten.Text = Percent(SldTeethWhiten.Value);
        TxtSharpening.Text = Percent(SldSharpening.Value);
        TxtVignette.Text = Percent(SldVignette.Value);
        TxtGrain.Text = Percent(SldGrain.Value);
        TxtFade.Text = Percent(SldFade.Value);
    }

    private static string Signed(double value, string suffix = "") =>
        $"{(value >= 0 ? "+" : "")}{value:0.00}{suffix}";

    private static string Percent(double value) =>
        $"{(value >= 0 ? "+" : "")}{value * 100:0}%";

    private void ScheduleRender()
    {
        if (_original == null) return;
        _renderCancellation?.Cancel();
        _renderCancellation?.Dispose();
        _renderCancellation = new CancellationTokenSource();
        int version = ++_renderVersion;
        CancellationToken token = _renderCancellation.Token;
        LabAdjustments snapshot = _adjustments.Clone();
        BitmapSource original = _original;

        _ = RenderAsync(version, original, snapshot, token);
    }

    private async Task RenderAsync(int version, BitmapSource original, LabAdjustments snapshot, CancellationToken token)
    {
        try
        {
            BitmapSource rendered = await Task.Run(() => LabImageProcessor.Apply(original, snapshot, token), token);
            if (token.IsCancellationRequested || version != _renderVersion) return;
            _current = rendered;
            ImgPreview.Source = _showOriginal ? _original : _current;
            UpdateHistogram();
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            if (version == _renderVersion)
            {
                TxtPreviewMode.Text = $"Render error: {ex.Message}";
            }
        }
    }

    private void UpdateHistogram()
    {
        BitmapSource? image = _showOriginal ? _original : _current;
        if (image != null)
        {
            TxtHistogram.Text = LabImageProcessor.Describe(image);
        }
    }

    private void BtnTogglePreview_Click(object sender, RoutedEventArgs e)
    {
        _showOriginal = !_showOriginal;
        ImgPreview.Source = _showOriginal ? _original : _current;
        TxtPreviewMode.Text = _showOriginal ? "ORIGINAL / قبل" : "EDITED PREVIEW / بعد";
        UpdateHistogram();
    }

    private void BtnReset_Click(object sender, RoutedEventArgs e)
    {
        if (_original == null) return;
        PushUndo();
        SetAdjustments(new LabAdjustments());
        CmbPreset.SelectedIndex = -1;
    }

    private void BtnCopyLook_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Clipboard.SetText(LabPresetSerializer.ToJson("Copied Lab Look", _adjustments));
            TxtPreviewMode.Text = "PRESET JSON COPIED";
        }
        catch (Exception ex)
        {
            TxtPreviewMode.Text = $"Clipboard unavailable: {ex.Message}";
        }
    }

    private void BtnUndo_Click(object sender, RoutedEventArgs e)
    {
        if (_undo.Count == 0) return;
        _redo.Push(_adjustments.Clone());
        SetAdjustments(_undo.Pop());
        CmbPreset.SelectedIndex = -1;
    }

    private void BtnRedo_Click(object sender, RoutedEventArgs e)
    {
        if (_redo.Count == 0) return;
        _undo.Push(_adjustments.Clone());
        SetAdjustments(_redo.Pop());
        CmbPreset.SelectedIndex = -1;
    }

    private void BtnImportPreset_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Enhancer / Photoshop Presets|*.enhancerpreset;*.json;*.xmp|All supported|*.*",
            Title = "Import Enhancer or Photoshop Camera Raw preset"
        };
        if (dialog.ShowDialog(this) != true) return;

        try
        {
            string text = File.ReadAllText(dialog.FileName);
            LabPresetFile preset = Path.GetExtension(dialog.FileName).Equals(".xmp", StringComparison.OrdinalIgnoreCase)
                ? LabPresetSerializer.FromXmp(text, dialog.FileName)
                : LabPresetSerializer.FromJson(text);
            PushUndo();
            SetAdjustments(preset.Adjustments);
            CmbPreset.SelectedIndex = -1;
            TxtPreviewMode.Text = $"IMPORTED: {preset.Name}";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not import the preset:\n\n{ex.Message}", "Enhancer Lab", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void BtnExportPreset_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog
        {
            Filter = "Enhancer Lab preset|*.enhancerpreset|Adobe Camera Raw XMP|*.xmp|JSON preset|*.json",
            DefaultExt = ".enhancerpreset",
            FileName = "My_Enhancer_Lab_Preset"
        };
        if (dialog.ShowDialog(this) != true) return;

        try
        {
            string name = CmbPreset.SelectedIndex >= 0 && CmbPreset.SelectedIndex < LabPresetCatalog.BuiltIn.Count
                ? LabPresetCatalog.BuiltIn[CmbPreset.SelectedIndex].NameEn
                : Path.GetFileNameWithoutExtension(dialog.FileName);
            string content = Path.GetExtension(dialog.FileName).Equals(".xmp", StringComparison.OrdinalIgnoreCase)
                ? LabPresetSerializer.ToXmp(name, _adjustments)
                : LabPresetSerializer.ToJson(name, _adjustments);
            File.WriteAllText(dialog.FileName, content);
            TxtPreviewMode.Text = $"EXPORTED PRESET: {Path.GetFileName(dialog.FileName)}";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not export the preset:\n\n{ex.Message}", "Enhancer Lab", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void BtnSaveJpeg_Click(object sender, RoutedEventArgs e) => SaveImage(false);
    private void BtnSavePng_Click(object sender, RoutedEventArgs e) => SaveImage(true);

    private void SaveImage(bool png)
    {
        if (_current == null) return;
        var dialog = new SaveFileDialog
        {
            Filter = png ? "PNG image|*.png" : "JPEG image|*.jpg;*.jpeg",
            DefaultExt = png ? ".png" : ".jpg",
            FileName = Path.GetFileNameWithoutExtension(_initialImagePath ?? "EnhancerLab") + "_Lab"
        };
        if (dialog.ShowDialog(this) != true) return;

        try
        {
            BitmapEncoder encoder;
            if (png)
            {
                encoder = new PngBitmapEncoder();
            }
            else
            {
                encoder = new JpegBitmapEncoder { QualityLevel = 100 };
            }
            BitmapMetadata metadata = new BitmapMetadata(png ? "png" : "jpg")
            {
                ApplicationName = "Enhancer Lab",
                Comment = "Enhancer Lab — offline non-destructive retouch and color grade"
            };
            encoder.Frames.Add(BitmapFrame.Create(_current, null, metadata, _current.ColorContexts));
            using FileStream stream = File.Create(dialog.FileName);
            encoder.Save(stream);
            TxtPreviewMode.Text = $"SAVED: {Path.GetFileName(dialog.FileName)}";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not save the edited image:\n\n{ex.Message}", "Enhancer Lab", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void BtnDone_Click(object sender, RoutedEventArgs e) => Close();

    private void BtnFit_Click(object sender, RoutedEventArgs e) => ApplyFitToPreview();

    private void Btn100_Click(object sender, RoutedEventArgs e)
    {
        ImgPreview.Stretch = Stretch.None;
        ImgPreview.Width = double.NaN;
        ImgPreview.Height = double.NaN;
        ImgPreview.LayoutTransform = new ScaleTransform(1, 1);
        TxtPreviewMode.Text = "100% / PIXEL VIEW";
    }

    private void Btn200_Click(object sender, RoutedEventArgs e)
    {
        ImgPreview.Stretch = Stretch.None;
        ImgPreview.Width = double.NaN;
        ImgPreview.Height = double.NaN;
        ImgPreview.LayoutTransform = new ScaleTransform(2, 2);
        TxtPreviewMode.Text = "200% / DETAIL VIEW";
    }

    private void ApplyFitToPreview()
    {
        if (_current == null || PreviewScroll.ActualWidth < 20 || PreviewScroll.ActualHeight < 20) return;
        ImgPreview.LayoutTransform = Transform.Identity;
        ImgPreview.Stretch = Stretch.Uniform;
        ImgPreview.Width = Math.Max(40, PreviewScroll.ActualWidth - 38);
        ImgPreview.Height = Math.Max(40, PreviewScroll.ActualHeight - 38);
        TxtPreviewMode.Text = _showOriginal ? "ORIGINAL / قبل" : "EDITED PREVIEW / بعد";
    }

    private void LabWindow_KeyDown(object sender, KeyEventArgs e)
    {
        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.Z)
        {
            BtnUndo_Click(sender, e);
            e.Handled = true;
        }
        else if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.Y)
        {
            BtnRedo_Click(sender, e);
            e.Handled = true;
        }
        else if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.S)
        {
            BtnSaveJpeg_Click(sender, e);
            e.Handled = true;
        }
        else if (e.Key == Key.P && Keyboard.Modifiers == ModifierKeys.None)
        {
            BtnTogglePreview_Click(sender, e);
            e.Handled = true;
        }
    }
}
