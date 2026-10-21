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
/// The Windows candidate-presentation configuration against a real, isolated Host with a
/// disposable writable profile: read the stored value, save both layouts, reread, and prove
/// the file lands in Fcitx's own per-user configuration directory.
/// </summary>
/// <remarks>
/// <para>
/// This is the configuration the owner authorized on 2026-10-22 — a Windows presentation
/// setting with its own configuration, holding no engine or global value. The checks below
/// are about the <b>stored</b> value and the file, not about pixels: whether the candidate
/// window actually changes geometry is a separate, product-path check, because a saved enum
/// alone proves nothing about the renderer.
/// </para>
/// <para>
/// It needs <c>FUSION_SETTINGS_GLOBAL_PROFILE</c> (the same profile root the global save
/// uses) so it can locate the file and prove a no-change save rewrites nothing.
/// </para>
/// </remarks>
internal static class PresentationSaveTests
{
    private const string Option = "CandidateLayout";
    private const string ConfigTypeName = "CandidatePresentationConfig";
    private const string FileName = "candidate-presentation.conf";

    public static async Task RunAsync(string endpoint, string profileRoot,
                                      CancellationToken cancellationToken)
    {
        Harness.Section("candidate layout: read, save both directions, reread");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the workspace loads the presentation configuration");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var snapshot = workspace.SnapshotFor(SettingsTarget.Presentation);
        Harness.Check(snapshot is not null && snapshot.Types.Count > 0,
            "the Host describes the presentation configuration");
        if (snapshot is null || snapshot.Types.Count == 0)
        {
            return;
        }

        var type = snapshot.Types[0];
        Harness.CheckEqual(ConfigTypeName, type.Name,
            "the fourth schema is the Windows presentation configuration, not an official one");
        var option = type.Options.FirstOrDefault(candidate =>
            string.Equals(candidate.Name, Option, StringComparison.Ordinal));
        Harness.Check(option is not null, "it exposes the candidate layout option");
        if (option is null)
        {
            return;
        }

        Harness.CheckEqual("Enum", option.Type, "the layout is a two-valued option");
        Harness.CheckEqual("Horizontal|Vertical", string.Join("|", option.EnumLabels),
            "with exactly the two layouts and no third stored state");
        Harness.CheckEqual("Vertical", option.DefaultValue.Scalar,
            "and a default that preserves the presentation shipped before the option existed");

        var draft = workspace.PresentationDraft;
        Harness.Check(draft is not null, "a draft exists for it");
        if (draft is null)
        {
            return;
        }

        // 1. A profile that never configured it: the schema default, and no file yet.
        Harness.CheckEqual("Vertical", draft.Effective(Option).Scalar,
            "a profile that never configured it reads as the default, vertical");
        Harness.Check(!draft.HasChanges, "and nothing is pending");
        Harness.Check(FindConfigFile(profileRoot) is null,
            "no presentation file exists before the first save");

        // 2. Horizontal: edited in its own draft, saved, read back from the Host.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Horizontal")),
            "the layout is edited in the presentation draft");
        Harness.Check(draft.HasChanges, "and is pending");
        Harness.Check(!workspace.GlobalDraft!.HasChanges && !workspace.PinyinDraft!.HasChanges,
            "while no other draft is touched");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the save reports success");
        Harness.Check(!draft.HasChanges, "and nothing is pending afterwards");
        Harness.CheckEqual("Horizontal", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "the Host stores the horizontal layout");

        var file = FindConfigFile(profileRoot);
        Harness.Check(file is not null, "and a presentation file now exists");
        if (file is not null)
        {
            // The storage location is Fcitx's own per-user configuration directory; a file
            // anywhere else would be the additional store the boundary forbids. Measured on
            // this runtime: <user config root>/fcitx5/conf/candidate-presentation.conf, i.e.
            // the same "conf" directory the other Fcitx configurations are written to. The
            // assertion checks the two facts that matter (a Fcitx config root and a conf/
            // path) rather than the whole literal path, which carries a platform level.
            var relative = Path.GetRelativePath(profileRoot, file).Replace('\\', '/');
            Harness.Check(
                relative.EndsWith("/conf/" + FileName, StringComparison.Ordinal) &&
                relative.Contains("Fcitx5/", StringComparison.Ordinal),
                "it is written to Fcitx's per-user configuration directory: " + relative);
        }

        // 3. A save with nothing pending sends no request, so nothing on disk may move.
        var before = Snapshot(profileRoot);
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(workspace.SaveSummary.Contains("没有需要保存的修改", StringComparison.Ordinal),
            "a save with nothing pending says so instead of sending a request");
        Harness.CheckEqual(string.Empty, Describe(before, Snapshot(profileRoot)),
            "and no file under the profile was rewritten");

        // 4. Discard puts the baseline back without touching the Host.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Vertical")),
            "a pending edit is made");
        workspace.Discard();
        Harness.Check(!draft.HasChanges, "discard clears it");
        Harness.CheckEqual("Horizontal", draft.Effective(Option).Scalar,
            "and the draft shows the stored value again");
        Harness.CheckEqual("Horizontal", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "while the Host still holds what was saved");

        // 5. An explicit reload re-reads the Host and drops pending edits.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Vertical")),
            "another pending edit is made");
        Harness.Check(await workspace.ReloadAsync(preserveEdits: false, cancellationToken)
                .ConfigureAwait(false),
            "an explicit reload succeeds");
        var reloaded = workspace.PresentationDraft!;
        Harness.Check(!reloaded.HasChanges, "the reload left nothing pending");
        Harness.CheckEqual("Horizontal", reloaded.Effective(Option).Scalar,
            "and the draft shows the Host's value, not the discarded edit");

        // 6. The other direction, saved and read back.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Vertical")),
            "the layout is changed to vertical");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "the vertical save leaves nothing pending");
        Harness.CheckEqual("Vertical", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "the Host stores the vertical layout");

        // 7. Leave a non-default value behind, so the post-restart run proves persistence of a
        //    choice rather than of a default that would read the same either way.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Horizontal")),
            "the layout is set back to horizontal for the restart check");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual("Horizontal", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "the horizontal layout is stored again");

        Console.WriteLine($"   PRESENTATION_EXPECT={Option}=Horizontal");
    }

    /// <summary>
    /// After the isolated Host is restarted against the same profile: the value is still there.
    /// </summary>
    public static async Task VerifyPersistedAsync(string endpoint, string expectation,
                                                  CancellationToken cancellationToken)
    {
        Harness.Section("candidate layout: the saved value survives a Host restart");

        var separator = expectation.IndexOf('=');
        Harness.Check(separator > 0, "the expectation is name=value");
        if (separator <= 0)
        {
            return;
        }

        var name = expectation[..separator];
        var expected = expectation[(separator + 1)..];

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.PresentationUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(reread.IsOk, "the restarted Host answers");
        if (!reread.IsOk)
        {
            Console.WriteLine($"   Host said: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return;
        }

        var stored = reread.Value!.Values.TryGet(name, out var value) ? value.Scalar : "(absent)";
        Harness.CheckEqual(expected, stored, "the value written before the restart is still stored");
    }

    private static async Task<string> ReadStoredAsync(string endpoint, CancellationToken cancellationToken)
    {
        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.PresentationUri, cancellationToken)
            .ConfigureAwait(false);
        if (!reread.IsOk)
        {
            Console.WriteLine($"   reread failed: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return "(reread failed)";
        }

        return reread.Value!.Values.TryGet(Option, out var value) ? value.Scalar : "(absent)";
    }

    private static string? FindConfigFile(string profileRoot) =>
        Directory.Exists(profileRoot)
            ? Directory.EnumerateFiles(profileRoot, FileName, SearchOption.AllDirectories)
                .FirstOrDefault()
            : null;

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
