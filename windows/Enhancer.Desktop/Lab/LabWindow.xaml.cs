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
    private bool _maskEditing;
    private bool _eraseMask;
    private bool _retouchMaskMode;
    private bool _maskUndoPushed;
    private Point? _lastMaskPoint;
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

    private void BtnPaintMask_Click(object sender, RoutedEventArgs e)
    {
        SetMaskMode(!_maskEditing || _eraseMask || _retouchMaskMode, false, false);
    }

    private void BtnEraseMask_Click(object sender, RoutedEventArgs e)
    {
        SetMaskMode(!_maskEditing || !_eraseMask || _retouchMaskMode, true, false);
    }

    private void BtnPaintRetouch_Click(object sender, RoutedEventArgs e)
    {
        SetMaskMode(!_maskEditing || _eraseMask || !_retouchMaskMode, false, true);
    }

    private void BtnEraseRetouch_Click(object sender, RoutedEventArgs e)
    {
        SetMaskMode(!_maskEditing || !_eraseMask || !_retouchMaskMode, true, true);
    }

    private void BtnStrongManualRetouch_Click(object sender, RoutedEventArgs e)
    {
        if (_original == null) return;
        PushUndo();
        LabAdjustments stronger = _adjustments.Clone();
        stronger.ManualRetouchStrength = 0.9;
        stronger.SkinSmoothing = Math.Max(stronger.SkinSmoothing, 0.72);
        stronger.FaceLight = Math.Max(stronger.FaceLight, 0.28);
        stronger.EyeDetail = Math.Max(stronger.EyeDetail, 0.42);
        stronger.TeethWhiten = Math.Max(stronger.TeethWhiten, 0.28);
        stronger.Sharpening = Math.Max(stronger.Sharpening, 0.18);
        CmbPreset.SelectedIndex = -1;
        SetAdjustments(stronger);
        TxtPreviewMode.Text = "STRONG LOCAL RETOUCH • paint the orange mask over the face";
    }

    private void BtnClearMask_Click(object sender, RoutedEventArgs e)
    {
        if (_adjustments.MaskStrokes == null || _adjustments.MaskStrokes.Count == 0) return;
        PushUndo();
        _adjustments.MaskStrokes.Clear();
        UpdateMaskOverlay();
        ScheduleRender();
        TxtPreviewMode.Text = "FACE PROTECTION MASK CLEARED";
    }

    private void SetMaskMode(bool enabled, bool erase, bool retouch)
    {
        _maskEditing = enabled;
        _eraseMask = erase;
        _retouchMaskMode = retouch;
        MaskCanvas.IsHitTestVisible = enabled;
        BtnPaintMask.Content = enabled && !erase && !retouch ? "Painting Protect Mask" : "Paint Protect Mask";
        BtnEraseMask.Content = enabled && erase && !retouch ? "Erasing Protect Mask" : "Erase Protect Mask";
        BtnPaintRetouch.Content = enabled && !erase && retouch ? "Painting Retouch Area" : "Paint Retouch Area";
        BtnEraseRetouch.Content = enabled && erase && retouch ? "Erasing Retouch Area" : "Erase Retouch Area";
        MaskCanvas.Opacity = enabled ? 0.92 : 0.0;
        if (enabled)
        {
            TxtPreviewMode.Text = retouch
                ? (erase ? "RETOUCH MASK ERASE • remove the orange area" : "RETOUCH MASK PAINT • select the face area")
                : (erase ? "PROTECT MASK ERASE • remove the protected area" : "PROTECT MASK PAINT • protect identity-critical areas");
        }
    }

    private void MaskCanvas_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!_maskEditing || _original == null) return;
        _maskUndoPushed = false;
        _lastMaskPoint = null;
        MaskCanvas.CaptureMouse();
        AddMaskStroke(e.GetPosition(MaskCanvas));
        e.Handled = true;
    }

    private void MaskCanvas_MouseMove(object sender, MouseEventArgs e)
    {
        if (!_maskEditing || !_maskUndoPushed || e.LeftButton != MouseButtonState.Pressed) return;
        AddMaskStroke(e.GetPosition(MaskCanvas));
        e.Handled = true;
    }

    private void MaskCanvas_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (!_maskEditing) return;
        MaskCanvas.ReleaseMouseCapture();
        _lastMaskPoint = null;
        _maskUndoPushed = false;
        e.Handled = true;
    }

    private void AddMaskStroke(Point point)
    {
        if (!_maskEditing) return;
        Rect imageRect = GetImageContentRect();
        if (imageRect.Width <= 1 || imageRect.Height <= 1 || !imageRect.Contains(point)) return;
        double nx = Math.Clamp((point.X - imageRect.X) / imageRect.Width, 0, 1);
        double ny = Math.Clamp((point.Y - imageRect.Y) / imageRect.Height, 0, 1);
        double minDistance = Math.Max(2.0, SldMaskBrush.Value * Math.Min(imageRect.Width, imageRect.Height) * 0.22);
        if (_lastMaskPoint is Point previous &&
            Math.Abs(point.X - previous.X) < minDistance && Math.Abs(point.Y - previous.Y) < minDistance)
        {
            return;
        }

        if (!_maskUndoPushed)
        {
            PushUndo();
            _maskUndoPushed = true;
        }
        _adjustments.MaskStrokes ??= new List<LabMaskStroke>();
        _adjustments.MaskStrokes.Add(new LabMaskStroke
        {
            X = nx,
            Y = ny,
            Radius = Math.Clamp(SldMaskBrush.Value, 0.01, 0.45),
            Opacity = 0.88,
            Erase = _eraseMask,
            Retouch = _retouchMaskMode
        });
        _lastMaskPoint = point;
        UpdateMaskOverlay();
        ScheduleRender();
    }

    private void UpdateMaskOverlay()
    {
        if (MaskCanvas == null) return;
        MaskCanvas.Children.Clear();
        if (_adjustments.MaskStrokes == null) return;
        Rect imageRect = GetImageContentRect();
        if (imageRect.Width <= 1 || imageRect.Height <= 1) return;
        double width = imageRect.Width;
        double height = imageRect.Height;
        foreach (LabMaskStroke stroke in _adjustments.MaskStrokes)
        {
            double radiusX = Math.Max(2, stroke.Radius * width);
            double radiusY = Math.Max(2, stroke.Radius * height);
            Color overlayColor = stroke.Erase
                ? Color.FromRgb(248, 113, 113)
                : stroke.Retouch
                    ? Color.FromRgb(251, 146, 60)
                    : Color.FromRgb(52, 211, 153);
            var ellipse = new System.Windows.Shapes.Ellipse
            {
                Width = radiusX * 2,
                Height = radiusY * 2,
                Fill = new SolidColorBrush(Color.FromArgb(48, overlayColor.R, overlayColor.G, overlayColor.B)),
                Stroke = new SolidColorBrush(Color.FromArgb(130, overlayColor.R, overlayColor.G, overlayColor.B)),
                StrokeThickness = 1,
                IsHitTestVisible = false
            };
            Canvas.SetLeft(ellipse, imageRect.X + Math.Clamp(stroke.X, 0, 1) * width - radiusX);
            Canvas.SetTop(ellipse, imageRect.Y + Math.Clamp(stroke.Y, 0, 1) * height - radiusY);
            MaskCanvas.Children.Add(ellipse);
        }
    }

    private Rect GetImageContentRect()
    {
        double boxWidth = MaskCanvas?.ActualWidth ?? 0;
        double boxHeight = MaskCanvas?.ActualHeight ?? 0;
        if (_original == null || boxWidth <= 1 || boxHeight <= 1)
        {
            return new Rect(0, 0, boxWidth, boxHeight);
        }

        double sourceAspect = _original.PixelWidth / (double)Math.Max(1, _original.PixelHeight);
        double boxAspect = boxWidth / boxHeight;
        if (boxAspect > sourceAspect)
        {
            double contentWidth = boxHeight * sourceAspect;
            return new Rect((boxWidth - contentWidth) / 2.0, 0, contentWidth, boxHeight);
        }

        double contentHeight = boxWidth / sourceAspect;
        return new Rect(0, (boxHeight - contentHeight) / 2.0, boxWidth, contentHeight);
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
            SldCurveBlack.Value = _adjustments.CurveBlack;
            SldCurveQuarter.Value = _adjustments.CurveQuarter;
            SldCurveMid.Value = _adjustments.CurveMid;
            SldCurveThreeQuarter.Value = _adjustments.CurveThreeQuarter;
            SldCurveWhite.Value = _adjustments.CurveWhite;
            SldShadowHue.Value = _adjustments.ShadowHue;
            SldShadowSaturation.Value = _adjustments.ShadowSaturation;
            SldHighlightHue.Value = _adjustments.HighlightHue;
            SldHighlightSaturation.Value = _adjustments.HighlightSaturation;
            SldManualRetouchStrength.Value = _adjustments.ManualRetouchStrength;
            ChkMonochrome.IsChecked = _adjustments.Monochrome;
            ChkFaceProtection.IsChecked = _adjustments.FaceProtection;
        }
        finally
        {
            _suppressChanges = false;
        }
        UpdateValueLabels();
        UpdateMaskOverlay();
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
        CurveBlack = SldCurveBlack.Value,
        CurveQuarter = SldCurveQuarter.Value,
        CurveMid = SldCurveMid.Value,
        CurveThreeQuarter = SldCurveThreeQuarter.Value,
        CurveWhite = SldCurveWhite.Value,
        ShadowHue = SldShadowHue.Value,
        ShadowSaturation = SldShadowSaturation.Value,
        HighlightHue = SldHighlightHue.Value,
        HighlightSaturation = SldHighlightSaturation.Value,
        ManualRetouchStrength = SldManualRetouchStrength.Value,
        Monochrome = ChkMonochrome.IsChecked == true,
        FaceProtection = ChkFaceProtection.IsChecked != false,
        MaskStrokes = _adjustments.MaskStrokes?.Select(stroke => stroke.Clone()).ToList() ?? new()
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
        TxtCurveBlack.Text = Percent(SldCurveBlack.Value);
        TxtCurveQuarter.Text = Percent(SldCurveQuarter.Value);
        TxtCurveMid.Text = Percent(SldCurveMid.Value);
        TxtCurveThreeQuarter.Text = Percent(SldCurveThreeQuarter.Value);
        TxtCurveWhite.Text = Percent(SldCurveWhite.Value);
        TxtShadowHue.Text = Percent(SldShadowHue.Value);
        TxtShadowSaturation.Text = Percent(SldShadowSaturation.Value);
        TxtHighlightHue.Text = Percent(SldHighlightHue.Value);
        TxtHighlightSaturation.Text = Percent(SldHighlightSaturation.Value);
        TxtManualRetouchStrength.Text = Percent(SldManualRetouchStrength.Value);
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
        TxtPreviewMode.Text = $"RENDERING LAB PREVIEW • {original.PixelWidth}×{original.PixelHeight}px";

        _ = RenderAsync(version, original, snapshot, token);
    }

    private async Task RenderAsync(int version, BitmapSource original, LabAdjustments snapshot, CancellationToken token)
    {
        Stopwatch stopwatch = Stopwatch.StartNew();
        try
        {
            BitmapSource rendered = await Task.Run(() => LabImageProcessor.Apply(original, snapshot, token), token);
            if (token.IsCancellationRequested || version != _renderVersion) return;
            _current = rendered;
            ImgPreview.Source = _showOriginal ? _original : _current;
            UpdateHistogram();
            string maskStatus = snapshot.MaskStrokes?.Any(stroke => stroke.Retouch && !stroke.Erase) == true
                ? " • orange retouch mask active"
                : string.Empty;
            TxtPreviewMode.Text = _showOriginal
                ? $"ORIGINAL / قبل • preview ready in {stopwatch.Elapsed.TotalSeconds:0.0}s"
                : $"EDITED PREVIEW / بعد • rendered in {stopwatch.Elapsed.TotalSeconds:0.0}s{maskStatus}";
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

    private async void BtnBatchApply_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Images|*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff",
            Multiselect = true,
            Title = "Batch apply the current Lab look"
        };
        if (dialog.ShowDialog(this) != true || dialog.FileNames.Length == 0) return;

        MessageBoxResult confirmation = MessageBox.Show(
            this,
            $"Apply the current non-destructive Lab look to {dialog.FileNames.Length} image(s)?\n\nOutputs are written beside the originals with a _Lab suffix.",
            "Enhancer Lab batch",
            MessageBoxButton.OKCancel,
            MessageBoxImage.Question);
        if (confirmation != MessageBoxResult.OK) return;

        LabAdjustments snapshot = _adjustments.Clone();
        int completed = 0;
        int failed = 0;
        var failures = new List<string>();
        TxtPreviewMode.Text = "BATCH APPLY 0%";
        try
        {
            await Task.Run(() =>
            {
                for (int i = 0; i < dialog.FileNames.Length; i++)
                {
                    string sourcePath = dialog.FileNames[i];
                    try
                    {
                        BitmapSource source = NativeEngine.DecodeImageWithExif(sourcePath).PreviewBitmap;
                        BitmapSource rendered = LabImageProcessor.Apply(source, snapshot);
                        bool jpeg = Path.GetExtension(sourcePath).Equals(".jpg", StringComparison.OrdinalIgnoreCase) ||
                                    Path.GetExtension(sourcePath).Equals(".jpeg", StringComparison.OrdinalIgnoreCase);
                        string outputPath = GetBatchOutputPath(sourcePath, jpeg ? ".jpg" : ".png");
                        SaveBitmapFile(rendered, outputPath, jpeg);
                        completed++;
                    }
                    catch (Exception error)
                    {
                        failed++;
                        failures.Add($"{Path.GetFileName(sourcePath)}: {error.Message}");
                    }

                    int progress = (int)Math.Round((i + 1) * 100.0 / dialog.FileNames.Length);
                    Dispatcher.Invoke(() => TxtPreviewMode.Text = $"BATCH APPLY {progress}% • {completed} saved");
                }
            });

            string detail = failures.Count == 0 ? string.Empty : $"\n\nFailed:\n{string.Join("\n", failures.Take(5))}";
            TxtPreviewMode.Text = $"BATCH COMPLETE • {completed} saved • {failed} failed";
            if (failed > 0)
            {
                MessageBox.Show(this, $"Batch processing finished with {failed} failure(s).{detail}",
                    "Enhancer Lab batch", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }
        catch (Exception ex)
        {
            TxtPreviewMode.Text = "BATCH FAILED";
            MessageBox.Show(this, $"Batch processing failed:\n\n{ex.Message}",
                "Enhancer Lab batch", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private static string GetBatchOutputPath(string sourcePath, string extension)
    {
        string directory = Path.GetDirectoryName(sourcePath) ?? Directory.GetCurrentDirectory();
        string stem = Path.GetFileNameWithoutExtension(sourcePath);
        string path = Path.Combine(directory, stem + "_Lab" + extension);
        int suffix = 2;
        while (File.Exists(path))
        {
            path = Path.Combine(directory, $"{stem}_Lab_{suffix++}{extension}");
        }
        return path;
    }

    private static void SaveBitmapFile(BitmapSource bitmap, string path, bool jpeg)
    {
        BitmapEncoder encoder = jpeg
            ? new JpegBitmapEncoder { QualityLevel = 100 }
            : new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using FileStream stream = File.Create(path);
        encoder.Save(stream);
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
            encoder.Frames.Add(BitmapFrame.Create(_current));
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
        MaskCanvas.LayoutTransform = new ScaleTransform(1, 1);
        UpdateMaskOverlay();
        TxtPreviewMode.Text = "100% / PIXEL VIEW";
    }

    private void Btn200_Click(object sender, RoutedEventArgs e)
    {
        ImgPreview.Stretch = Stretch.None;
        ImgPreview.Width = double.NaN;
        ImgPreview.Height = double.NaN;
        ImgPreview.LayoutTransform = new ScaleTransform(2, 2);
        MaskCanvas.LayoutTransform = new ScaleTransform(2, 2);
        UpdateMaskOverlay();
        TxtPreviewMode.Text = "200% / DETAIL VIEW";
    }

    private void ApplyFitToPreview()
    {
        if (_current == null || PreviewScroll.ActualWidth < 20 || PreviewScroll.ActualHeight < 20) return;
        ImgPreview.LayoutTransform = Transform.Identity;
        MaskCanvas.LayoutTransform = Transform.Identity;
        ImgPreview.Stretch = Stretch.Uniform;
        ImgPreview.Width = Math.Max(40, PreviewScroll.ActualWidth - 38);
        ImgPreview.Height = Math.Max(40, PreviewScroll.ActualHeight - 38);
        UpdateMaskOverlay();
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
