using DevZip.App.Services;
using DevZip.App.ViewModels;

using System.IO;
using System.Threading;
using System.Threading.Tasks;

using Xunit;

namespace DevZip.App.Tests;

public sealed class MainWindowViewModelTests
{
    [Fact]
    public async Task HandleDropAsync_CompletesWithOutputPath()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var runner = new FakeRunner(async (request, progress, cancellationToken) =>
        {
            progress.Report(new CompressionProgress(
                CompressionStage.Compressing,
                50,
                "Compressing payload",
                request.SourcePath));

            await Task.Delay(5, cancellationToken);
            return new CompressionJobResult(true, request.OutputPath, "Archive created successfully.");
        });

        var viewModel = new MainWindowViewModel(runner, new OutputPathPolicy());
        await viewModel.HandleDropAsync(new[] { source });

        Assert.Equal(CompressionState.Completed, viewModel.State);
        Assert.EndsWith(".dvz", viewModel.OutputPath);
        Assert.True(viewModel.CanOpenOutputFolder);
        Assert.Equal("Archive complete", viewModel.StatusHeadline);
    }

    [Fact]
    public async Task HandleDropAsync_RejectsMultiplePaths()
    {
        var viewModel = new MainWindowViewModel(
            new FakeRunner((_, _, _) => Task.FromResult(new CompressionJobResult(true, "", ""))),
            new OutputPathPolicy());

        await viewModel.HandleDropAsync(new[] { "one", "two" });

        Assert.Equal(CompressionState.Failed, viewModel.State);
        Assert.Equal("One item at a time", viewModel.StatusHeadline);
    }

    [Fact]
    public async Task HandleDropAsync_RejectsEmptyDrops()
    {
        var viewModel = new MainWindowViewModel(
            new FakeRunner((_, _, _) => Task.FromResult(new CompressionJobResult(true, "", ""))),
            new OutputPathPolicy());

        await viewModel.HandleDropAsync(System.Array.Empty<string>());

        Assert.Equal(CompressionState.Failed, viewModel.State);
        Assert.Equal("Nothing to compress", viewModel.StatusHeadline);
    }

    [Fact]
    public async Task HandleDropAsync_FailsWhenSourceIsMissing()
    {
        var viewModel = new MainWindowViewModel(
            new FakeRunner((_, _, _) => Task.FromResult(new CompressionJobResult(true, "", ""))),
            new OutputPathPolicy());

        await viewModel.HandleDropAsync(new[] { "Z:\\path\\that\\does\\not\\exist.txt" });

        Assert.Equal(CompressionState.Failed, viewModel.State);
        Assert.Equal("Source not found", viewModel.StatusHeadline);
    }

    [Fact]
    public async Task CancelAsync_TransitionsToCancelled()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var runner = new FakeRunner(async (_, _, cancellationToken) =>
        {
            await Task.Delay(5000, cancellationToken);
            return new CompressionJobResult(true, "unused", "unused");
        });

        var viewModel = new MainWindowViewModel(runner, new OutputPathPolicy());
        var operation = viewModel.HandleDropAsync(new[] { source });

        await Task.Delay(50);
        await viewModel.CancelAsync();
        await operation;

        Assert.Equal(CompressionState.Cancelled, viewModel.State);
        Assert.Equal("Compression cancelled", viewModel.StatusHeadline);
    }

    [Fact]
    public async Task HandleDropAsync_UsesRunnerFailureMessage()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var viewModel = new MainWindowViewModel(
            new FakeRunner((request, _, _) => Task.FromResult(
                new CompressionJobResult(false, request.OutputPath, "backend exploded"))),
            new OutputPathPolicy());

        await viewModel.HandleDropAsync(new[] { source });

        Assert.Equal(CompressionState.Failed, viewModel.State);
        Assert.Equal("Compression failed", viewModel.StatusHeadline);
        Assert.Equal("backend exploded", viewModel.StatusDetail);
    }

    [Fact]
    public async Task HandleDropAsync_UsesThrownExceptionMessage()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var viewModel = new MainWindowViewModel(
            new FakeRunner((_, _, _) => throw new InvalidOperationException("runner threw")),
            new OutputPathPolicy());

        await viewModel.HandleDropAsync(new[] { source });

        Assert.Equal(CompressionState.Failed, viewModel.State);
        Assert.Equal("Compression failed", viewModel.StatusHeadline);
        Assert.Equal("runner threw", viewModel.StatusDetail);
    }

    [Fact]
    public void OutputPathPolicy_AppendsCollisionSuffix()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "archive-source");
        Directory.CreateDirectory(source);
        File.WriteAllText(Path.Combine(tempDirectory.Path, "archive-source.dvz"), "taken");

        var output = new OutputPathPolicy().GetArchivePath(source);
        Assert.EndsWith("archive-source (2).dvz", output);
    }

    [Fact]
    public async Task SetDragOver_IgnoresDragStateWhileBusy()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var runner = new FakeRunner(async (_, _, cancellationToken) =>
        {
            await Task.Delay(5000, cancellationToken);
            return new CompressionJobResult(true, "unused", "unused");
        });

        var viewModel = new MainWindowViewModel(runner, new OutputPathPolicy());
        var operation = viewModel.HandleDropAsync(new[] { source });
        await Task.Delay(50);

        viewModel.SetDragOver(true);
        Assert.False(viewModel.IsDragOver);

        await viewModel.CancelAsync();
        await operation;
    }

    [Fact]
    public async Task CancelAsync_LeavesOpenFolderEnabledWhenOutputExists()
    {
        using var tempDirectory = new TemporaryDirectory();
        var source = Path.Combine(tempDirectory.Path, "input.txt");
        await File.WriteAllTextAsync(source, "hello");

        var runner = new FakeRunner(async (_, _, cancellationToken) =>
        {
            await Task.Delay(5000, cancellationToken);
            return new CompressionJobResult(true, "unused", "unused");
        });

        var viewModel = new MainWindowViewModel(runner, new OutputPathPolicy());
        var operation = viewModel.HandleDropAsync(new[] { source });
        await Task.Delay(50);

        File.WriteAllText(viewModel.OutputPath, "partial");
        await viewModel.CancelAsync();
        await operation;

        Assert.Equal(CompressionState.Cancelled, viewModel.State);
        Assert.True(viewModel.CanOpenOutputFolder);
    }

    [Fact]
    public async Task HandleDropAsync_IgnoresNewDropWhileBusy()
    {
        using var tempDirectory = new TemporaryDirectory();
        var firstSource = Path.Combine(tempDirectory.Path, "first.txt");
        var secondSource = Path.Combine(tempDirectory.Path, "second.txt");
        await File.WriteAllTextAsync(firstSource, "first");
        await File.WriteAllTextAsync(secondSource, "second");

        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var callCount = 0;

        var runner = new FakeRunner(async (_, _, cancellationToken) =>
        {
            Interlocked.Increment(ref callCount);
            entered.TrySetResult();
            await release.Task.WaitAsync(cancellationToken);
            return new CompressionJobResult(true, "unused", "done");
        });

        var viewModel = new MainWindowViewModel(runner, new OutputPathPolicy());
        var operation = viewModel.HandleDropAsync(new[] { firstSource });
        await entered.Task;

        await viewModel.HandleDropAsync(new[] { secondSource });
        Assert.Equal(1, callCount);
        Assert.Equal(CompressionState.Running, viewModel.State);
        Assert.Equal("Compression already running", viewModel.StatusHeadline);

        release.TrySetResult();
        await operation;
    }

    private sealed class FakeRunner : ICompressionJobRunner
    {
        private readonly Func<CompressionJobRequest, IProgress<CompressionProgress>, CancellationToken, Task<CompressionJobResult>> _handler;

        public FakeRunner(Func<CompressionJobRequest, IProgress<CompressionProgress>, CancellationToken, Task<CompressionJobResult>> handler)
        {
            _handler = handler;
        }

        public Task<CompressionJobResult> RunAsync(
            CompressionJobRequest request,
            IProgress<CompressionProgress> progress,
            CancellationToken cancellationToken)
        {
            return _handler(request, progress, cancellationToken);
        }
    }

    private sealed class TemporaryDirectory : IDisposable
    {
        public TemporaryDirectory()
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), System.IO.Path.GetRandomFileName());
            Directory.CreateDirectory(Path);
        }

        public string Path { get; }

        public void Dispose()
        {
            if (Directory.Exists(Path))
            {
                Directory.Delete(Path, recursive: true);
            }
        }
    }
}
