using System;
using System.IO;

namespace Fusion.Settings.App.Services;

/// <summary>
/// A one-line-per-event startup trace, written next to the executable.
/// </summary>
/// <remarks>
/// WinUI surfaces a fault inside the XAML host as an access violation, which a managed
/// handler cannot describe. A trace of the last completed step is then the only evidence
/// of where it happened, so it is kept deliberately small and always on.
/// </remarks>
public static class Trace
{
    private static readonly object Gate = new();
    // %LOCALAPPDATA%, not the application folder: an installed copy lives under Program
    // Files, where a standard user cannot write.
    private static readonly string Path = BuildPath();

    private static string BuildPath()
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var folder = System.IO.Path.Combine(root, "fusion-settings");
        try
        {
            Directory.CreateDirectory(folder);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }

        return System.IO.Path.Combine(folder, "trace.log");
    }

    public static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                File.AppendAllText(Path, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
            }
        }
        catch (IOException)
        {
            // Tracing must never be the reason the app fails.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    public static void Reset()
    {
        try
        {
            lock (Gate)
            {
                File.WriteAllText(Path, $"{DateTime.Now:HH:mm:ss.fff} start{Environment.NewLine}");
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
