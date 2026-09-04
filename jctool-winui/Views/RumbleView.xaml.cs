using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using JcTool.WinUI.Models;
using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace JcTool.WinUI.Views;

public sealed partial class RumbleView : UserControl
{
    private RumbleAsset? _asset;

    public RumbleView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += RumbleView_Unloaded;
    }

    public MainViewModel ViewModel { get; }

    private async void LoadButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.MusicLibrary,
            ViewMode = PickerViewMode.List
        };
        picker.FileTypeFilter.Add(".jcvib");
        picker.FileTypeFilter.Add(".bnvib");
        var window = App.MainWindowInstance;
        if (window is null)
        {
            return;
        }
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker,
            WinRT.Interop.WindowNative.GetWindowHandle(window));
        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }

        var buffer = await FileIO.ReadBufferAsync(file);
        var bytes = buffer.ToArray();
        if (!RumbleFileParser.TryParse(bytes, out var asset, out var errorResource))
        {
            _asset = null;
            PlayButton.IsEnabled = false;
            FileNameText.Text = file.Name;
            FileDetailsText.Text = ViewModel.Text(errorResource);
            EqualizerPanel.Visibility = Visibility.Collapsed;
            return;
        }

        var parsedAsset = asset!;
        _asset = parsedAsset;
        FileNameText.Text = file.Name;
        FileDetailsText.Text = string.Format(
            ViewModel.Text("RumbleDetailsFormat"),
            parsedAsset.SourceSampleCount,
            parsedAsset.SampleRateMilliseconds,
            parsedAsset.TotalSampleCount(0) * parsedAsset.SampleRateMilliseconds / 1000d);
        EqualizerPanel.Visibility = parsedAsset.SourceSamples is null
            ? Visibility.Collapsed
            : Visibility.Visible;
        ConvertStatusText.Text = ViewModel.Text("RumbleConvertReady");
        ApplyEqualizer();
        PlayButton.IsEnabled = ViewModel.HasSelectedController;
        PlaybackStatusText.Text = ViewModel.Text("Ready");
    }

    private async void PlayButton_Click(object sender, RoutedEventArgs e)
    {
        if (_asset is null)
        {
            return;
        }
        ApplyEqualizer();
        var loopTimes = Math.Clamp((int)LoopCountBox.Value, 0, 1000);
        var samples = _asset.Expand(loopTimes);
        PlayButton.IsEnabled = false;
        PlaybackStatusText.Text = ViewModel.Text("RumblePlaying");
        await ViewModel.PlayRumbleAsync(samples, _asset.SampleRateMilliseconds);
        PlaybackStatusText.Text = ViewModel.StatusText;
        PlayButton.IsEnabled = ViewModel.HasSelectedController;
    }

    private async void StopButton_Click(object sender, RoutedEventArgs e)
    {
        PlaybackStatusText.Text = ViewModel.Text("RumbleStopping");
        await ViewModel.StopRumbleAsync();
    }

    private void ApplyEqualizerButton_Click(object sender, RoutedEventArgs e)
    {
        ApplyEqualizer();
    }

    private void ApplyEqualizer()
    {
        if (_asset is null)
        {
            return;
        }
        if (_asset.SourceSamples is not null)
        {
            _asset = new RumbleAsset
            {
                Samples = RumbleFileParser.ConvertBinary(
                    _asset.SourceSamples,
                    (int)LowAmplitudeSlider.Value * 10,
                    (int)LowFrequencySlider.Value * 10,
                    (int)HighAmplitudeSlider.Value * 10,
                    (int)HighFrequencySlider.Value * 10),
                SourceSamples = _asset.SourceSamples,
                SampleRateMilliseconds = _asset.SampleRateMilliseconds,
                LoopStart = _asset.LoopStart,
                LoopEnd = _asset.LoopEnd,
                LoopWait = _asset.LoopWait,
                HasLoop = _asset.HasLoop,
                FormatResource = _asset.FormatResource
            };
        }
        var loops = Math.Clamp((int)LoopCountBox.Value, 0, 1000);
        FileDetailsText.Text = string.Format(
            ViewModel.Text("RumbleDetailsFormat"),
            _asset.SourceSampleCount,
            _asset.SampleRateMilliseconds,
            _asset.TotalSampleCount(loops) * _asset.SampleRateMilliseconds / 1000d);
        ConvertStatusText.Text = ViewModel.Text("RumbleConvertReady");
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.SelectedController))
        {
            PlayButton.IsEnabled = _asset is not null && ViewModel.HasSelectedController;
        }
        else if (e.PropertyName is nameof(MainViewModel.IsBusy)
            or nameof(MainViewModel.IsRumblePlaying))
        {
            PlayButton.IsEnabled = _asset is not null
                && ViewModel.HasSelectedController
                && !ViewModel.IsBusy;
        }
    }

    private void RumbleView_Unloaded(object sender, RoutedEventArgs e)
    {
        _ = ViewModel.StopRumbleAsync();
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }
}
