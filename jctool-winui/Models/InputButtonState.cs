using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace JcTool.WinUI.Models;

public sealed class InputButtonState : INotifyPropertyChanged
{
    private bool _isAvailable;
    private bool _isPressed;

    public InputButtonState(string label, uint mask, int supportedProducts)
    {
        Label = label;
        Mask = mask;
        SupportedProducts = supportedProducts;
    }

    public string Label { get; }
    public uint Mask { get; }
    public int SupportedProducts { get; }

    public bool IsAvailable
    {
        get => _isAvailable;
        private set => SetField(ref _isAvailable, value);
    }

    public bool IsPressed
    {
        get => _isPressed;
        private set => SetField(ref _isPressed, value);
    }

    public void Update(uint buttons, uint productId)
    {
        var product = productId switch
        {
            0x2006 => 1,
            0x2007 => 2,
            0x2009 => 4,
            _ => 0
        };
        IsAvailable = (SupportedProducts & product) != 0;
        IsPressed = IsAvailable && (buttons & Mask) != 0;
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
