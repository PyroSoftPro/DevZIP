using DevZip.App.Services;

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

namespace DevZip.App.ViewModels;

public enum CompressionState
{
    Idle,
    Running,
    Completed,
    Failed,
    Cancelled
}

public sealed class MainWindowViewModel : INotifyPropertyChanged
{
    private readonly ICompressionJobRunner _runner;
    private readonly OutputPathPolicy _outputPathPolicy;
    private CancellationTokenSource? _cancellationTokenSource;

    private bool _isDragOver;
    private bool _isBusy;
    private bool _canCancel;
    private bool _canOpenOutputFolder;
    private double _progressValue;
    private string _statusHeadline = "Ready for a drop";
    private string _statusDetail = "Drag a file or folder into the target to start compression.";
    private string _outputPath = "No archive created yet.";
    private CompressionState _state = CompressionState.Idle;
    private CompressionLevel _selectedLevel = CompressionLevel.Balanced;

    public MainWindowViewModel(ICompressionJobRunner runner, OutputPathPolicy outputPathPolicy)
    {
        _runner = runner;
        _outputPathPolicy = outputPathPolicy;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public IReadOnlyList<CompressionLevel> AvailableLevels { get; } = new[]
    {
        CompressionLevel.Fast,
        CompressionLevel.Balanced,
        CompressionLevel.Max,
        CompressionLevel.Insane
    };

    public CompressionLevel SelectedLevel
    {
        get => _selectedLevel;
        set => SetField(ref _selectedLevel, value);
    }

    public CompressionState State
    {
        get => _state;
        private set => SetField(ref _state, value);
    }

    public bool IsDragOver
    {
        get => _isDragOver;
        private set => SetField(ref _isDragOver, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set => SetField(ref _isBusy, value);
    }

    public bool CanCancel
    {
        get => _canCancel;
        private set => SetField(ref _canCancel, value);
    }

    public bool CanOpenOutputFolder
    {
        get => _canOpenOutputFolder;
        private set => SetField(ref _canOpenOutputFolder, value);
    }

    public double ProgressValue
    {
        get => _progressValue;
        private set => SetField(ref _progressValue, value);
    }

    public string StatusHeadline
    {
        get => _statusHeadline;
        private set => SetField(ref _statusHeadline, value);
    }

    public string StatusDetail
    {
        get => _statusDetail;
        private set => SetField(ref _statusDetail, value);
    }

    public string OutputPath
    {
        get => _outputPath;
        private set => SetField(ref _outputPath, value);
    }

    public string DropHint => IsBusy
        ? "Compression is in progress. You can cancel, then drop something else."
        : "Files keep their extension. Folders archive to <name>.dvz beside the source.";

    public void SetDragOver(bool isDragOver)
    {
        if (IsBusy)
        {
            IsDragOver = false;
            return;
        }

        if (SetField(ref _isDragOver, isDragOver, nameof(IsDragOver)))
        {
            OnPropertyChanged(nameof(DropHint));
        }
    }

    public async Task HandleDropAsync(IReadOnlyList<string> paths)
    {
        if (IsBusy)
        {
            StatusHeadline = "Compression already running";
            StatusDetail = "Cancel the current job before dropping another item.";
            return;
        }

        if (paths is null || paths.Count == 0)
        {
            TransitionToFailure("Nothing to compress", "Drop one file or folder to begin.");
            return;
        }

        if (paths.Count > 1)
        {
            TransitionToFailure("One item at a time", "Drop a single file or folder into DevZip.");
            return;
        }

        var sourcePath = paths[0];
        if (!File.Exists(sourcePath) && !Directory.Exists(sourcePath))
        {
            TransitionToFailure("Source not found", sourcePath);
            return;
        }

        _cancellationTokenSource?.Dispose();
        _cancellationTokenSource = new CancellationTokenSource();

        SetDragOver(false);
        State = CompressionState.Running;
        IsBusy = true;
        CanCancel = true;
        CanOpenOutputFolder = false;
        ProgressValue = 0;
        StatusHeadline = "Preparing archive";
        StatusDetail = sourcePath;
        OutputPath = _outputPathPolicy.GetArchivePath(sourcePath);
        OnPropertyChanged(nameof(DropHint));

        var progress = new Progress<CompressionProgress>(UpdateProgress);

        try
        {
            var result = await _runner.RunAsync(
                new CompressionJobRequest(sourcePath, OutputPath, SelectedLevel),
                progress,
                _cancellationTokenSource.Token);

            if (result.Success)
            {
                State = CompressionState.Completed;
                IsBusy = false;
                CanCancel = false;
                CanOpenOutputFolder = true;
                ProgressValue = 100;
                StatusHeadline = "Archive complete";
                StatusDetail = result.Message;
                return;
            }

            TransitionToFailure("Compression failed", result.Message);
        }
        catch (OperationCanceledException)
        {
            State = CompressionState.Cancelled;
            IsBusy = false;
            CanCancel = false;
            CanOpenOutputFolder = File.Exists(OutputPath);
            StatusHeadline = "Compression cancelled";
            StatusDetail = "The current job was stopped before completion.";
            ProgressValue = 0;
        }
        catch (Exception exception)
        {
            TransitionToFailure("Compression failed", exception.Message);
        }
        finally
        {
            IsBusy = false;
            CanCancel = false;
            OnPropertyChanged(nameof(DropHint));
        }
    }

    public Task CancelAsync()
    {
        _cancellationTokenSource?.Cancel();
        return Task.CompletedTask;
    }

    public void OpenOutputFolder()
    {
        if (string.IsNullOrWhiteSpace(OutputPath))
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(Path.GetDirectoryName(OutputPath)))
        {
            return;
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = "explorer.exe",
            Arguments = $"/select,\"{OutputPath}\"",
            UseShellExecute = true
        });
    }

    private void UpdateProgress(CompressionProgress progress)
    {
        ProgressValue = progress.Percent;
        StatusHeadline = progress.Headline;
        StatusDetail = progress.Detail;
    }

    private void TransitionToFailure(string headline, string detail)
    {
        State = CompressionState.Failed;
        IsBusy = false;
        CanCancel = false;
        CanOpenOutputFolder = false;
        ProgressValue = 0;
        StatusHeadline = headline;
        StatusDetail = detail;
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
