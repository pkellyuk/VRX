using System;
using System.IO;

namespace Automaton.Core.Services;

/// <summary>
/// Provides centralized access to Automaton's home directory and file paths.
/// All persistent data is stored in the SQLite database at %USERPROFILE%\.automaton\config.db
/// except for documentation files which are stored per-goal in working folders.
/// </summary>
public static class AutomatonPaths
{
    /// <summary>
    /// The directory name for Automaton's home folder.
    /// </summary>
    public const string AutomatonDirectoryName = ".automaton";

    private static string? _homeDirectory;
    private static readonly object HomeDirectoryLock = new();

    /// <summary>
    /// Selects a process-local data directory before any path is first used.
    /// Does not create the directory. Repeating the same selection is harmless;
    /// changing it after initialization is refused to avoid splitting a profile.
    /// Applications which do not opt in retain the user-profile default.
    /// </summary>
    public static void ConfigureHomeDirectory(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory) || !Path.IsPathFullyQualified(directory))
            throw new ArgumentException("The data directory must be an absolute path.", nameof(directory));
        var selected = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
        lock (HomeDirectoryLock)
        {
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (_homeDirectory != null && !string.Equals(Path.TrimEndingDirectorySeparator(_homeDirectory), selected, comparison))
                throw new InvalidOperationException("The data directory has already been initialized.");
            _homeDirectory = selected;
        }
    }

    /// <summary>
    /// Gets the home directory path for Automaton (typically %USERPROFILE%\.automaton\).
    /// </summary>
    public static string HomeDirectory
    {
        get
        {
            lock (HomeDirectoryLock)
            {
                return _homeDirectory ??= Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    AutomatonDirectoryName);
            }
        }
    }

    /// <summary>
    /// Gets the path to the documentation folder for a specific goal.
    /// </summary>
    /// <param name="goalId">The goal ID.</param>
    /// <param name="workingFolder">The working folder for the goal.</param>
    /// <returns>The path to the goal's documentation folder.</returns>
    public static string GetGoalDocsPath(string goalId, string workingFolder)
    {
        AutomatonDebug.Log($"AutomatonPaths.GetGoalDocsPath: Entry - goalId={goalId}, workingFolder={workingFolder}");
        var result = Path.Combine(workingFolder, AutomatonDirectoryName, "docs", goalId);
        AutomatonDebug.Log($"AutomatonPaths.GetGoalDocsPath: Exiting - result={result}");
        return result;
    }

    /// <summary>
    /// Gets the path to the logs directory.
    /// </summary>
    /// <param name="projectDirectory">Optional project directory for project-specific logs.</param>
    /// <returns>The path to the logs directory.</returns>
    public static string GetLogsDirectory(string? projectDirectory = null)
    {
        AutomatonDebug.Log($"AutomatonPaths.GetLogsDirectory: Entry - projectDirectory={projectDirectory}");
        string result;
        if (!string.IsNullOrEmpty(projectDirectory))
        {
            result = Path.Combine(projectDirectory, AutomatonDirectoryName, "logs");
        }
        else
        {
            result = Path.Combine(HomeDirectory, "logs");
        }
        AutomatonDebug.Log($"AutomatonPaths.GetLogsDirectory: Exiting - result={result}");
        return result;
    }

    /// <summary>
    /// Ensures the home directory exists.
    /// </summary>
    public static void EnsureHomeDirectoryExists()
    {
        AutomatonDebug.Log("AutomatonPaths.EnsureHomeDirectoryExists: Entry");
        Directory.CreateDirectory(HomeDirectory);
        AutomatonDebug.Log($"AutomatonPaths.EnsureHomeDirectoryExists: Exiting - HomeDirectory={HomeDirectory}");
    }

    /// <summary>
    /// Ensures the logs directory exists.
    /// </summary>
    /// <param name="projectDirectory">Optional project directory for project-specific logs.</param>
    public static void EnsureLogsDirectoryExists(string? projectDirectory = null)
    {
        AutomatonDebug.Log($"AutomatonPaths.EnsureLogsDirectoryExists: Entry - projectDirectory={projectDirectory}");
        Directory.CreateDirectory(GetLogsDirectory(projectDirectory));
        AutomatonDebug.Log($"AutomatonPaths.EnsureLogsDirectoryExists: Exiting");
    }
}
