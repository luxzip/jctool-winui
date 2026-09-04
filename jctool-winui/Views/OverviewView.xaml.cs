using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace JcTool.WinUI.Views;

public sealed partial class OverviewView : UserControl
{
    public OverviewView(MainViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        SizeChanged += OverviewView_SizeChanged;
        ToolTipService.SetToolTip(IdentifyButton, ViewModel.Text("IdentifyController"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(
            IdentifyButton,
            ViewModel.Text("IdentifyController"));
    }

    public MainViewModel ViewModel { get; }

    private async void IdentifyButton_Click(object sender, RoutedEventArgs e)
    {
        await ViewModel.IdentifySelectedAsync();
    }

    private void OverviewView_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 820;
        PrimaryColumn.Width = new GridLength(narrow ? 1 : 3, GridUnitType.Star);
        SecondaryColumn.Width = narrow ? new GridLength(0) : new GridLength(2, GridUnitType.Star);
        Grid.SetRow(SafetyPanel, narrow ? 1 : 0);
        Grid.SetColumn(SafetyPanel, narrow ? 0 : 1);
        Grid.SetRow(TaskPanel, narrow ? 2 : 1);
        Grid.SetColumnSpan(TaskPanel, narrow ? 1 : 2);
    }
}
