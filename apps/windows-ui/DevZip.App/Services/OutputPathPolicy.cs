using System;
using System.IO;

namespace DevZip.App.Services;

public sealed class OutputPathPolicy
{
    public string GetArchivePath(string sourcePath)
    {
        if (string.IsNullOrWhiteSpace(sourcePath))
        {
            throw new ArgumentException("A source path is required.", nameof(sourcePath));
        }

        var trimmed = sourcePath.Trim();
        var parent = Path.GetDirectoryName(trimmed);
        if (string.IsNullOrWhiteSpace(parent))
        {
            parent = Directory.GetCurrentDirectory();
        }

        var sourceName = Path.GetFileName(trimmed.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (string.IsNullOrWhiteSpace(sourceName))
        {
            sourceName = "archive";
        }

        var candidate = Path.Combine(parent, $"{sourceName}.dvz");
        if (!File.Exists(candidate))
        {
            return candidate;
        }

        for (var suffix = 2; suffix < 1000; suffix++)
        {
            candidate = Path.Combine(parent, $"{sourceName} ({suffix}).dvz");
            if (!File.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new IOException("Unable to determine an available archive path.");
    }
}
