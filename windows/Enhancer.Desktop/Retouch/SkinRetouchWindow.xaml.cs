using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Enhancer.Desktop.Native;
using Microsoft.Win32;

namespace Enhancer.Desktop.Retouch;

public partial class SkinRetouchWindow : Window
{
    private readonly string? _initialImagePath;
    private string? _sourceImagePath;
    private BitmapSource? _original;
    private BitmapSource? _current;
    private SkinRetouchSettings _settings = new();
    private CancellationTokenSource? _renderCancellation;
    private bool _suppressChanges;
    private bool _showOriginal;
    private bool _maskEditing;
    private bool _eraseMask;
    private bool _maskUndoPushed;
    private Point? _lastMaskPoint;
    private int _renderVersion;

    public string? SavedOutputPath { get; private set; }
    public int SavedOutputWidth { get; private set; }
    public int SavedOutputHeight { get; private set; }

    public SkinRetouchWindow(string? imagePath = null)
    {
        InitializeComponent();
        _initialImagePath = imagePath;
        if (!string.IsNullOrWhiteSpace(imagePath))
        {
            LoadImage(imagePath);
        }
        else
        {
            UpdateValueLabels();
        }
    }

    private void SkinRetouchWindow_Loaded(object sender, RoutedEventArgs e)
    {
        ApplyFitToPreview();
        UpdateMaskOverlay();
    }

    private void LoadImage(string path)
    {
        try
        {
            if (!File.Exists(path)) throw new FileNotFoundException("Image file not found.", path);
            var decoded = NativeEngine.DecodeImageWithExif(path);
            _sourceImagePath = path;
            _original = decoded.PreviewBitmap;
            _current = _original;
            _settings = new SkinRetouchSettings();
            _showOriginal = false;
            _maskEditing = false;
            _eraseMask = false;
            _maskUndoPushed = false;
            SavedOutputPath = null;
            TxtImageName.Text = Path.GetFileName(path);
            ImgPreview.Source = _current;
            TxtPreviewMode.Text = "EDITED PREVIEW / بعد";
            UpdateValueLabels();
            ApplyFitToPreview();
            UpdateMaskOverlay();
            TxtResultStatus.Text = $"Ready • {decoded.Width}×{decoded.Height}px • paint the orange area";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not open the image:\n\n{ex.Message}", "Skin Retouch", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void BtnOpenImage_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Images|*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff|All files|*.*",
            Multiselect = false,
            Title = "Open image in Skin Retouch"
        };
        if (dialog.ShowDialog(this) == true) LoadImage(dialog.FileName);
    }

    private void BtnPaint_Click(object sender, RoutedEventArgs e) =>
        SetMaskMode(!_maskEditing || _eraseMask, false);

    private void BtnErase_Click(object sender, RoutedEventArgs e) =>
        SetMaskMode(!_maskEditing || !_eraseMask, true);

    private void SetMaskMode(bool enabled, bool erase)
    {
        _maskEditing = enabled;
        _eraseMask = erase;
        MaskCanvas.IsHitTestVisible = enabled;
        BtnPaint.Content = enabled && !erase ? "Painting Skin Area" : "Paint";
        BtnErase.Content = enabled && erase ? "Erasing Skin Area" : "Erase";
        MaskCanvas.Opacity = enabled ? 0.94 : 0.0;
        TxtResultStatus.Text = enabled
            ? (erase ? "ERASE MODE • remove orange mask" : "PAINT MODE • select skin only")
            : "Mask editing stopped";
    }

    private void SettingChanged(object sender, RoutedEventArgs e)
    {
        if (_suppressChanges || !IsLoaded) return;
        _settings.Strength = SldStrength.Value;
        _settings.TexturePreservation = SldTexture.Value;
        _settings.SkinToneProtection = ChkSkinTone.IsChecked == true;
        UpdateValueLabels();
    }

    private void SettingChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        SettingChanged(sender, (RoutedEventArgs)e);

    private void UpdateValueLabels()
    {
        if (TxtStrength == null) return;
        TxtBrush.Text = $"{SldBrush.Value * 100:0}%";
        TxtStrength.Text = $"{SldStrength.Value * 100:0}%";
        TxtTexture.Text = $"{SldTexture.Value * 100:0}%";
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
        if (!_maskEditing || _original == null) return;
        Rect imageRect = GetImageContentRect();
        if (imageRect.Width <= 1 || imageRect.Height <= 1 || !imageRect.Contains(point)) return;

        double nx = Math.Clamp((point.X - imageRect.X) / imageRect.Width, 0, 1);
        double ny = Math.Clamp((point.Y - imageRect.Y) / imageRect.Height, 0, 1);
        double minDistance = Math.Max(2.0, SldBrush.Value * Math.Min(imageRect.Width, imageRect.Height) * 0.20);
        if (_lastMaskPoint is Point previous &&
            Math.Abs(point.X - previous.X) < minDistance &&
            Math.Abs(point.Y - previous.Y) < minDistance)
        {
            return;
        }

        _settings.Strokes ??= new List<SkinRetouchStroke>();
        _settings.Strokes.Add(new SkinRetouchStroke
        {
            X = nx,
            Y = ny,
            Radius = Math.Clamp(SldBrush.Value, 0.01, 0.40),
            Opacity = 0.95,
            Erase = _eraseMask
        });
        _maskUndoPushed = true;
        _lastMaskPoint = point;
        _showOriginal = false;
        ImgPreview.Source = _current;
        UpdateMaskOverlay();
        ScheduleRender();
    }

    private void BtnClearMask_Click(object sender, RoutedEventArgs e)
    {
        if (_settings.Strokes == null || _settings.Strokes.Count == 0) return;
        _settings.Strokes.Clear();
        UpdateMaskOverlay();
        ScheduleRender();
        TxtResultStatus.Text = "Mask cleared • original is unchanged";
    }

    private void BtnRender_Click(object sender, RoutedEventArgs e) => ScheduleRender();

    private void ScheduleRender()
    {
        if (_original == null) return;
        _renderCancellation?.Cancel();
        _renderCancellation?.Dispose();
        _renderCancellation = new CancellationTokenSource();
        int version = ++_renderVersion;
        CancellationToken token = _renderCancellation.Token;
        SkinRetouchSettings snapshot = _settings.Clone();
        BitmapSource original = _original;
        TxtResultStatus.Text = "RENDERING LOCAL SKIN RETOUCH...";
        _ = RenderAsync(version, original, snapshot, token);
    }

    private async Task RenderAsync(
        int version,
        BitmapSource original,
        SkinRetouchSettings snapshot,
        CancellationToken token)
    {
        Stopwatch stopwatch = Stopwatch.StartNew();
        try
        {
            SkinRetouchResult result = await Task.Run(
                () => SkinRetouchProcessor.Apply(original, snapshot, token),
                token);
            if (token.IsCancellationRequested || version != _renderVersion) return;

            _current = result.Bitmap;
            ImgPreview.Source = _showOriginal ? _original : _current;
            TxtMaskMetrics.Text = $"Mask: {result.MaskedPixels:N0} px";
            TxtChangeMetrics.Text = $"Changed pixels: {result.ChangedPixels:N0}";
            TxtDeltaMetrics.Text = $"Maximum delta: {result.MaxChannelDelta} • mean: {result.MeanChannelDelta:0.0}";
            TxtResultStatus.Text = result.HasVisibleChange
                ? $"RETOUCH COMPLETE • {result.ChangedPixels:N0} pixels changed • {stopwatch.Elapsed.TotalSeconds:0.0}s"
                : "No visible pixel change • paint a wider skin area or increase strength";
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            if (version == _renderVersion) TxtResultStatus.Text = $"Render error: {ex.Message}";
        }
    }

    private void BtnToggleBefore_Click(object sender, RoutedEventArgs e)
    {
        _showOriginal = !_showOriginal;
        ImgPreview.Source = _showOriginal ? _original : _current;
        TxtPreviewMode.Text = _showOriginal ? "ORIGINAL / قبل" : "EDITED PREVIEW / بعد";
    }

    private void BtnFit_Click(object sender, RoutedEventArgs e) => ApplyFitToPreview();
    private void Btn100_Click(object sender, RoutedEventArgs e) => SetZoom(1.0);
    private void Btn200_Click(object sender, RoutedEventArgs e) => SetZoom(2.0);

    private void PreviewScroll_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_original != null && ImgPreview.Stretch == Stretch.Uniform) ApplyFitToPreview();
    }

    private void ApplyFitToPreview()
    {
        if (_original == null || PreviewScroll.ActualWidth < 20 || PreviewScroll.ActualHeight < 20) return;
        double width = Math.Max(40, PreviewScroll.ActualWidth - 40);
        double height = Math.Max(40, PreviewScroll.ActualHeight - 40);
        ImgPreview.Stretch = Stretch.Uniform;
        PreviewHost.Width = width;
        PreviewHost.Height = height;
        ImgPreview.Width = width;
        ImgPreview.Height = height;
        MaskCanvas.Width = width;
        MaskCanvas.Height = height;
        UpdateMaskOverlay();
    }

    private void SetZoom(double factor)
    {
        if (_original == null) return;
        double width = Math.Max(40, _original.PixelWidth * factor);
        double height = Math.Max(40, _original.PixelHeight * factor);
        ImgPreview.Stretch = Stretch.None;
        PreviewHost.Width = width;
        PreviewHost.Height = height;
        ImgPreview.Width = width;
        ImgPreview.Height = height;
        MaskCanvas.Width = width;
        MaskCanvas.Height = height;
        UpdateMaskOverlay();
    }

    private Rect GetImageContentRect()
    {
        double boxWidth = PreviewHost?.ActualWidth ?? 0;
        double boxHeight = PreviewHost?.ActualHeight ?? 0;
        if (_original == null || boxWidth <= 1 || boxHeight <= 1)
            return new Rect(0, 0, boxWidth, boxHeight);

        if (ImgPreview.Stretch == Stretch.None)
            return new Rect(0, 0, boxWidth, boxHeight);

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

    private void UpdateMaskOverlay()
    {
        if (MaskCanvas == null) return;
        MaskCanvas.Children.Clear();
        if (_settings.Strokes == null) return;
        Rect imageRect = GetImageContentRect();
        if (imageRect.Width <= 1 || imageRect.Height <= 1) return;

        foreach (SkinRetouchStroke stroke in _settings.Strokes)
        {
            double radiusX = Math.Max(2, stroke.Radius * imageRect.Width);
            double radiusY = Math.Max(2, stroke.Radius * imageRect.Height);
            Color color = stroke.Erase ? Color.FromRgb(248, 113, 113) : Color.FromRgb(251, 146, 60);
            var ellipse = new System.Windows.Shapes.Ellipse
            {
                Width = radiusX * 2,
                Height = radiusY * 2,
                Fill = new SolidColorBrush(Color.FromArgb(50, color.R, color.G, color.B)),
                Stroke = new SolidColorBrush(Color.FromArgb(155, color.R, color.G, color.B)),
                StrokeThickness = 1,
                IsHitTestVisible = false
            };
            Canvas.SetLeft(ellipse, imageRect.X + Math.Clamp(stroke.X, 0, 1) * imageRect.Width - radiusX);
            Canvas.SetTop(ellipse, imageRect.Y + Math.Clamp(stroke.Y, 0, 1) * imageRect.Height - radiusY);
            MaskCanvas.Children.Add(ellipse);
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
            FileName = Path.GetFileNameWithoutExtension(_initialImagePath ?? _sourceImagePath ?? "Enhancer") + "_SkinRetouch"
        };
        if (dialog.ShowDialog(this) != true) return;

        try
        {
            if (!string.IsNullOrWhiteSpace(_sourceImagePath) &&
                string.Equals(Path.GetFullPath(dialog.FileName), Path.GetFullPath(_sourceImagePath), StringComparison.OrdinalIgnoreCase))
            {
                MessageBox.Show(this, "The original image is protected. Choose a new output filename.", "Non-destructive Skin Retouch", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            BitmapEncoder encoder = png
                ? new PngBitmapEncoder()
                : new JpegBitmapEncoder { QualityLevel = 100 };
            encoder.Frames.Add(BitmapFrame.Create(_current));
            using FileStream stream = File.Create(dialog.FileName);
            encoder.Save(stream);
            SavedOutputPath = dialog.FileName;
            SavedOutputWidth = _current.PixelWidth;
            SavedOutputHeight = _current.PixelHeight;
            TxtResultStatus.Text = $"SAVED COPY: {Path.GetFileName(dialog.FileName)}";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not save the retouched image:\n\n{ex.Message}", "Skin Retouch", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void BtnReset_Click(object sender, RoutedEventArgs e)
    {
        _settings = new SkinRetouchSettings();
        _suppressChanges = true;
        try
        {
            SldBrush.Value = 0.075;
            SldStrength.Value = _settings.Strength;
            SldTexture.Value = _settings.TexturePreservation;
            ChkSkinTone.IsChecked = _settings.SkinToneProtection;
        }
        finally
        {
            _suppressChanges = false;
        }
        _current = _original;
        _showOriginal = false;
        ImgPreview.Source = _current;
        UpdateValueLabels();
        UpdateMaskOverlay();
        ScheduleRender();
    }

    private void BtnDone_Click(object sender, RoutedEventArgs e) => Close();

    private void SkinRetouchWindow_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.P && Keyboard.Modifiers == ModifierKeys.None)
        {
            BtnToggleBefore_Click(sender, e);
            e.Handled = true;
        }
        else if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.S)
        {
            BtnSaveJpeg_Click(sender, e);
            e.Handled = true;
        }
    }
}
