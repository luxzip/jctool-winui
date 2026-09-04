using System.ComponentModel;
using System.Globalization;
using System.Text;
using JcTool.WinUI.Models;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace JcTool.WinUI.Views;

public sealed partial class DiagnosticsView : UserControl
{
    public DiagnosticsView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += DiagnosticsView_SizeChanged;
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += DiagnosticsView_Unloaded;
        Loaded += (_, _) => UpdateEnvironment();
        UpdateEnvironment();
        UpdateControls();
    }

    public MainViewModel ViewModel { get; }

    private async void SendButton_Click(object sender, RoutedEventArgs e)
    {
        if (!TryBuildCommand(out var command, out var error))
        {
            CommandStatusText.Text = error;
            return;
        }
        var internalCommand = ModePicker.SelectedItem is ComboBoxItem item
            && string.Equals(item.Tag?.ToString(), "Internal", StringComparison.Ordinal);
        var reply = await ViewModel.SendDiagnosticCommandAsync(internalCommand, command);
        if (reply is not null)
        {
            CommandStatusText.Text = ViewModel.Text(reply.Accepted
                ? "DiagnosticAccepted"
                : "DiagnosticReplyReceived");
            ReplyText.Text = FormatReply(reply);
        }
    }

    private async void ShipmentButton_Click(object sender, RoutedEventArgs e)
    {
        await RunPresetAsync("SetShipmentConfirm", new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01 });
    }

    private async void ClearPairingButton_Click(object sender, RoutedEventArgs e)
    {
        await RunPresetAsync("ClearPairingConfirm", new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x07 });
    }

    private async void RebootPairingButton_Click(object sender, RoutedEventArgs e)
    {
        await RunPresetAsync("RebootPairingConfirm", new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x06, 0x02 });
    }

    private async void HardwarePreflightButton_Click(object sender, RoutedEventArgs e)
    {
        HardwarePreflightButton.IsEnabled = false;
        var result = await ViewModel.RunHardwarePreflightAsync();
        HardwarePreflightStatusText.Text = result is null
            ? ViewModel.StatusText
            : result.Succeeded
                ? ViewModel.Text("HardwarePreflightPassed")
                : string.Format(
                    ViewModel.Text("HardwarePreflightFailedFormat"),
                    result.ExitCode,
                    ViewModel.Text(result.StepResource));
        UpdateControls();
    }

    private async Task RunPresetAsync(string confirmationResource, byte[] command)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text("ConfirmInternalCommandTitle"),
            Content = new TextBlock
            {
                Text = ViewModel.Text(confirmationResource),
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
        var reply = await ViewModel.SendDiagnosticCommandAsync(true, PadCommand(command));
        if (reply is not null)
        {
            ReplyText.Text = FormatReply(reply);
        }
    }

    private bool TryBuildCommand(out byte[] command, out string error)
    {
        command = Array.Empty<byte>();
        error = string.Empty;
        if (!TryByte(CommandBox.Text, out var reportCommand)
            || !TryByte(SubcommandBox.Text, out var subcommand))
        {
            error = ViewModel.Text("DiagnosticByteInvalid");
            return false;
        }
        if (!TryHexBytes(RumbleBox.Text, 4, out var rumble)
            || !TryHexBytes(ArgumentsBox.Text, 38, out var arguments)
            || rumble.Length != 4)
        {
            error = ViewModel.Text("DiagnosticHexInvalid");
            return false;
        }
        command = new byte[44];
        command[0] = reportCommand;
        rumble.CopyTo(command, 1);
        command[5] = subcommand;
        arguments.CopyTo(command, 6);
        return true;
    }

    private static byte[] PadCommand(byte[] command)
    {
        var output = new byte[44];
        command.AsSpan(0, Math.Min(command.Length, output.Length)).CopyTo(output);
        return output;
    }

    private static bool TryByte(string? text, out byte value)
    {
        var normalized = text?.Trim();
        if (normalized?.StartsWith("0x", StringComparison.OrdinalIgnoreCase) == true)
        {
            normalized = normalized[2..];
        }
        return byte.TryParse(normalized, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryHexBytes(string? text, int maxLength, out byte[] bytes)
    {
        bytes = Array.Empty<byte>();
        var normalized = text?.Replace(",", " ", StringComparison.Ordinal)
            .Replace("0x", string.Empty, StringComparison.OrdinalIgnoreCase)
            .Trim();
        if (string.IsNullOrEmpty(normalized))
        {
            return true;
        }
        var tokens = normalized.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        var output = new List<byte>();
        foreach (var token in tokens)
        {
            if (token.Length != 2 || !byte.TryParse(token, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value))
            {
                return false;
            }
            output.Add(value);
        }
        if (output.Count > maxLength)
        {
            return false;
        }
        bytes = output.ToArray();
        return true;
    }

    private string FormatReply(DiagnosticReply reply)
    {
        var output = new StringBuilder();
        output.AppendLine(ViewModel.Text("DiagnosticSentLabel"));
        output.AppendLine(FormatBytes(reply.Sent));
        output.AppendLine();
        output.AppendLine(ViewModel.Text("DiagnosticReceivedLabel"));
        output.AppendLine(reply.Received.Length == 0 ? ViewModel.Text("DiagnosticNoReply") : FormatBytes(reply.Received));
        output.AppendLine();
        output.AppendLine(string.Format(
            ViewModel.Text("DiagnosticReplyStateFormat"),
            reply.Matched ? ViewModel.Text("Yes") : ViewModel.Text("No"),
            reply.Accepted ? ViewModel.Text("Yes") : ViewModel.Text("No")));
        return output.ToString().TrimEnd();
    }

    private static string FormatBytes(byte[] bytes) => string.Join(" ", bytes.Select(value => value.ToString("X2")));

    private void UpdateEnvironment()
    {
        var scale = XamlRoot?.RasterizationScale ?? 1d;
        EnvironmentText.Text = string.Format(
            ViewModel.Text("DiagnosticEnvironmentFormat"),
            Environment.Is64BitProcess ? "x64" : "x86",
            Environment.OSVersion.Version,
            (int)Math.Round(scale * 96),
            AppContext.BaseDirectory);
    }

    private void UpdateControls()
    {
        var enabled = ViewModel.HasSelectedController && !ViewModel.IsBusy;
        SendButton.IsEnabled = enabled;
        HardwarePreflightButton.IsEnabled = enabled;
        PresetPanel.IsHitTestVisible = enabled;
        PresetPanel.Opacity = enabled ? 1 : 0.55;
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.IsBusy) or nameof(MainViewModel.SelectedController))
        {
            UpdateControls();
            UpdateEnvironment();
        }
    }

    private void DiagnosticsView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 820;
        CommandColumn.Width = new GridLength(1, GridUnitType.Star);
        InfoColumn.Width = narrow ? new GridLength(0) : new GridLength(2, GridUnitType.Star);
        Grid.SetRow(PresetPanel, narrow ? 1 : 0);
        Grid.SetColumn(PresetPanel, narrow ? 0 : 1);
        Grid.SetRow(EnvironmentPanel, narrow ? 2 : 1);
        Grid.SetColumnSpan(EnvironmentPanel, narrow ? 1 : 2);
    }

    private void DiagnosticsView_Unloaded(object sender, RoutedEventArgs e)
    {
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }
}
