using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// A successful save of the <b>global</b> configuration, and what it does to the profile on disk.
/// </summary>
/// <remarks>
/// <para>
/// This is the case the earlier rounds could not run. An isolated Host started with
/// <c>FUSION_FCITX_SKIP_USER_PATH=1</c> has no user configuration directory, so the Host's global
/// apply ends in Fcitx's <c>safeSave()</c> and fails — which proved the client reports a failure
/// but proved nothing about a save that works. The Host here is started with a disposable,
/// <b>writable</b> profile instead, so the whole path runs: load, change, save, reread.
/// </para>
/// <para>
/// It needs <c>FUSION_SETTINGS_GLOBAL_PROFILE</c> — the profile root the Host was pointed at —
/// so it can also check that a save with nothing pending does not rewrite anything. That part is
/// a property of the profile, not of the client, and cannot be inferred from the reply.
/// </para>
/// </remarks>
internal static class GlobalSaveTests
{
    private const string BehaviorType = "Behavior$BehaviorConfig";
    private const string Option = "DefaultPageSize";

    public static async Task RunAsync(string endpoint, string profileRoot, CancellationToken cancellationToken)
    {
        Harness.Section("global: a supported value saves, is reread, and persists");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the workspace loads the global configuration");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var snapshot = workspace.SnapshotFor(SettingsTarget.Global)!;
        var option = snapshot.Types
            .First(type => string.Equals(type.Name, BehaviorType, StringComparison.Ordinal))
            .Options.First(candidate => string.Equals(candidate.Name, Option, StringComparison.Ordinal));
        var current = workspace.GlobalDraft!
            .EffectiveNested(snapshot, BehaviorType, Option).Scalar;

        var next = int.TryParse(current, out var parsed) ? parsed + 1 : 6;
        if (option.IntMaximum is { } maximum && next > maximum)
        {
            next = option.IntMinimum ?? 1;
        }

        Harness.Check(workspace.TrySetNested(SettingsTarget.Global, BehaviorType, Option,
                SettingsValue.Text(next.ToString(System.Globalization.CultureInfo.InvariantCulture))),
            "the global value is edited in the Global draft");
        Harness.Check(workspace.GlobalDraft.HasChanges, "and is pending");
        Harness.Check(!workspace.PinyinDraft!.HasChanges, "while the Pinyin draft is untouched");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the global save reports success, not the safeSave() failure the no-path Host gave");
        Harness.Check(!workspace.GlobalDraft.HasChanges, "and nothing is pending afterwards");

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.GlobalUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(reread.IsOk, "the global configuration can be read back");
        var stored = reread.Value!.Values
            .TryGetPath(new[] { "Behavior", Option }, out var value) ? value.Scalar : "(absent)";
        Harness.CheckEqual(next.ToString(System.Globalization.CultureInfo.InvariantCulture), stored,
            "the Host stored the new value");

        // A save with nothing pending sends no request, so nothing on disk may move.
        var before = Snapshot(profileRoot);
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(workspace.SaveSummary.Contains("没有需要保存的修改", StringComparison.Ordinal),
            "a save with nothing pending says so instead of sending a request");
        var after = Snapshot(profileRoot);
        var moved = Describe(before, after);
        Harness.Check(moved.Length == 0,
            "and no file under the profile was rewritten" + (moved.Length == 0 ? string.Empty : ": " + moved));

        Console.WriteLine($"   GLOBAL_EXPECT={Option}={next}");
    }

    /// <summary>
    /// After the isolated Host is restarted against the same profile: the value is still there.
    /// </summary>
    public static async Task VerifyPersistedAsync(string endpoint, string expectation,
                                                  CancellationToken cancellationToken)
    {
        Harness.Section("global: the saved value survives a Host restart");

        var separator = expectation.IndexOf('=');
        Harness.Check(separator > 0, "the expectation is name=value");
        if (separator <= 0)
        {
            return;
        }

        var name = expectation[..separator];
        var expected = expectation[(separator + 1)..];

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.GlobalUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(reread.IsOk, "the restarted Host answers");
        if (!reread.IsOk)
        {
            Console.WriteLine($"   Host said: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return;
        }

        var stored = reread.Value!.Values.TryGetPath(new[] { "Behavior", name }, out var value)
            ? value.Scalar
            : "(absent)";
        Harness.CheckEqual(expected, stored, "the value written before the restart is still stored");
    }

    private static Dictionary<string, string> Snapshot(string profileRoot)
    {
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!Directory.Exists(profileRoot))
        {
            return files;
        }

        foreach (var path in Directory.EnumerateFiles(profileRoot, "*", SearchOption.AllDirectories))
        {
            var info = new FileInfo(path);
            files[path] = $"{info.Length}:{info.LastWriteTimeUtc.Ticks}";
        }

        return files;
    }

    private static string Describe(Dictionary<string, string> before, Dictionary<string, string> after)
    {
        var changes = new List<string>();
        foreach (var (path, state) in after)
        {
            if (!before.TryGetValue(path, out var was))
            {
                changes.Add("created " + Path.GetFileName(path));
            }
            else if (!string.Equals(was, state, StringComparison.Ordinal))
            {
                changes.Add("rewritten " + Path.GetFileName(path));
            }
        }

        foreach (var path in before.Keys.Where(path => !after.ContainsKey(path)))
        {
            changes.Add("removed " + Path.GetFileName(path));
        }

        return string.Join(", ", changes);
    }
}
