using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using JcTool.WinUI.Models;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace JcTool.WinUI.Views;

public sealed partial class IrCameraView : UserControl
{
    private IrColorMode _activeColorMode;
    private byte[]? _lastPixels;
    private int _lastWidth;
    private int _lastHeight;

    public IrCameraView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += IrCameraView_SizeChanged;
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += IrCameraView_Unloaded;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            PreviewImage,
            ViewModel.Text("IrPreviewAutomationName"));
        UpdateControls();
    }

    public MainViewModel ViewModel { get; }

    private async void CaptureButton_Click(object sender, RoutedEventArgs e)
    {
        await CaptureAsync();
    }

    private async void StreamButton_Click(object sender, RoutedEventArgs e)
    {
        if (ViewModel.IsIrStreaming)
        {
            ViewModel.StopIrStream();
            return;
        }

        var options = ReadOptions();
        _activeColorMode = options.ColorMode;
        await ViewModel.CaptureIrAsync(options, ApplyFrame, continuous: true);
    }

    internal async Task CaptureAsync()
    {
        var options = ReadOptions();
        _activeColorMode = options.ColorMode;
        await ViewModel.CaptureIrAsync(options, ApplyFrame, continuous: false);
    }

    private async void SaveButton_Click(object sender, RoutedEventArgs e)
    {
        if (_lastPixels is null || App.MainWindowInstance is null)
        {
            return;
        }

        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.PicturesLibrary,
            SuggestedFileName = $"IRcamera_{DateTime.Now:yyyyMMdd_HHmmss}"
        };
        picker.FileTypeChoices.Add("PNG", new List<string> { ".png" });
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker,
            WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindowInstance));
        var file = await picker.PickSaveFileAsync();
        if (file is null)
        {
            return;
        }

        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(
            BitmapPixelFormat.Bgra8,
            BitmapAlphaMode.Straight,
            (uint)_lastWidth,
            (uint)_lastHeight,
            96,
            96,
            _lastPixels);
        await encoder.FlushAsync();
    }

    private IrCameraOptions ReadOptions()
    {
        return new IrCameraOptions
        {
            Resolution = Math.Clamp(ResolutionPicker.SelectedIndex, 0, 3),
            ExposureMicroseconds = (int)Math.Clamp(ExposureBox.Value, 0, 600),
            DigitalGain = (int)Math.Clamp(GainSlider.Value, 1, 255),
            LedsEnabled = LedsToggle.IsOn,
            ExternalLightFilter = ExternalFilterToggle.IsOn,
            Denoise = DenoiseToggle.IsOn,
            FlipHorizontal = FlipToggle.IsOn,
            ColorMode = (IrColorMode)Math.Clamp(ColorModePicker.SelectedIndex, 0, 2)
        };
    }

    private void ApplyFrame(IrFrame frame)
    {
        var outputWidth = frame.Height;
        var outputHeight = frame.Width;
        var pixels = new byte[outputWidth * outputHeight * 4];
        for (var y = 0; y < frame.Height; y++)
        {
            for (var x = 0; x < frame.Width; x++)
            {
                var value = frame.Pixels[y * frame.Width + x];
                var destinationX = frame.Height - y - 1;
                var destinationY = x;
                var destination = (destinationY * outputWidth + destinationX) * 4;
                switch (_activeColorMode)
                {
                case IrColorMode.NightVision:
                    pixels[destination + 1] = value;
                    break;
                case IrColorMode.Infrared:
                    pixels[destination + 2] = value;
                    break;
                default:
                    pixels[destination] = value;
                    pixels[destination + 1] = value;
                    pixels[destination + 2] = value;
                    break;
                }
                pixels[destination + 3] = 0xff;
            }
        }

        var bitmap = new WriteableBitmap(outputWidth, outputHeight);
        using (var stream = bitmap.PixelBuffer.AsStream())
        {
            stream.Write(pixels);
        }
        bitmap.Invalidate();
        _lastPixels = pixels;
        _lastWidth = outputWidth;
        _lastHeight = outputHeight;
        PreviewImage.Source = bitmap;
        SaveButton.IsEnabled = true;
        EmptyPreview.Visibility = Visibility.Collapsed;
        FrameStatsText.Text = string.Format(
            ViewModel.Text("IrStatsFormat"),
            frame.FrameNumber,
            frame.Width,
            frame.Height,
            frame.AverageIntensity,
            frame.WhitePixels,
            frame.AmbientPixels);
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.IsBusy)
            or nameof(MainViewModel.IsIrStreaming)
            or nameof(MainViewModel.CanCancelOperation)
            or nameof(MainViewModel.SelectedController))
        {
            UpdateControls();
        }
    }

    private void UpdateControls()
    {
        var streaming = ViewModel.IsIrStreaming;
        var supported = ViewModel.SelectedSupportsIr;
        CaptureButton.IsEnabled = supported && !ViewModel.IsBusy;
        StreamButton.IsEnabled = supported
            && (!ViewModel.IsBusy || streaming && ViewModel.CanCancelOperation);
        SettingsPanel.IsHitTestVisible = supported && !ViewModel.IsBusy;
        SettingsPanel.Opacity = supported ? 1 : 0.55;
        StreamIcon.Glyph = streaming ? "\uE71A" : "\uE768";
        StreamLabel.Text = ViewModel.Text(streaming
            ? "StopIrStream.Content"
            : "StartIrStream.Content");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            StreamButton,
            StreamLabel.Text);
    }

    private void IrCameraView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 850;
        PreviewColumn.Width = new GridLength(1, GridUnitType.Star);
        SettingsColumn.Width = narrow ? new GridLength(0) : new GridLength(2, GridUnitType.Star);
        Grid.SetRow(SettingsPanel, narrow ? 1 : 0);
        Grid.SetColumn(SettingsPanel, narrow ? 0 : 1);
        Grid.SetRow(StatsPanel, narrow ? 2 : 1);
        Grid.SetColumnSpan(StatsPanel, narrow ? 1 : 2);
    }

    private void IrCameraView_Unloaded(object sender, RoutedEventArgs e)
    {
        ViewModel.StopIrStream();
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }
}
