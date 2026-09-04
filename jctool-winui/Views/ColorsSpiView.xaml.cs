using System.ComponentModel;
using System.Globalization;
using System.Text;
using System.Runtime.InteropServices.WindowsRuntime;
using JcTool.WinUI.Models;
using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using Windows.Storage;
using Windows.Storage.Pickers;
using System.IO;

namespace JcTool.WinUI.Views;

public sealed partial class ColorsSpiView : UserControl
{
    private readonly Color[] _colors = new Color[4];
    private bool _updatingEditor;
    private byte[]? _loadedBackup;
    private string? _loadedBackupHash;
    private SpiBackupValidation? _loadedBackupValidation;
    private bool _fullRestoreReady;

    public ColorsSpiView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += ColorsSpiView_SizeChanged;
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += ColorsSpiView_Unloaded;
        ToolTipService.SetToolTip(ReloadColorsButton, ViewModel.Text("ReloadColorsTooltip"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            ReloadColorsButton,
            ViewModel.Text("ReloadColorsTooltip"));
        LoadSelectedColors();
        BackupStatusText.Text = ViewModel.Text("SpiBackupNotLoaded");
        RestoreScopePicker_SelectionChanged(RestoreScopePicker, null!);
    }

    public MainViewModel ViewModel { get; }

    private void LoadSelectedColors()
    {
        var controller = ViewModel.SelectedController;
        _colors[0] = ParseColor(controller?.BodyColor) ?? Microsoft.UI.Colors.Black;
        _colors[1] = ParseColor(controller?.ButtonColor) ?? Microsoft.UI.Colors.Black;
        _colors[2] = ParseColor(controller?.LeftGripColor) ?? Microsoft.UI.Colors.Black;
        _colors[3] = ParseColor(controller?.RightGripColor) ?? Microsoft.UI.Colors.Black;
        UpdateSwatches();
        LoadEditorColor();
    }

    private void LoadEditorColor()
    {
        if (ColorTarget.SelectedIndex < 0)
        {
            return;
        }

        _updatingEditor = true;
        ColorEditor.Color = _colors[ColorTarget.SelectedIndex];
        ColorHex.Text = ToHex(_colors[ColorTarget.SelectedIndex]);
        _updatingEditor = false;
    }

    private void UpdateSwatches()
    {
        BodySwatch.Background = new SolidColorBrush(_colors[0]);
        ButtonSwatch.Background = new SolidColorBrush(_colors[1]);
        LeftGripSwatch.Background = new SolidColorBrush(_colors[2]);
        RightGripSwatch.Background = new SolidColorBrush(_colors[3]);
    }

    private void ColorTarget_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ColorEditor is not null)
        {
            LoadEditorColor();
        }
    }

    private void ColorEditor_ColorChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (_updatingEditor || ColorTarget.SelectedIndex < 0)
        {
            return;
        }

        _colors[ColorTarget.SelectedIndex] = args.NewColor;
        _updatingEditor = true;
        ColorHex.Text = ToHex(args.NewColor);
        _updatingEditor = false;
        UpdateSwatches();
    }

    private void ColorHex_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_updatingEditor || ColorTarget.SelectedIndex < 0)
        {
            return;
        }

        var color = ParseColor(ColorHex.Text);
        if (color is null)
        {
            return;
        }

        _colors[ColorTarget.SelectedIndex] = color.Value;
        _updatingEditor = true;
        ColorEditor.Color = color.Value;
        _updatingEditor = false;
        UpdateSwatches();
    }

    private async void ReloadColorsButton_Click(object sender, RoutedEventArgs e)
    {
        await ViewModel.RefreshAsync();
        LoadSelectedColors();
    }

    private async void WriteColorsButton_Click(object sender, RoutedEventArgs e)
    {
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(new TextBlock
        {
            Text = ViewModel.Text("ConfirmColorWriteLine1"),
            TextWrapping = TextWrapping.Wrap
        });
        content.Children.Add(new TextBlock
        {
            Text = ViewModel.Text("ConfirmColorWriteLine2"),
            TextWrapping = TextWrapping.Wrap,
            Foreground = (Brush)Application.Current.Resources["WarningBrush"]
        });
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text("ConfirmColorWriteTitle"),
            Content = content,
            PrimaryButtonText = ViewModel.Text("Continue"),
            CloseButtonText = ViewModel.Text("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        var bytes = _colors.SelectMany(color => new[] { color.R, color.G, color.B }).ToArray();
        if (await ViewModel.WriteSelectedColorsAsync(bytes))
        {
            LoadSelectedColors();
        }
    }

    private async void ReadSpiButton_Click(object sender, RoutedEventArgs e)
    {
        var addressText = SpiAddress.Text.Trim();
        if (addressText.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            addressText = addressText[2..];
        }
        if (!uint.TryParse(addressText, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var address)
            || address > 0x7ffff)
        {
            SpiOutput.Text = ViewModel.Text("InvalidSpiAddress");
            return;
        }

        var bytes = await ViewModel.ReadSelectedSpiAsync(address, (int)SpiLength.Value);
        if (bytes is not null)
        {
            SpiOutput.Text = FormatHex(bytes, address);
        }
    }

    private async void CreateBackupButton_Click(object sender, RoutedEventArgs e)
    {
        if (App.MainWindowInstance is null)
        {
            return;
        }
        var backup = await ViewModel.CreateSpiBackupAsync();
        if (backup is null)
        {
            return;
        }

        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            SuggestedFileName = $"SPI_{ViewModel.SelectedController?.SerialNumber ?? "backup"}"
        };
        picker.FileTypeChoices.Add("SPI backup", new List<string> { ".bin" });
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker,
            WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindowInstance));
        var file = await picker.PickSaveFileAsync();
        if (file is null)
        {
            return;
        }

        await FileIO.WriteBytesAsync(file, backup);
        var hash = SpiBackupValidator.ComputeSha256(backup);
        await File.WriteAllTextAsync(file.Path + ".sha256", hash);
        LoadBackupState(backup, hash);
        BackupStatusText.Text = ViewModel.Text("SpiBackupCreated");
    }

    private async void LoadBackupButton_Click(object sender, RoutedEventArgs e)
    {
        if (App.MainWindowInstance is null)
        {
            return;
        }
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            ViewMode = PickerViewMode.List
        };
        picker.FileTypeFilter.Add(".bin");
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker,
            WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindowInstance));
        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }

        var backup = (await FileIO.ReadBufferAsync(file)).ToArray();
        var hash = File.Exists(file.Path + ".sha256")
            ? (await File.ReadAllTextAsync(file.Path + ".sha256")).Trim()
            : null;
        LoadBackupState(backup, hash);
    }

    private void LoadBackupState(byte[] backup, string? hash)
    {
        _loadedBackup = backup;
        _loadedBackupHash = hash;
        _loadedBackupValidation = ViewModel.ValidateSpiBackup(backup, hash);
        var validation = _loadedBackupValidation;
        BackupStatusText.Text = string.IsNullOrWhiteSpace(hash)
            ? ViewModel.Text("SpiBackupChecksumMissing")
            : ViewModel.Text(validation.MessageResource);
        BackupHashText.Text = string.Format(
            ViewModel.Text("SpiBackupHashFormat"),
            validation.Sha256,
            validation.HasChecksum ? ViewModel.Text("ChecksumVerified") : ViewModel.Text("ChecksumMissing"));
        UpdateRestoreState();
    }

    private async void RestoreBackupButton_Click(object sender, RoutedEventArgs e)
    {
        if (_loadedBackup is null || _loadedBackupValidation is null || !_loadedBackupValidation.IsValid)
        {
            return;
        }
        var scope = SelectedRestoreScope();
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(new TextBlock
        {
            Text = ViewModel.Text("SpiRestoreWarningLine1"),
            TextWrapping = TextWrapping.Wrap
        });
        content.Children.Add(new TextBlock
        {
            Text = ViewModel.Text(scope == SpiRestoreScope.Full
                ? "SpiRestoreFullWarningLine2"
                : "SpiRestoreWarningLine2"),
            Foreground = (Brush)Application.Current.Resources["WarningBrush"],
            TextWrapping = TextWrapping.Wrap
        });
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text("ConfirmSpiRestoreTitle"),
            Content = content,
            PrimaryButtonText = ViewModel.Text("Continue"),
            CloseButtonText = ViewModel.Text("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        RestoreBackupButton.IsEnabled = false;
        if (await ViewModel.RestoreSpiBackupAsync(
            _loadedBackup,
            scope,
            _loadedBackupHash))
        {
            LoadSelectedColors();
            BackupStatusText.Text = ViewModel.Text("SpiRestoreCompleted");
            _fullRestoreReady = scope == SpiRestoreScope.Full;
            FinalizeRestoreButton.Visibility = _fullRestoreReady
                ? Visibility.Visible
                : Visibility.Collapsed;
        }
        UpdateRestoreState();
    }

    private async void FinalizeRestoreButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text("ConfirmFullRestoreFinalizeTitle"),
            Content = new TextBlock
            {
                Text = ViewModel.Text("FullRestoreFinalizeWarning"),
                TextWrapping = TextWrapping.Wrap
            },
            PrimaryButtonText = ViewModel.Text("Continue"),
            CloseButtonText = ViewModel.Text("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }
        FinalizeRestoreButton.IsEnabled = false;
        if (await ViewModel.FinalizeFullRestoreAsync())
        {
            BackupStatusText.Text = ViewModel.Text("FullRestoreFinalized");
            _fullRestoreReady = false;
            FinalizeRestoreButton.Visibility = Visibility.Collapsed;
        }
        UpdateRestoreState();
    }

    private async void WriteSerialButton_Click(object sender, RoutedEventArgs e)
    {
        var serial = SerialNumberBox.Text.Trim();
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text("ConfirmSerialWriteTitle"),
            Content = new TextBlock
            {
                Text = ViewModel.Text("SerialWriteWarning"),
                TextWrapping = TextWrapping.Wrap
            },
            PrimaryButtonText = ViewModel.Text("Continue"),
            CloseButtonText = ViewModel.Text("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await ViewModel.WriteSerialNumberAsync(serial);
        }
    }

    private void RestoreScopePicker_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SerialNumberBox is null || WriteSerialButton is null)
        {
            return;
        }
        var isSerial = SelectedRestoreScope() == SpiRestoreScope.SerialNumber;
        SerialNumberBox.Visibility = isSerial ? Visibility.Visible : Visibility.Collapsed;
        WriteSerialButton.Visibility = isSerial ? Visibility.Visible : Visibility.Collapsed;
        UpdateRestoreState();
    }

    private SpiRestoreScope SelectedRestoreScope()
    {
        return RestoreScopePicker.SelectedItem is ComboBoxItem item
            && Enum.TryParse<SpiRestoreScope>(item.Tag?.ToString(), out var scope)
            ? scope
            : SpiRestoreScope.Colors;
    }

    private void UpdateRestoreState()
    {
        var valid = _loadedBackupValidation?.IsValid == true;
        var serialRestore = SelectedRestoreScope() == SpiRestoreScope.SerialNumber;
        RestoreBackupButton.IsEnabled = valid
            && !string.IsNullOrWhiteSpace(_loadedBackupHash)
            && ViewModel.HasSelectedController
            && (!serialRestore || ViewModel.SelectedController?.ProductId != 0x2009)
            && ViewModel.SelectedController?.ProductId != 0;
        FinalizeRestoreButton.IsEnabled = _fullRestoreReady
            && ViewModel.HasSelectedController
            && !ViewModel.IsBusy;
        WriteSerialButton.IsEnabled = ViewModel.HasSelectedController
            && ViewModel.SelectedController?.ProductId != 0x2009
            && !ViewModel.IsBusy;
        CreateBackupButton.IsEnabled = ViewModel.HasSelectedController && !ViewModel.IsBusy;
        LoadBackupButton.IsEnabled = !ViewModel.IsBusy;
    }

    private static string FormatHex(byte[] bytes, uint baseAddress)
    {
        var output = new StringBuilder();
        for (var index = 0; index < bytes.Length; index += 16)
        {
            output.Append($"{baseAddress + index:X5}  ");
            output.AppendLine(string.Join(" ", bytes.Skip(index).Take(16).Select(value => value.ToString("X2"))));
        }
        return output.ToString().TrimEnd();
    }

    private static Color? ParseColor(string? text)
    {
        var value = text?.Trim().TrimStart('#');
        return value?.Length == 6 && uint.TryParse(value, NumberStyles.HexNumber,
            CultureInfo.InvariantCulture, out var parsed)
            ? Color.FromArgb(255, (byte)(parsed >> 16), (byte)(parsed >> 8), (byte)parsed)
            : null;
    }

    private static string ToHex(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.SelectedController))
        {
            _fullRestoreReady = false;
            FinalizeRestoreButton.Visibility = Visibility.Collapsed;
            LoadSelectedColors();
            if (_loadedBackup is not null)
            {
                _loadedBackupValidation = ViewModel.ValidateSpiBackup(_loadedBackup, _loadedBackupHash);
                BackupStatusText.Text = ViewModel.Text(_loadedBackupValidation.MessageResource);
            }
            UpdateRestoreState();
        }
        else if (e.PropertyName is nameof(MainViewModel.IsBusy)
            or nameof(MainViewModel.CanCancelOperation))
        {
            UpdateRestoreState();
        }
    }

    private void ColorsSpiView_Unloaded(object sender, RoutedEventArgs e)
    {
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }

    private void ColorsSpiView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 900;
        EditorColumn.Width = new GridLength(narrow ? 1 : 3, GridUnitType.Star);
        ToolsColumn.Width = narrow ? new GridLength(0) : new GridLength(2, GridUnitType.Star);
        Grid.SetRow(SpiPanel, narrow ? 1 : 0);
        Grid.SetColumn(SpiPanel, narrow ? 0 : 1);
    }
}
