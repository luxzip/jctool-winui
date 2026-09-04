using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Windows.AppLifecycle;

namespace JcTool.WinUI.Views;

public sealed partial class SettingsView : UserControl
{
    private readonly LanguagePreferenceService _languagePreference;

    public SettingsView(MainViewModel viewModel, LanguagePreferenceService languagePreference)
    {
        ViewModel = viewModel;
        _languagePreference = languagePreference;
        InitializeComponent();
        LanguagePicker.SelectedItem = LanguagePicker.Items
            .OfType<ComboBoxItem>()
            .First(item => item.Tag?.ToString() == _languagePreference.CurrentPreference);
    }

    public MainViewModel ViewModel { get; }

    private void ApplyLanguageButton_Click(object sender, RoutedEventArgs e)
    {
        if (LanguagePicker.SelectedItem is not ComboBoxItem item || item.Tag is not string language)
        {
            return;
        }

        try
        {
            _languagePreference.Save(language);
            var failure = AppInstance.Restart(string.Empty);
            RestartError.Message = ViewModel.Text("RestartFailed") + $" ({failure})";
            RestartError.IsOpen = true;
        }
        catch (Exception exception)
        {
            RestartError.Message = ViewModel.Text("OperationFailedFormat").Replace("{0}", exception.Message);
            RestartError.IsOpen = true;
        }
    }
}
