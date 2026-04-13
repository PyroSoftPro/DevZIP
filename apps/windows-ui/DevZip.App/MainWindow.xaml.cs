using DevZip.App.Services;
using DevZip.App.ViewModels;

using System.IO;
using System.Linq;
using System.Windows;

namespace DevZip.App;

public partial class MainWindow : Window
{
    public MainWindowViewModel ViewModel { get; }

    public MainWindow()
    {
        InitializeComponent();

        ViewModel = new MainWindowViewModel(
            new CompressionJobRunner(),
            new OutputPathPolicy());

        DataContext = ViewModel;
    }

    private async void DropSurface_Drop(object sender, DragEventArgs e)
    {
        ViewModel.SetDragOver(false);

        if (!e.Data.GetDataPresent(DataFormats.FileDrop))
        {
            return;
        }

        var paths = ((string[])e.Data.GetData(DataFormats.FileDrop))
            .Where(static path => !string.IsNullOrWhiteSpace(path))
            .ToArray();

        await ViewModel.HandleDropAsync(paths);
    }

    private void DropSurface_DragEnter(object sender, DragEventArgs e)
    {
        var isValid = e.Data.GetDataPresent(DataFormats.FileDrop);
        e.Effects = isValid ? DragDropEffects.Copy : DragDropEffects.None;
        ViewModel.SetDragOver(isValid);
        e.Handled = true;
    }

    private void DropSurface_DragOver(object sender, DragEventArgs e)
    {
        var isValid = e.Data.GetDataPresent(DataFormats.FileDrop);
        e.Effects = isValid ? DragDropEffects.Copy : DragDropEffects.None;
        ViewModel.SetDragOver(isValid);
        e.Handled = true;
    }

    private void DropSurface_DragLeave(object sender, DragEventArgs e)
    {
        ViewModel.SetDragOver(false);
    }

    private async void CancelButton_Click(object sender, RoutedEventArgs e)
    {
        await ViewModel.CancelAsync();
    }

    private void OpenOutputButton_Click(object sender, RoutedEventArgs e)
    {
        ViewModel.OpenOutputFolder();
    }
}
