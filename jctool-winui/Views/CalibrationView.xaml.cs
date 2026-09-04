using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace JcTool.WinUI.Views;

public sealed partial class CalibrationView : UserControl
{
    private string? _validatedDeviceKey;
    private string? _validatedMacAddress;
    private bool _leftStickSupported;
    private bool _rightStickSupported;

    public CalibrationView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += CalibrationView_SizeChanged;
        SetToggleLabels(EnableLeftStick);
        SetToggleLabels(EnableRightStick);
        SetToggleLabels(EnableSensor);
        ValidationStatus.Text = ViewModel.Text("CalibrationNotRead");
    }

    public MainViewModel ViewModel { get; }

    private void SetToggleLabels(ToggleSwitch toggle)
    {
        toggle.OnContent = ViewModel.Text("Enabled");
        toggle.OffContent = ViewModel.Text("Disabled");
    }

    private async void ReadCalibrationButton_Click(object sender, RoutedEventArgs e)
    {
        await ReadCalibrationAsync();
    }

    internal async Task ReadCalibrationAsync()
    {
        var controller = ViewModel.SelectedController;
        if (controller is null)
        {
            ValidationStatus.Text = ViewModel.Text("SelectControllerFirst");
            return;
        }

        WriteCalibrationButton.IsEnabled = false;
        WriteParametersButton.IsEnabled = false;
        _validatedDeviceKey = null;
        _validatedMacAddress = null;

        var stickData = await ViewModel.ReadSelectedSpiAsync(0x8010, 22);
        var sensorData = await ViewModel.ReadSelectedSpiAsync(0x8026, 26);
        var primaryParameters = await ViewModel.ReadSelectedSpiAsync(0x6089, 3);
        var secondaryParameters = controller.ProductId == 0x2009
            ? await ViewModel.ReadSelectedSpiAsync(0x609b, 3)
            : null;
        if (stickData is null || sensorData is null || primaryParameters is null
            || controller.ProductId == 0x2009 && secondaryParameters is null)
        {
            ValidationStatus.Text = ViewModel.Text("CalibrationReadFailed");
            return;
        }

        _leftStickSupported = controller.ProductId != 0x2007;
        _rightStickSupported = controller.ProductId != 0x2006;
        SetStickControlsEnabled(true, _leftStickSupported);
        SetStickControlsEnabled(false, _rightStickSupported);
        EnableLeftStick.IsOn = ReadUInt16(stickData, 0) == 0xa1b2;
        EnableRightStick.IsOn = ReadUInt16(stickData, 11) == 0xa1b2;
        if (EnableLeftStick.IsOn)
        {
            LoadStick(stickData, 2, 5, 8,
                LeftXMin, LeftXCenter, LeftXMax, LeftYMin, LeftYCenter, LeftYMax);
        }
        if (EnableRightStick.IsOn)
        {
            LoadStick(stickData, 19, 13, 16,
                RightXMin, RightXCenter, RightXMax, RightYMin, RightYCenter, RightYMax);
        }

        EnableSensor.IsOn = ReadUInt16(sensorData, 0) == 0xa1b2;
        if (EnableSensor.IsOn)
        {
            AccX.Value = ReadUInt16(sensorData, 2);
            AccY.Value = ReadUInt16(sensorData, 4);
            AccZ.Value = ReadUInt16(sensorData, 6);
            GyroX.Value = ReadUInt16(sensorData, 14);
            GyroY.Value = ReadUInt16(sensorData, 16);
            GyroZ.Value = ReadUInt16(sensorData, 18);
        }

        var primary = DecodeStickPair(primaryParameters, 0);
        PrimaryDeadzone.Value = primary.X;
        PrimaryRange.Value = primary.Y;
        var isProController = controller.ProductId == 0x2009;
        SecondaryDeadzone.IsEnabled = isProController;
        SecondaryRange.IsEnabled = isProController;
        if (secondaryParameters is not null)
        {
            var secondary = DecodeStickPair(secondaryParameters, 0);
            SecondaryDeadzone.Value = secondary.X;
            SecondaryRange.Value = secondary.Y;
        }

        _validatedDeviceKey = controller.DeviceKey;
        _validatedMacAddress = controller.MacAddress;
        WriteCalibrationButton.IsEnabled = true;
        WriteParametersButton.IsEnabled = true;
        ValidationStatus.Text = ViewModel.Text("CalibrationReady");
    }

    private async void WriteCalibrationButton_Click(object sender, RoutedEventArgs e)
    {
        if (!IdentityIsValid() || !StickValuesAreOrdered())
        {
            ValidationStatus.Text = ViewModel.Text(
                IdentityIsValid() ? "CalibrationOrderInvalid" : "CalibrationIdentityChanged");
            return;
        }
        if (!await ConfirmWriteAsync("ConfirmCalibrationTitle", "ConfirmCalibrationLine1", "ConfirmCalibrationLine2"))
        {
            return;
        }

        var controllerKey = _validatedDeviceKey;
        var sticks = Enumerable.Repeat((byte)0xff, 22).ToArray();
        if (EnableLeftStick.IsOn && _leftStickSupported)
        {
            Array.Clear(sticks, 0, 11);
            WriteUInt16(sticks, 0, 0xa1b2);
            EncodeStickPair(sticks, 5, Value(LeftXCenter), Value(LeftYCenter));
            EncodeStickPair(sticks, 2,
                Value(LeftXMax) - Value(LeftXCenter),
                Value(LeftYMax) - Value(LeftYCenter));
            EncodeStickPair(sticks, 8,
                Value(LeftXCenter) - Value(LeftXMin),
                Value(LeftYCenter) - Value(LeftYMin));
        }
        if (EnableRightStick.IsOn && _rightStickSupported)
        {
            Array.Clear(sticks, 11, 11);
            WriteUInt16(sticks, 11, 0xa1b2);
            EncodeStickPair(sticks, 13, Value(RightXCenter), Value(RightYCenter));
            EncodeStickPair(sticks, 19,
                Value(RightXMax) - Value(RightXCenter),
                Value(RightYMax) - Value(RightYCenter));
            EncodeStickPair(sticks, 16,
                Value(RightXCenter) - Value(RightXMin),
                Value(RightYCenter) - Value(RightYMin));
        }

        var sensors = EnableSensor.IsOn ? new byte[26] : Enumerable.Repeat((byte)0xff, 26).ToArray();
        if (EnableSensor.IsOn)
        {
            WriteUInt16(sensors, 0, 0xa1b2);
            WriteUInt16(sensors, 2, Value(AccX));
            WriteUInt16(sensors, 4, Value(AccY));
            WriteUInt16(sensors, 6, Value(AccZ));
            WriteUInt16(sensors, 14, Value(GyroX));
            WriteUInt16(sensors, 16, Value(GyroY));
            WriteUInt16(sensors, 18, Value(GyroZ));
        }

        WriteCalibrationButton.IsEnabled = false;
        WriteParametersButton.IsEnabled = false;
        var firstWritten = await ViewModel.WriteSelectedSpiAsync(0x8010, sticks, "CalibrationWritten");
        var secondWritten = firstWritten && ViewModel.SelectedController?.DeviceKey == controllerKey
            && await ViewModel.WriteSelectedSpiAsync(0x8026, sensors, "CalibrationWritten");
        InvalidateReadState();
        ValidationStatus.Text = secondWritten
            ? ViewModel.Text("CalibrationWritten")
            : ViewModel.Text("CalibrationWriteFailed");
    }

    private async void WriteParametersButton_Click(object sender, RoutedEventArgs e)
    {
        if (!IdentityIsValid())
        {
            ValidationStatus.Text = ViewModel.Text("CalibrationIdentityChanged");
            return;
        }
        if (!await ConfirmWriteAsync("ConfirmParametersTitle", "ConfirmParametersLine1", "ConfirmParametersLine2"))
        {
            return;
        }

        var controller = ViewModel.SelectedController!;
        var primary = new byte[3];
        EncodeStickPair(primary, 0, Value(PrimaryDeadzone), Value(PrimaryRange));
        var written = await ViewModel.WriteSelectedSpiAsync(0x6089, primary, "ParametersWritten");
        if (written && controller.ProductId == 0x2009)
        {
            var secondary = new byte[3];
            EncodeStickPair(secondary, 0, Value(SecondaryDeadzone), Value(SecondaryRange));
            written = await ViewModel.WriteSelectedSpiAsync(0x609b, secondary, "ParametersWritten");
        }
        InvalidateReadState();
        ValidationStatus.Text = written
            ? ViewModel.Text("ParametersWritten")
            : ViewModel.Text("CalibrationWriteFailed");
    }

    private async Task<bool> ConfirmWriteAsync(string title, string line1, string line2)
    {
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(new TextBlock { Text = ViewModel.Text(line1), TextWrapping = TextWrapping.Wrap });
        content.Children.Add(new TextBlock
        {
            Text = ViewModel.Text(line2),
            TextWrapping = TextWrapping.Wrap,
            Foreground = (Brush)Application.Current.Resources["WarningBrush"]
        });
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = ViewModel.Text(title),
            Content = content,
            PrimaryButtonText = ViewModel.Text("Continue"),
            CloseButtonText = ViewModel.Text("Cancel"),
            DefaultButton = ContentDialogButton.Close
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    private bool IdentityIsValid()
    {
        var controller = ViewModel.SelectedController;
        return controller is not null
            && controller.DeviceKey == _validatedDeviceKey
            && controller.MacAddress == _validatedMacAddress;
    }

    private bool StickValuesAreOrdered()
    {
        return (!EnableLeftStick.IsOn || !_leftStickSupported
                || Ordered(LeftXMin, LeftXCenter, LeftXMax) && Ordered(LeftYMin, LeftYCenter, LeftYMax))
            && (!EnableRightStick.IsOn || !_rightStickSupported
                || Ordered(RightXMin, RightXCenter, RightXMax) && Ordered(RightYMin, RightYCenter, RightYMax));
    }

    private void SetStickControlsEnabled(bool left, bool enabled)
    {
        var controls = left
            ? new Control[] { EnableLeftStick, LeftXMin, LeftXCenter, LeftXMax, LeftYMin, LeftYCenter, LeftYMax }
            : new Control[] { EnableRightStick, RightXMin, RightXCenter, RightXMax, RightYMin, RightYCenter, RightYMax };
        foreach (var control in controls)
        {
            control.IsEnabled = enabled;
        }
        (left ? LeftStickPanel : RightStickPanel).Opacity = enabled ? 1 : 0.55;
    }

    private static bool Ordered(NumberBox minimum, NumberBox center, NumberBox maximum)
    {
        return minimum.Value < center.Value && center.Value < maximum.Value;
    }

    private void InvalidateReadState()
    {
        _validatedDeviceKey = null;
        _validatedMacAddress = null;
        WriteCalibrationButton.IsEnabled = false;
        WriteParametersButton.IsEnabled = false;
    }

    private static void LoadStick(
        byte[] data,
        int plusOffset,
        int centerOffset,
        int minusOffset,
        NumberBox xMinimum,
        NumberBox xCenter,
        NumberBox xMaximum,
        NumberBox yMinimum,
        NumberBox yCenter,
        NumberBox yMaximum)
    {
        var plus = DecodeStickPair(data, plusOffset);
        var center = DecodeStickPair(data, centerOffset);
        var minus = DecodeStickPair(data, minusOffset);
        xMinimum.Value = Math.Max(0, center.X - minus.X);
        xCenter.Value = center.X;
        xMaximum.Value = Math.Min(4095, center.X + plus.X);
        yMinimum.Value = Math.Max(0, center.Y - minus.Y);
        yCenter.Value = center.Y;
        yMaximum.Value = Math.Min(4095, center.Y + plus.Y);
    }

    private static (int X, int Y) DecodeStickPair(byte[] data, int offset)
    {
        return (
            data[offset] | (data[offset + 1] & 0x0f) << 8,
            data[offset + 1] >> 4 | data[offset + 2] << 4);
    }

    private static void EncodeStickPair(byte[] data, int offset, int x, int y)
    {
        x = Math.Clamp(x, 0, 4095);
        y = Math.Clamp(y, 0, 4095);
        data[offset] = (byte)x;
        data[offset + 1] = (byte)((x >> 8) & 0x0f | (y & 0x0f) << 4);
        data[offset + 2] = (byte)(y >> 4);
    }

    private static ushort ReadUInt16(byte[] data, int offset)
    {
        return (ushort)(data[offset] | data[offset + 1] << 8);
    }

    private static void WriteUInt16(byte[] data, int offset, int value)
    {
        data[offset] = (byte)value;
        data[offset + 1] = (byte)(value >> 8);
    }

    private static int Value(NumberBox numberBox)
    {
        return double.IsNaN(numberBox.Value) ? 0 : (int)numberBox.Value;
    }

    private void CalibrationView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 900;
        LeftColumn.Width = new GridLength(1, GridUnitType.Star);
        RightColumn.Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        Grid.SetRow(RightStickPanel, narrow ? 1 : 0);
        Grid.SetColumn(RightStickPanel, narrow ? 0 : 1);
        Grid.SetRow(SensorPanel, narrow ? 2 : 1);
        Grid.SetColumn(SensorPanel, 0);
        Grid.SetRow(ParametersPanel, narrow ? 3 : 1);
        Grid.SetColumn(ParametersPanel, narrow ? 0 : 1);
    }
}
