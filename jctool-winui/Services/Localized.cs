using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace JcTool.WinUI.Services;

public static class Localized
{
    private static ILocalizationService? _localization;

    public static readonly DependencyProperty KeyProperty = DependencyProperty.RegisterAttached(
        "Key",
        typeof(string),
        typeof(Localized),
        new PropertyMetadata(null, OnKeyChanged));

    public static void Initialize(ILocalizationService localization)
    {
        _localization = localization;
    }

    public static string GetKey(DependencyObject target)
    {
        return (string)target.GetValue(KeyProperty);
    }

    public static void SetKey(DependencyObject target, string value)
    {
        target.SetValue(KeyProperty, value);
    }

    private static void OnKeyChanged(DependencyObject target, DependencyPropertyChangedEventArgs args)
    {
        if (_localization is null || args.NewValue is not string key)
        {
            return;
        }

        var text = _localization.Get(key);
        switch (target)
        {
        case TextBlock textBlock:
            textBlock.Text = text;
            break;
        case TextBox textBox:
            textBox.Header = text;
            break;
        case ComboBox comboBox:
            comboBox.Header = text;
            break;
        case NumberBox numberBox:
            numberBox.Header = text;
            break;
        case ToggleSwitch toggleSwitch:
            toggleSwitch.Header = text;
            break;
        case ContentControl contentControl:
            contentControl.Content = text;
            break;
        }
    }
}
