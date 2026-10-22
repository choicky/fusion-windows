using System;
using System.IO;
using System.Text.RegularExpressions;
using Fusion.Settings.Ipc;

namespace Fusion.Settings.Tests;

/// <summary>
/// The C++ and C# ends of the wire protocol must agree on the version number.
/// </summary>
/// <remarks>
/// <para>
/// The two ends carry it as separate literals — <c>fusion::ipc::protocol_version</c> in
/// <c>include/fusion/ipc_protocol.hpp</c> and <see cref="WireLimits.ProtocolVersion"/> here —
/// and both check it exactly, so a mismatch is not a warning: the handshake fails and the
/// client is served nothing.
/// </para>
/// <para>
/// That is what happened when the protocol went 4 → 5 for the candidate font: the C++ side
/// moved, the C# constant did not, and every check still passed because none of them compared
/// the two. This check exists so the next shape change cannot pass without both sides moving.
/// It reads the header from the repository rather than trusting a copy, and it fails — not
/// skips — when the header cannot be found, because a guard that quietly does nothing is the
/// failure it is meant to prevent.
/// </para>
/// </remarks>
internal static class ProtocolVersionTests
{
    public static void Run()
    {
        Harness.Section("protocol: the C++ and C# versions are the same literal");

        var header = FindHeader();
        Harness.Check(header is not null,
            "include/fusion/ipc_protocol.hpp is reachable from the test binary");
        if (header is null)
        {
            Console.WriteLine("   searched upwards from " + AppContext.BaseDirectory);
            return;
        }

        var text = File.ReadAllText(header);
        var match = Regex.Match(text, @"protocol_version\s*=\s*(\d+)\s*;");
        Harness.Check(match.Success, "the header declares protocol_version as a literal");
        if (!match.Success)
        {
            return;
        }

        var cpp = uint.Parse(match.Groups[1].Value);
        Console.WriteLine($"   C++ {cpp} ({header}) vs C# {WireLimits.ProtocolVersion}");
        Harness.CheckEqual(WireLimits.ProtocolVersion, cpp,
            "WireLimits.ProtocolVersion matches fusion::ipc::protocol_version");
    }

    /// <summary>
    /// Walks up from the test binary to the repository root. The project sits at
    /// <c>src/settings-winui3/FusionSettings.Core.Tests</c>, so the root is a few levels above
    /// the build output; nothing is assumed about how deep.
    /// </summary>
    private static string? FindHeader()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for (var level = 0; level < 12 && directory is not null; level++)
        {
            var candidate = Path.Combine(directory.FullName, "include", "fusion", "ipc_protocol.hpp");
            if (File.Exists(candidate))
            {
                return candidate;
            }

            directory = directory.Parent;
        }

        return null;
    }
}
