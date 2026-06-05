using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace DevZip.App.Services;

public enum CompressionStage
{
    Scanning,
    Compressing,
    Finalizing,
    Completed
}

public sealed record CompressionProgress(
    CompressionStage Stage,
    double Percent,
    string Headline,
    string Detail);

// Mirrors the native CLI's --level flag.  Higher levels enable more (and more
// expensive) recompressors/backends for smaller output.
public enum CompressionLevel
{
    Fast,
    Balanced,
    Max,
    Insane
}

public sealed record CompressionJobRequest(
    string SourcePath,
    string OutputPath,
    CompressionLevel Level = CompressionLevel.Balanced);

public sealed record CompressionJobResult(bool Success, string OutputPath, string Message);

public interface ICompressionJobRunner
{
    Task<CompressionJobResult> RunAsync(
        CompressionJobRequest request,
        IProgress<CompressionProgress> progress,
        CancellationToken cancellationToken);
}

public sealed class CompressionJobRunner : ICompressionJobRunner
{
    public async Task<CompressionJobResult> RunAsync(
        CompressionJobRequest request,
        IProgress<CompressionProgress> progress,
        CancellationToken cancellationToken)
    {
        progress.Report(new CompressionProgress(
            CompressionStage.Scanning,
            12,
            "Scanning source",
            $"Preparing {Path.GetFileName(request.SourcePath)}"));

        await Task.Delay(160, cancellationToken).ConfigureAwait(false);

        var command = ResolveCommand(request);
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = command.FileName,
                Arguments = command.Arguments,
                WorkingDirectory = command.WorkingDirectory,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true
            }
        };

        if (!process.Start())
        {
            throw new InvalidOperationException("Failed to start compression process.");
        }

        progress.Report(new CompressionProgress(
            CompressionStage.Compressing,
            68,
            "Compressing payload",
            command.ModeDescription));

        using var registration = cancellationToken.Register(() =>
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
            }
            catch
            {
                // Ignore kill failures during shutdown.
            }
        });

        var stdoutTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderrTask = process.StandardError.ReadToEndAsync(cancellationToken);

        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        var stdout = await stdoutTask.ConfigureAwait(false);
        var stderr = await stderrTask.ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            var message = string.IsNullOrWhiteSpace(stderr) ? stdout : stderr;
            return new CompressionJobResult(false, request.OutputPath, message.Trim());
        }

        progress.Report(new CompressionProgress(
            CompressionStage.Finalizing,
            94,
            "Finalizing archive",
            "Writing the final DVZ container"));

        await Task.Delay(120, cancellationToken).ConfigureAwait(false);

        progress.Report(new CompressionProgress(
            CompressionStage.Completed,
            100,
            "Archive ready",
            request.OutputPath));

        return new CompressionJobResult(true, request.OutputPath, "Archive created successfully.");
    }

    private static CompressionCommand ResolveCommand(CompressionJobRequest request)
    {
        var repoRoot = TryFindRepoRoot();
        if (repoRoot is null)
        {
            throw new InvalidOperationException(
                "The DevZip repository root could not be located from the current application directory.");
        }

        var nativeCandidates = new[]
        {
            Path.Combine(repoRoot, "native", "engine", "build", "devzip_cli.exe"),
            Path.Combine(repoRoot, "native", "engine", "build", "Release", "devzip_cli.exe"),
            Path.Combine(repoRoot, "native", "engine", "out", "devzip_cli.exe")
        };

        var levelName = request.Level.ToString().ToLowerInvariant();

        var nativeCli = nativeCandidates.FirstOrDefault(File.Exists);
        if (!string.IsNullOrWhiteSpace(nativeCli))
        {
            return new CompressionCommand(
                nativeCli,
                $"compress \"{request.SourcePath}\" \"{request.OutputPath}\" --level {levelName}",
                repoRoot,
                $"Using the native engine ({levelName} level)");
        }

        var allowReferenceCodec = string.Equals(
            Environment.GetEnvironmentVariable("DEVZIP_USE_REFERENCE_CODEC"),
            "1",
            StringComparison.OrdinalIgnoreCase);

        if (!allowReferenceCodec)
        {
            throw new InvalidOperationException(
                "The native DevZip CLI was not found. Build `devzip_cli.exe`, or set DEVZIP_USE_REFERENCE_CODEC=1 for the development-only reference codec.");
        }

        var referenceScript = Path.Combine(repoRoot, "native", "engine", "reference", "devzip_reference.py");
        return new CompressionCommand(
            "python",
            $"\"{referenceScript}\" compress \"{request.SourcePath}\" \"{request.OutputPath}\"",
            repoRoot,
            "Using the development-only reference codec");
    }

    private static string? TryFindRepoRoot()
    {
        var candidates = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            AppContext.BaseDirectory,
            Environment.CurrentDirectory
        };

        foreach (var seed in candidates)
        {
            var current = new DirectoryInfo(seed);
            while (current is not null)
            {
                var scriptPath = Path.Combine(current.FullName, "native", "engine", "reference", "devzip_reference.py");
                if (File.Exists(scriptPath))
                {
                    return current.FullName;
                }
                current = current.Parent;
            }
        }

        return null;
    }

    private sealed record CompressionCommand(
        string FileName,
        string Arguments,
        string WorkingDirectory,
        string ModeDescription);
}
