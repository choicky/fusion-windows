using System;
using System.Runtime.InteropServices;

namespace Fusion.Settings.Client;

/// <summary>
/// Endpoint naming, mirroring <c>fusion::ipc::compute_pipe_name</c> and
/// <c>logical_pipe_name</c>.
/// </summary>
/// <remarks>
/// The production name is per user and per session. A supplied <c>--pipe-name</c> must
/// stand on its own: falling back to the production pipe would silently connect to a
/// different Host than the one the caller asked for, so a malformed value is rejected
/// rather than repaired.
/// </remarks>
public static class PipeEndpoint
{
    public const string NamePrefix = "FusionHost-v1-";
    public const string DevicePathPrefix = @"\\.\pipe\";

    private static readonly char[] InvalidNameCharacters =
        { '\\', '/', ':', '*', '?', '"', '<', '>', '|' };

    /// <summary>The production endpoint for the current user and session, or null on failure.</summary>
    public static string? Production()
    {
        var sid = Native.CurrentUserSidString();
        var session = Native.CurrentSessionId();
        if (string.IsNullOrEmpty(sid) || session is null)
        {
            return null;
        }

        return NamePrefix + sid + "-" +
               session.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// Reduces a full Win32 pipe path or a bare logical name to the logical name.
    /// Returns null when the endpoint cannot name a pipe, so callers fail closed.
    /// </summary>
    public static string? Normalize(string? endpoint)
    {
        if (endpoint is null)
        {
            return null;
        }

        var name = endpoint;
        if (name.Length > DevicePathPrefix.Length &&
            name.StartsWith(DevicePathPrefix, StringComparison.OrdinalIgnoreCase))
        {
            name = name[DevicePathPrefix.Length..];
        }

        if (name.Trim().Length == 0)
        {
            return null;
        }

        foreach (var character in name)
        {
            if (character < 0x20 || Array.IndexOf(InvalidNameCharacters, character) >= 0)
            {
                return null;
            }
        }

        return name;
    }

    /// <summary>True when the logical name is the installed product's own endpoint.</summary>
    public static bool IsProduction(string? logicalName)
    {
        var production = Production();
        return production is not null && logicalName is not null &&
               string.Equals(logicalName, production, StringComparison.OrdinalIgnoreCase);
    }

    public static string DevicePath(string logicalName) => DevicePathPrefix + logicalName;
}

/// <summary>The few Win32 calls endpoint naming and server verification need.</summary>
internal static class Native
{
    private const int TokenUser = 1;
    private const uint TokenQuery = 0x0008;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private static readonly IntPtr CurrentProcess = new(-1);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(IntPtr token, int informationClass, IntPtr buffer,
                                                   int length, out int returnLength);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool ConvertSidToStringSidW(IntPtr sid, out IntPtr stringSid);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ProcessIdToSessionId(uint processId, out uint sessionId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LocalFree(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetCurrentProcessId();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint serverProcessId);

    internal static string? CurrentUserSidString() => SidStringOfTokenOf(CurrentProcess, isPseudo: true);

    internal static uint? CurrentSessionId() =>
        ProcessIdToSessionId(GetCurrentProcessId(), out var session) ? session : null;

    internal static uint? SessionIdOf(uint processId) =>
        ProcessIdToSessionId(processId, out var session) ? session : null;

    internal static uint? ServerProcessId(IntPtr pipe) =>
        GetNamedPipeServerProcessId(pipe, out var pid) ? pid : null;

    /// <summary>The user SID of another process, or null when it cannot be read.</summary>
    internal static string? ProcessUserSidString(uint processId)
    {
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == IntPtr.Zero)
        {
            return null;
        }

        try
        {
            return SidStringOfTokenOf(process, isPseudo: false);
        }
        finally
        {
            CloseHandle(process);
        }
    }

    private static string? SidStringOfTokenOf(IntPtr process, bool isPseudo)
    {
        if (!OpenProcessToken(process, TokenQuery, out var token))
        {
            return null;
        }

        try
        {
            GetTokenInformation(token, TokenUser, IntPtr.Zero, 0, out var length);
            if (length <= 0)
            {
                return null;
            }

            var buffer = Marshal.AllocHGlobal(length);
            try
            {
                if (!GetTokenInformation(token, TokenUser, buffer, length, out _))
                {
                    return null;
                }

                // TOKEN_USER begins with SID_AND_ATTRIBUTES, whose first field is the SID.
                var sid = Marshal.ReadIntPtr(buffer);
                if (sid == IntPtr.Zero || !ConvertSidToStringSidW(sid, out var text))
                {
                    return null;
                }

                try
                {
                    return Marshal.PtrToStringUni(text);
                }
                finally
                {
                    LocalFree(text);
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
        finally
        {
            CloseHandle(token);
        }
    }

    internal static bool IsCurrentProcess(uint processId) => processId == GetCurrentProcessId();
}
