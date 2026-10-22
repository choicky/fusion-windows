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
/// alone proves nothing about the renderer. They also cover what a <b>refused</b> write leaves
/// behind, because a save that reports success without persisting is the failure mode this
/// configuration has to rule out.
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

        Harness.CheckEqual("Enum", option.Type, "the layout is an enumeration");
        // Three arrangements now (R40 added automatic scrolling), and still no "not chosen"
        // value: an absent choice is the default, not a fourth stored state that would be
        // indistinguishable from a key the user never wrote.
        Harness.CheckEqual("Horizontal|Vertical|AutoScroll", string.Join("|", option.EnumLabels),
            "with the three arrangements and no additional stored state");
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

        // 6b. The third arrangement is a stored value like the other two: the schema offers
        //     it, the Host accepts it, and it comes back. This is the only one the settings
        //     window can offer that changes the *shape* of the window rather than its
        //     direction, so it is worth proving on the same path rather than by inspection.
        var layoutOption = type.Options.FirstOrDefault(candidate =>
            string.Equals(candidate.Name, Option, StringComparison.Ordinal));
        Harness.Check(layoutOption is not null, "the layout option is in the schema");
        if (layoutOption is not null)
        {
            Harness.CheckEqual(3, layoutOption.EnumLabels.Count,
                "the schema offers exactly three arrangements");
            Harness.Check(layoutOption.EnumLabels.Contains("AutoScroll", StringComparer.Ordinal),
                "and the automatic scrolling arrangement is one of them");
            Harness.CheckEqual("Horizontal|Vertical",
                string.Join("|", layoutOption.EnumLabels.Take(2)),
                "the two arrangements that shipped first kept their names and order");
            // The default is still vertical, so a profile that never chose keeps the window it
            // has always had rather than silently becoming a scrolling one.
            Harness.CheckEqual("Vertical", layoutOption.DefaultValue.Scalar,
                "and the default is still the vertical list");
        }

        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("AutoScroll")),
            "the automatic scrolling arrangement is selected");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "the save leaves nothing pending");
        Harness.CheckEqual("AutoScroll", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "the Host stores the automatic scrolling arrangement");
        // Back to the value step 7 needs, through the same path.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Vertical")),
            "and back to vertical for the next step");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual("Vertical", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "which the Host also stores");

        // 7. A write that the operating system genuinely refuses must not publish anything.
        //
        //    The failure is produced by the real write path, not by a stub: Fcitx saves by
        //    writing a temporary file and renaming it over the target, so holding the target
        //    open without delete sharing makes the rename fail with access denied. The Host
        //    then takes its error branch, and the temporary file it created is removed by
        //    Fcitx itself.
        //
        //    This is the regression for a real defect: the Host used to load the live
        //    configuration before writing it, so a failed write left it reporting a layout it
        //    had never persisted, the projection still using the old one, and a resubmission
        //    matching the "nothing changed" branch — a false success with no file write. Each
        //    assertion below fails against that ordering.
        var lockedFile = FindConfigFile(profileRoot);
        Harness.Check(lockedFile is not null, "the stored file is known before the failure is induced");
        if (lockedFile is null)
        {
            return;
        }

        var beforeFailure = File.ReadAllText(lockedFile);
        Harness.Check(beforeFailure.Contains("CandidateLayout=Vertical", StringComparison.Ordinal),
            "the file holds the known vertical value before the failure");

        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("Horizontal")),
            "a change to the other layout is made while the file cannot be replaced");
        using (new FileStream(lockedFile, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Console.WriteLine($"   refused save said: {workspace.SaveSummary.Replace("\n", " / ")}");
            Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "a save the filesystem refuses is reported as not saved");
            Harness.Check(workspace.PresentationDraft!.HasChanges,
                "and the edit is kept, so the user can retry it");
            Harness.CheckEqual("Vertical", await ReadStoredAsync(endpoint, cancellationToken)
                    .ConfigureAwait(false),
                "the Host still reports the old layout, not the value it failed to persist");
            Harness.CheckEqual(beforeFailure, File.ReadAllText(lockedFile),
                "and the stored file is byte-for-byte unchanged");
        }

        // 8. The same request, once the file can be replaced, must really be written. Against
        //    the old ordering this returned success while writing nothing, which the file
        //    assertion below would catch.
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   retried save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the identical request succeeds once writing is possible again");
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "and nothing is pending");
        Harness.CheckEqual("Horizontal", await ReadStoredAsync(endpoint, cancellationToken)
                .ConfigureAwait(false),
            "the Host reports the layout it now holds");
        Harness.Check(File.ReadAllText(lockedFile).Contains("CandidateLayout=Horizontal",
                StringComparison.Ordinal),
            "and the file really contains the retried value, so the success was not a no-op");

        // 9. The candidate font, which is the other half of this configuration: a string and
        //    an integer, so it exercises the schema and the save path in a way the enum does
        //    not, and the renderer reads both values.
        var familyOption = type.Options.FirstOrDefault(candidate =>
            string.Equals(candidate.Name, "FontFamily", StringComparison.Ordinal));
        var sizeOption = type.Options.FirstOrDefault(candidate =>
            string.Equals(candidate.Name, "FontSize", StringComparison.Ordinal));
        Harness.Check(familyOption is not null && sizeOption is not null,
            "the configuration also exposes the candidate font and its size");
        if (familyOption is null || sizeOption is null)
        {
            return;
        }

        Harness.CheckEqual("String", familyOption.Type, "the font family is a string option");
        Harness.CheckEqual("Microsoft YaHei UI", familyOption.DefaultValue.Scalar,
            "whose default is the font this product already drew with");
        Harness.CheckEqual("Integer", sizeOption.Type, "the size is an integer option");
        Harness.CheckEqual("11", sizeOption.DefaultValue.Scalar,
            "in points, matching the 15 px height the popup used at 96 dpi");
        Harness.Check(sizeOption.Properties.TryGet("IntMin", out var min) &&
                      sizeOption.Properties.TryGet("IntMax", out var max) &&
                      min.Scalar == "6" && max.Scalar == "48",
            "with the bounds the popup's own arithmetic assumes");
        Harness.CheckEqual("Microsoft YaHei UI",
            workspace.PresentationDraft!.Effective("FontFamily").Scalar,
            "a profile that never configured it reads the default family");
        Harness.CheckEqual("11", workspace.PresentationDraft!.Effective("FontSize").Scalar,
            "and the default size");

        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, "FontFamily",
                SettingsValue.Text("Segoe UI")),
            "the font family is edited in the same draft");
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, "FontSize",
                SettingsValue.Text("16")),
            "the font size is edited too");
        Harness.Check(workspace.PresentationDraft!.HasChanges, "both are pending");
        Harness.Check(!workspace.GlobalDraft!.HasChanges && !workspace.PinyinDraft!.HasChanges,
            "and no other configuration is touched by them");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   font save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the font save reports success");
        // A save replaces the draft, so the current one is read again rather than reused.
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "and nothing is pending");

        Harness.CheckEqual("16",
            await ReadPresentationValueAsync(endpoint, "FontSize", cancellationToken)
                .ConfigureAwait(false),
            "the Host reads the size back");
        Harness.CheckEqual("Segoe UI",
            await ReadPresentationValueAsync(endpoint, "FontFamily", cancellationToken)
                .ConfigureAwait(false),
            "and the family");

        var fontFile = FindConfigFile(profileRoot);
        Harness.Check(fontFile is not null, "the presentation file is on disk");
        if (fontFile is not null)
        {
            // The stored form is Fcitx's, not this test's: an option equal to its default is
            // written as a comment, and a value with a space is quoted. Both are read back
            // through the same parser the Host uses.
            var storedFamily = ReadIniValue(File.ReadAllText(fontFile), "FontFamily");
            var storedSize = ReadIniValue(File.ReadAllText(fontFile), "FontSize");
            Harness.CheckEqual("Segoe UI", storedFamily,
                "the family is really in the file, not only in the Host's memory");
            Harness.CheckEqual("16", storedSize, "and so is the size");
        }

        // 10. A size outside the schema's bounds never becomes a stored value. The draft is a
        //     value store and accepts it (validation belongs to the schema, not to the text
        //     box), so the refusal has to come from the Host — and it must leave everything
        //     as it was instead of writing a default in place of the rejected number.
        var beforeRejection = File.ReadAllText(fontFile!);
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, "FontSize",
                SettingsValue.Text("400")),
            "an out-of-range size can be typed into the draft");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine(
            $"   out-of-range save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "saving it is refused rather than silently clamped");
        Harness.CheckEqual("16",
            await ReadPresentationValueAsync(endpoint, "FontSize", cancellationToken)
                .ConfigureAwait(false),
            "the Host still holds the accepted size");
        Harness.CheckEqual(beforeRejection, File.ReadAllText(fontFile!),
            "and the file was not rewritten");
        workspace.Discard();
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "the refused edit is discarded");

        // 11. Restoring the defaults writes them, and the file follows: this is what the
        //     settings window's 恢复默认字体 does, through the same schema values.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, "FontFamily",
                familyOption.DefaultValue) &&
                workspace.TrySet(SettingsTarget.Presentation, "FontSize", sizeOption.DefaultValue),
            "the schema defaults are written back into the draft");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual("11",
            await ReadPresentationValueAsync(endpoint, "FontSize", cancellationToken)
                .ConfigureAwait(false),
            "the Host holds the default size again");
        Harness.CheckEqual("Microsoft YaHei UI",
            await ReadPresentationValueAsync(endpoint, "FontFamily", cancellationToken)
                .ConfigureAwait(false),
            "and the default family");
        // A default is stored as a comment, which is still the file's own record of the value.
        Harness.CheckEqual("11", ReadIniValue(File.ReadAllText(fontFile!), "FontSize"),
            "and the file agrees");

        // 12. Leave two non-default choices behind, so the post-restart run proves
        //     persistence of the choices rather than of defaults that would read the same
        //     either way: the automatic scrolling arrangement and a 16 pt font.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, "FontSize",
                SettingsValue.Text("16")),
            "a non-default size is set again for the restart check");
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, Option,
                SettingsValue.Text("AutoScroll")),
            "and the automatic scrolling arrangement");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual("16",
            await ReadPresentationValueAsync(endpoint, "FontSize", cancellationToken)
                .ConfigureAwait(false),
            "the size is stored");
        Harness.CheckEqual("AutoScroll",
            await ReadPresentationValueAsync(endpoint, Option, cancellationToken)
                .ConfigureAwait(false),
            "and so is the arrangement");

        Console.WriteLine($"   PRESENTATION_EXPECT={Option}=AutoScroll;FontSize=16");
    }

    /// <summary>
    /// The stored value of <paramref name="key"/>. A live value is written as `Key=Value`, a
    /// value equal to its default as `# Key=Value`, and a value containing a space is quoted;
    /// all three are the same fact for a reader, and only the uncommented form is a choice.
    /// </summary>
    private static string ReadIniValue(string contents, string key)
    {
        foreach (var line in contents.Split('\n'))
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith('#'))
            {
                trimmed = trimmed.TrimStart('#').Trim();
            }

            var separator = trimmed.IndexOf('=');
            if (separator <= 0 || !string.Equals(trimmed[..separator].Trim(), key, StringComparison.Ordinal))
            {
                continue;
            }

            return trimmed[(separator + 1)..].Trim().Trim('"');
        }

        return "(absent)";
    }

    private static async Task<string> ReadPresentationValueAsync(string endpoint, string name,
                                                                CancellationToken cancellationToken)
    {
        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.PresentationUri, cancellationToken)
            .ConfigureAwait(false);
        if (!reread.IsOk)
        {
            Console.WriteLine($"   reread failed: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return "(reread failed)";
        }

        return reread.Value!.Values.TryGet(name, out var value) ? value.Scalar : "(absent)";
    }

    /// <summary>
    /// After the isolated Host is restarted against the same profile: the value is still there.
    /// </summary>
    public static async Task VerifyPersistedAsync(string endpoint, string expectation,
                                                  CancellationToken cancellationToken)
    {
        Harness.Section("candidate presentation: the saved values survive a Host restart");

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.PresentationUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(reread.IsOk, "the restarted Host answers");
        if (!reread.IsOk)
        {
            Console.WriteLine($"   Host said: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return;
        }

        // One or more name=value pairs, separated by ';': the layout and the font are separate
        // facts and the run leaves both non-default for exactly this check.
        foreach (var pair in expectation.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var separator = pair.IndexOf('=');
            Harness.Check(separator > 0, "the expectation is name=value");
            if (separator <= 0)
            {
                continue;
            }

            var name = pair[..separator];
            var expected = pair[(separator + 1)..];
            var stored = reread.Value!.Values.TryGet(name, out var value) ? value.Scalar : "(absent)";
            Harness.CheckEqual(expected, stored,
                "the " + name + " written before the restart is still stored");
        }
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
