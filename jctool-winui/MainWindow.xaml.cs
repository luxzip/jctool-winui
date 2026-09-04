using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using JcTool.WinUI.Views;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace JcTool.WinUI;

public sealed partial class MainWindow : Window
{
    private readonly WindowPlacementService _windowPlacementService;
    private readonly LanguagePreferenceService _languagePreference;

    public MainWindow(
        MainViewModel viewModel,
        WindowPlacementService windowPlacementService,
        LanguagePreferenceService languagePreference)
    {
        ViewModel = viewModel;
        _windowPlacementService = windowPlacementService;
        _languagePreference = languagePreference;
        InitializeComponent();

        Title = ViewModel.Text("WindowTitle");
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        ConfigureTitleBar();
        ToolTipService.SetToolTip(RefreshButton, ViewModel.Text("RefreshControllers"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            RefreshButton,
            ViewModel.Text("RefreshControllers"));
        if (MainNavigation.SettingsItem is NavigationViewItem settingsItem)
        {
            settingsItem.Content = ViewModel.Text("SettingsHeader.Text");
        }
        _windowPlacementService.Attach(this);
        Closed += (_, _) => _ = ViewModel.CancelActiveOperationAsync();

        MainNavigation.SelectedItem = MainNavigation.MenuItems[0];
        PageHost.Content = new OverviewView(ViewModel);
        var pageArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--page=", StringComparison.OrdinalIgnoreCase));
        var initialPage = pageArgument?[7..];
        if (initialPage == "settings")
        {
            MainNavigation.SelectedItem = MainNavigation.SettingsItem;
            PageHost.Content = new SettingsView(ViewModel, _languagePreference);
        }
        var initialItem = MainNavigation.MenuItems
            .OfType<NavigationViewItem>()
            .FirstOrDefault(item => item.Tag?.ToString() == initialPage && item.IsEnabled);
        if (initialItem is not null)
        {
            MainNavigation.SelectedItem = initialItem;
        }
        RootGrid.Loaded += RootGrid_Loaded;
    }

    public MainViewModel ViewModel { get; }

    private void ConfigureTitleBar()
    {
        var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(windowHandle);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "pro2.ico"));
        appWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        appWindow.TitleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        appWindow.TitleBar.ButtonForegroundColor = Colors.White;
        appWindow.TitleBar.ButtonInactiveForegroundColor = Windows.UI.Color.FromArgb(255, 150, 156, 162);
    }

    private async void RootGrid_Loaded(object sender, RoutedEventArgs e)
    {
        ApplyTestViewport();
        await ViewModel.RefreshAsync();
        var controllerArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--controller=", StringComparison.OrdinalIgnoreCase));
        if (controllerArgument is not null
            && int.TryParse(controllerArgument[13..], out var controllerNumber))
        {
            ViewModel.SelectController(controllerNumber - 1);
        }
        var screenshotArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--screenshot=", StringComparison.OrdinalIgnoreCase));
        if (screenshotArgument is not null)
        {
            if (PageHost.Content is CalibrationView calibrationView)
            {
                await calibrationView.ReadCalibrationAsync();
            }
            else if (PageHost.Content is InputTestView inputView)
            {
                await inputView.RefreshAsync();
            }
            else if (PageHost.Content is IrCameraView irView)
            {
                await irView.CaptureAsync();
            }
            else if (PageHost.Content is NfcView nfcView)
            {
                await nfcView.ScanAsync();
            }
            await Task.Delay(250);
            await CaptureScreenshotAsync(screenshotArgument[13..].Trim('"'));
            Close();
        }
    }

    private void ApplyTestViewport()
    {
        var viewportArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--viewport=", StringComparison.OrdinalIgnoreCase));
        var dimensions = viewportArgument?[11..].Split('x', 'X');
        if (dimensions?.Length != 2
            || !int.TryParse(dimensions[0], out var width)
            || !int.TryParse(dimensions[1], out var height))
        {
            return;
        }

        var scale = RootGrid.XamlRoot?.RasterizationScale ?? 1d;
        var windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(windowHandle);
        AppWindow.GetFromWindowId(windowId).Resize(new SizeInt32(
            (int)Math.Round(width * scale),
            (int)Math.Round(height * scale)));
    }

    private async Task CaptureScreenshotAsync(string path)
    {
        var bitmap = new RenderTargetBitmap();
        await bitmap.RenderAsync(RootGrid);
        var pixels = await bitmap.GetPixelsAsync();
        var directory = await StorageFolder.GetFolderFromPathAsync(Path.GetDirectoryName(path)!);
        var file = await directory.CreateFileAsync(Path.GetFileName(path), CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(
            BitmapPixelFormat.Bgra8,
            BitmapAlphaMode.Premultiplied,
            (uint)bitmap.PixelWidth,
            (uint)bitmap.PixelHeight,
            96,
            96,
            pixels.ToArray());
        await encoder.FlushAsync();
    }

    private async void RefreshButton_Click(object sender, RoutedEventArgs e)
    {
        RefreshButton.IsEnabled = false;
        try
        {
            await ViewModel.RefreshAsync();
        }
        finally
        {
            RefreshButton.IsEnabled = true;
        }
    }

    private void DeviceButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: int slotIndex })
        {
            ViewModel.SelectController(slotIndex);
        }
    }

    private async void CancelOperationButton_Click(object sender, RoutedEventArgs e)
    {
        await ViewModel.CancelActiveOperationAsync();
    }

    private void NavigationView_SelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (args.IsSettingsSelected)
        {
            PageHost.Content = new SettingsView(ViewModel, _languagePreference);
            return;
        }

        switch (args.SelectedItemContainer?.Tag?.ToString())
        {
        case "overview":
            PageHost.Content = new OverviewView(ViewModel);
            break;
        case "colors":
            PageHost.Content = new ColorsSpiView(ViewModel);
            break;
        case "calibration":
            PageHost.Content = new CalibrationView(ViewModel);
            break;
        case "rumble":
            PageHost.Content = new RumbleView(ViewModel);
            break;
        case "input":
            PageHost.Content = new InputTestView(ViewModel);
            break;
        case "ir":
            PageHost.Content = new IrCameraView(ViewModel);
            break;
        case "nfc":
            PageHost.Content = new NfcView(ViewModel);
            break;
        case "diagnostics":
            PageHost.Content = new DiagnosticsView(ViewModel);
            break;
        }
    }
}
