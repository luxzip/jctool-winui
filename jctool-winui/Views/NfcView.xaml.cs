using System.ComponentModel;
using System.Text;
using JcTool.WinUI.Models;
using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace JcTool.WinUI.Views;

public sealed partial class NfcView : UserControl
{
    public NfcView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += NfcView_SizeChanged;
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += NfcView_Unloaded;
        UpdateControls();
    }

    public MainViewModel ViewModel { get; }

    private async void ScanButton_Click(object sender, RoutedEventArgs e)
    {
        if (ViewModel.IsNfcScanning)
        {
            ViewModel.StopNfcScan();
            return;
        }
        await ScanAsync();
    }

    internal async Task ScanAsync()
    {
        var tag = await ViewModel.ScanNfcAsync();
        if (tag is not null)
        {
            ApplyTag(tag);
        }
    }

    private void ApplyTag(NfcTagInfo tag)
    {
        UidText.Text = tag.Uid.Length == 0 ? "-" : string.Join(":", tag.Uid.Select(value => value.ToString("X2")));
        TagTypeText.Text = tag.TagType switch
        {
            2 when tag.TagModel > 0 => $"NTAG{tag.TagModel}",
            2 => "NTAG",
            4 => "MIFARE",
            _ => ViewModel.Text("NfcUnknownTag")
        };

        if (NdefParser.TryParse(tag.Data, out var kind, out var content))
        {
            ParsedKindText.Text = kind == "URI"
                ? ViewModel.Text("NfcUriRecord")
                : ViewModel.Text("NfcTextRecord");
            ParsedContentText.Text = content;
        }
        else if (!tag.IsNtag)
        {
            ParsedKindText.Text = ViewModel.Text("NfcMifareUnsupported");
            ParsedContentText.Text = string.Empty;
        }
        else
        {
            ParsedKindText.Text = ViewModel.Text("NfcNoNdefContent");
            ParsedContentText.Text = string.Empty;
        }
        RawContentText.Text = FormatRawPages(tag.Data);
    }

    private static string FormatRawPages(byte[] data)
    {
        if (data.Length == 0)
        {
            return string.Empty;
        }
        var output = new StringBuilder();
        for (var offset = 0; offset < data.Length; offset += 4)
        {
            var bytes = data.Skip(offset).Take(4).ToArray();
            output.Append($"{offset / 4:X2}: ");
            output.Append(string.Join(" ", bytes.Select(value => value.ToString("X2"))));
            output.Append("  |");
            output.Append(bytes.Select(value => value is >= 0x20 and <= 0x7e ? (char)value : '.').ToArray());
            output.AppendLine("|");
        }
        return output.ToString().TrimEnd();
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MainViewModel.IsBusy)
            or nameof(MainViewModel.IsNfcScanning)
            or nameof(MainViewModel.CanCancelOperation)
            or nameof(MainViewModel.SelectedController))
        {
            UpdateControls();
        }
    }

    private void UpdateControls()
    {
        var scanning = ViewModel.IsNfcScanning;
        ScanButton.IsEnabled = ViewModel.SelectedSupportsNfc
            && (!ViewModel.IsBusy || scanning && ViewModel.CanCancelOperation);
        ScanIcon.Glyph = scanning ? "\uE71A" : "\uE839";
        ScanLabel.Text = ViewModel.Text(scanning
            ? "StopNfcScan.Content"
            : "StartNfcScan.Content");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(ScanButton, ScanLabel.Text);
    }

    private void NfcView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 760;
        SummaryColumn.Width = new GridLength(1, GridUnitType.Star);
        ContentColumn.Width = narrow ? new GridLength(0) : new GridLength(3, GridUnitType.Star);
        Grid.SetRow(ParsedPanel, narrow ? 1 : 0);
        Grid.SetColumn(ParsedPanel, narrow ? 0 : 1);
        Grid.SetRow(RawPanel, narrow ? 2 : 1);
        Grid.SetColumnSpan(RawPanel, narrow ? 1 : 2);
    }

    private void NfcView_Unloaded(object sender, RoutedEventArgs e)
    {
        ViewModel.StopNfcScan();
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }
}
