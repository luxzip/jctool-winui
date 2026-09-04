using System.Collections.ObjectModel;
using System.ComponentModel;
using JcTool.WinUI.Models;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace JcTool.WinUI.Views;

public sealed partial class InputTestView : UserControl
{
    public InputTestView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        ButtonStates = CreateButtonStates();
        InitializeComponent();
        SizeChanged += InputTestView_SizeChanged;
        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        Unloaded += InputTestView_Unloaded;
        UpdateButtonAvailability();
        UpdateStreamButton();
    }

    public MainViewModel ViewModel { get; }
    public ObservableCollection<InputButtonState> ButtonStates { get; }

    private async void InputStreamButton_Click(object sender, RoutedEventArgs e)
    {
        if (ViewModel.IsInputStreaming)
        {
            ViewModel.StopInputStream();
            return;
        }

        UpdateStreamButton(streaming: true);
        try
        {
            await ViewModel.StreamInputAsync(ApplySnapshot);
        }
        finally
        {
            UpdateStreamButton();
        }
    }

    internal async Task RefreshAsync()
    {
        var snapshot = await ViewModel.ReadInputAsync();
        if (snapshot is not null)
        {
            ApplySnapshot(snapshot);
        }
    }

    internal void ApplySnapshot(InputSnapshot snapshot)
    {
        ButtonsValue.Text = $"{snapshot.Buttons & 0xff:X2} {(snapshot.Buttons >> 8) & 0xff:X2} {(snapshot.Buttons >> 16) & 0xff:X2}";
        ConnectionValue.Text = snapshot.ConnectionType switch
        {
            3 => "Bluetooth",
            0 => "USB",
            _ => $"{snapshot.ConnectionType:X}"
        };
        BatteryValue.Text = $"{snapshot.BatteryLevel}/4";
        ChargingValue.Text = ViewModel.Text(snapshot.Charging ? "Yes" : "No");
        LeftStickValue.Text = $"X {snapshot.LeftX:X3}  Y {snapshot.LeftY:X3}";
        RightStickValue.Text = $"X {snapshot.RightX:X3}  Y {snapshot.RightY:X3}";
        LeftXBar.Value = snapshot.LeftX;
        LeftYBar.Value = snapshot.LeftY;
        RightXBar.Value = snapshot.RightX;
        RightYBar.Value = snapshot.RightY;
        AccelerationValue.Text = $"X {snapshot.AccelerationX,6}   Y {snapshot.AccelerationY,6}   Z {snapshot.AccelerationZ,6}";
        GyroscopeValue.Text = $"X {snapshot.GyroscopeX,6}   Y {snapshot.GyroscopeY,6}   Z {snapshot.GyroscopeZ,6}";
        foreach (var button in ButtonStates)
        {
            button.Update(snapshot.Buttons, ViewModel.SelectedController?.ProductId ?? 0);
        }
    }

    private void InputTestView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 820;
        PrimaryColumn.Width = new GridLength(1, GridUnitType.Star);
        SecondaryColumn.Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        Grid.SetRow(SticksPanel, narrow ? 1 : 0);
        Grid.SetColumn(SticksPanel, narrow ? 0 : 1);
        Grid.SetRow(MotionPanel, narrow ? 2 : 1);
        Grid.SetColumnSpan(MotionPanel, narrow ? 1 : 2);
        Grid.SetRow(ButtonsPanel, narrow ? 3 : 2);
        Grid.SetColumnSpan(ButtonsPanel, narrow ? 1 : 2);
    }

    private void InputTestView_Unloaded(object sender, RoutedEventArgs e)
    {
        ViewModel.StopInputStream();
        ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
    }

    private void UpdateStreamButton(bool? streaming = null)
    {
        var isStreaming = streaming ?? ViewModel.IsInputStreaming;
        var resource = isStreaming ? "StopInputStream.Content" : "StartInputStream.Content";
        InputStreamGlyph.Glyph = isStreaming ? "\uE71A" : "\uE768";
        InputStreamLabel.Text = ViewModel.Text(resource);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            InputStreamButton,
            InputStreamLabel.Text);
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.SelectedController))
        {
            UpdateButtonAvailability();
        }
    }

    private void UpdateButtonAvailability()
    {
        foreach (var button in ButtonStates)
        {
            button.Update(0, ViewModel.SelectedController?.ProductId ?? 0);
        }
    }

    private ObservableCollection<InputButtonState> CreateButtonStates()
    {
        const int left = 1;
        const int right = 2;
        const int pro = 4;
        return new ObservableCollection<InputButtonState>
        {
            new("A", 1u << 3, right | pro),
            new("B", 1u << 2, right | pro),
            new("X", 1u << 1, right | pro),
            new("Y", 1u, right | pro),
            new("L", 1u << 22, left | pro),
            new("R", 1u << 6, right | pro),
            new("ZL", 1u << 23, left | pro),
            new("ZR", 1u << 7, right | pro),
            new(ViewModel.Text("ButtonMinus"), 1u << 8, left | pro),
            new(ViewModel.Text("ButtonPlus"), 1u << 9, right | pro),
            new(ViewModel.Text("ButtonLeftStick"), 1u << 11, left | pro),
            new(ViewModel.Text("ButtonRightStick"), 1u << 10, right | pro),
            new(ViewModel.Text("ButtonUp"), 1u << 17, left | pro),
            new(ViewModel.Text("ButtonDown"), 1u << 16, left | pro),
            new(ViewModel.Text("ButtonLeft"), 1u << 19, left | pro),
            new(ViewModel.Text("ButtonRight"), 1u << 18, left | pro),
            new(ViewModel.Text("ButtonHome"), 1u << 12, right | pro),
            new(ViewModel.Text("ButtonCapture"), 1u << 13, left | pro),
            new("SL (L)", 1u << 21, left),
            new("SR (L)", 1u << 20, left),
            new("SL (R)", 1u << 5, right),
            new("SR (R)", 1u << 4, right)
        };
    }
}
