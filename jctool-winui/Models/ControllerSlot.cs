using System.ComponentModel;
using System.Runtime.CompilerServices;
using JcTool.WinUI.Services;

namespace JcTool.WinUI.Models;

public sealed class ControllerSlot : INotifyPropertyChanged
{
    private string _displayName;
    private string _serialNumber;
    private string _statusText;
    private string _firmware;
    private string _macAddress;
    private string _batteryText;
    private string _temperatureText;
    private string _bodyColor = "#000000";
    private string _buttonColor = "#000000";
    private string _leftGripColor = "#000000";
    private string _rightGripColor = "#000000";
    private bool _isConnected;
    private bool _isSelected;

    public ControllerSlot(int slotIndex, ILocalizationService localization)
    {
        SlotIndex = slotIndex;
        SlotLabel = localization.Format("SlotFormat", slotIndex + 1);
        _displayName = localization.Get("EmptySlot");
        _serialNumber = localization.Get("NoControllerDetected");
        _statusText = localization.Get("Disconnected");
        _firmware = localization.Get("NotReadValue");
        _macAddress = localization.Get("NotReadValue");
        _batteryText = localization.Get("NotReadValue");
        _temperatureText = localization.Get("NotReadValue");
    }

    public int SlotIndex { get; }
    public string SlotLabel { get; }
    public string DeviceKey { get; set; } = string.Empty;
    public uint ProductId { get; set; }

    public string DisplayName
    {
        get => _displayName;
        set => SetField(ref _displayName, value);
    }

    public string SerialNumber
    {
        get => _serialNumber;
        set => SetField(ref _serialNumber, value);
    }

    public string StatusText
    {
        get => _statusText;
        set => SetField(ref _statusText, value);
    }

    public bool IsConnected
    {
        get => _isConnected;
        set => SetField(ref _isConnected, value);
    }

    public bool IsSelected
    {
        get => _isSelected;
        set => SetField(ref _isSelected, value);
    }

    public string Firmware
    {
        get => _firmware;
        set => SetField(ref _firmware, value);
    }

    public string MacAddress
    {
        get => _macAddress;
        set => SetField(ref _macAddress, value);
    }

    public string BatteryText
    {
        get => _batteryText;
        set => SetField(ref _batteryText, value);
    }

    public string TemperatureText
    {
        get => _temperatureText;
        set => SetField(ref _temperatureText, value);
    }

    public string BodyColor
    {
        get => _bodyColor;
        set => SetField(ref _bodyColor, value);
    }

    public string ButtonColor
    {
        get => _buttonColor;
        set => SetField(ref _buttonColor, value);
    }

    public string LeftGripColor
    {
        get => _leftGripColor;
        set => SetField(ref _leftGripColor, value);
    }

    public string RightGripColor
    {
        get => _rightGripColor;
        set => SetField(ref _rightGripColor, value);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
