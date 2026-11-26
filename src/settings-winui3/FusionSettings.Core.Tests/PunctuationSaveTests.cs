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
/// The zh_CN punctuation map against a real isolated Host and its real Fcitx data file. The
/// checks use the same workspace and save bar as the window, and never accept an in-memory
/// readback as proof: every successful edit is checked in the canonical file as well.
/// </summary>
internal static class PunctuationSaveTests
{
    private const string RelativeFile = "User/AppData/Roaming/Fcitx5/punctuation/punc.mb.zh_CN";

    public static async Task RunAsync(string endpoint, string profileRoot,
                                      CancellationToken cancellationToken)
    {
        Harness.Section("punctuation: official subconfig, file-backed save, invalid and refused writes");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the workspace loads the official zh_CN punctuation subconfiguration");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var snapshot = workspace.SnapshotFor(SettingsTarget.Punctuation);
        Harness.Check(snapshot is not null && snapshot.Types.Count > 0,
            "the Host describes the punctuation addon's own configuration");
        if (snapshot is null || snapshot.Types.Count == 0)
        {
            return;
        }
        Harness.CheckEqual("PunctuationMapConfig", snapshot.Types[0].Name,
            "using the addon's schema rather than a Fusion copy");
        var option = snapshot.Types[0].Options.FirstOrDefault(candidate =>
            string.Equals(candidate.Name, PunctuationMap.Entries, StringComparison.Ordinal));
        Harness.Check(option is not null && option.Type == "List|Entries$PunctuationMapEntryConfig",
            "Entries is the addon's ordered row list");
        if (option is null)
        {
            return;
        }

        var defaults = PunctuationMap.Defaults(snapshot);
        Harness.Check(defaults.Count > 0, "the schema carries the shipped table as its default");
        Harness.Check(Count(defaults, "#", "#") == 1 && Count(defaults, "#", "＃") == 1,
            "with two candidates for #");
        Harness.Check(defaults.Any(row => row.Key == "\"" && row.Mapping == "“" && row.AltMapping == "”"),
            "and a paired row with both halves on one row");

        var draft = workspace.PunctuationDraft;
        Harness.Check(draft is not null, "the punctuation map has its own draft");
        if (draft is null)
        {
            return;
        }
        var initial = PunctuationMap.RowsFromOption(draft.Effective(PunctuationMap.Entries));
        Harness.Check(initial.SequenceEqual(defaults),
            "a fresh profile reads the shipped map");

        var file = Path.Combine(profileRoot, RelativeFile.Replace('/', Path.DirectorySeparatorChar));
        var edited = initial.Where(row => !(row.Key == "#" && row.Mapping == "＃")).ToList();
        Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(edited)),
            "the full-width # row is deleted in the draft");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   edit save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the edit is reported as saved");
        var stored = await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false);
        Harness.Check(Count(stored, "#", "＃") == 0 && Count(stored, "#", "#") == 1,
            "the Host now has a single ASCII candidate for #");
        var fileText = File.ReadAllText(file);
        Harness.Check(fileText.Contains("# #\n", StringComparison.Ordinal) &&
                      !fileText.Contains("# ＃\n", StringComparison.Ordinal),
            "and Fcitx's canonical data file really holds the edit");

        // A row Fcitx would silently drop is refused by the Host with an explanation. The
        // failed save leaves the edit in the draft, the Host and the file at the previous map.
        var invalid = edited.Concat(new[] { new PunctuationRow("ab", "×", string.Empty) }).ToList();
        Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(invalid)),
            "an invalid row is placed in the draft");
        var beforeInvalid = File.ReadAllText(file);
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   invalid save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the invalid save is reported as not saved");
        Harness.Check(workspace.PunctuationDraft!.HasChanges, "and keeps the edit for correction");
        Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false))
                .Count == edited.Count,
            "the Host still holds the previous map");
        Harness.CheckEqual(beforeInvalid, File.ReadAllText(file),
            "and the file is byte-for-byte unchanged");
        workspace.Discard();

        // A filesystem refusal must not publish an in-memory value either. Holding the Fcitx
        // file open without delete sharing makes its atomic rename fail on Windows.
        var extra = edited.Concat(new[] { new PunctuationRow("=", "＝", string.Empty) }).ToList();
        Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(extra)),
            "a valid extra row is made while the file cannot be replaced");
        using (new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Console.WriteLine($"   refused save said: {workspace.SaveSummary.Replace("\n", " / ")}");
            Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "a write the filesystem refuses is reported as not saved");
            Harness.Check(workspace.PunctuationDraft!.HasChanges, "and the edit is kept");
            Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false))
                    .Count == edited.Count,
                "the Host still reports the old map, not the value it failed to persist");
            Harness.CheckEqual(beforeInvalid, File.ReadAllText(file),
                "and the canonical file is unchanged");
        }
        workspace.Discard();

        // Restore defaults is a normal save of the schema's DefaultValue, not a second table.
        Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(defaults)),
            "the shipped defaults are put into the draft");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "restore defaults is saved");
        Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false))
                .SequenceEqual(defaults),
            "the Host has the shipped rows again");
        Harness.Check(File.ReadAllText(file).Contains("# ＃\n", StringComparison.Ordinal),
            "and both # candidates are back in the file");

        // Leave the # edit in place for the post-restart run: it is the state whose persistence
        // has to be proved by a new Host process, not by this one's readback.
        Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(edited)),
            "the # edit is made again for the restart check");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "and saved");
        Console.WriteLine("PUNCTUATION_EXPECT=hash=one");
    }

    public static async Task VerifyPersistedAsync(string endpoint, string expectation,
                                                  CancellationToken cancellationToken)
    {
        Harness.Section("punctuation: a new Host process reads the saved map");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the restarted Host serves the punctuation configuration");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var snapshot = workspace.SnapshotFor(SettingsTarget.Punctuation)!;
        var rows = PunctuationMap.RowsFromOption(workspace.PunctuationDraft!.Effective(PunctuationMap.Entries));
        if (string.Equals(expectation, "hash=one", StringComparison.Ordinal))
        {
            Harness.Check(Count(rows, "#", "#") == 1 && Count(rows, "#", "＃") == 0,
                "the removal of the full-width # row survived the restart");

            var defaults = PunctuationMap.Defaults(snapshot);
            Harness.Check(workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                    PunctuationMap.Value(defaults)),
                "defaults are restored from the schema after the restart");
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "the restore is saved");
            var restored = PunctuationMap.Rows(
                workspace.PunctuationSnapshot!.Values);
            Harness.Check(restored.SequenceEqual(defaults) && Count(restored, "#", "＃") == 1,
                "both # candidates are back from the shipped default table");
        }
        else
        {
            Harness.Check(false, "an unknown punctuation restart expectation");
        }
    }

    private static int Count(IEnumerable<PunctuationRow> rows, string key, string mapping) =>
        rows.Count(row => row.Key == key && row.Mapping == mapping);

    private static async Task<IReadOnlyList<PunctuationRow>> ReadRowsAsync(
        string endpoint, CancellationToken cancellationToken)
    {
        var client = new SettingsClient(endpoint, TimeSpan.FromSeconds(6));
        var result = await client.GetConfigAsync(SettingsWorkspace.PunctuationUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(result.IsOk, "a fresh client can reread the punctuation map");
        return result.IsOk
            ? PunctuationMap.Rows(result.Value!.Values)
            : Array.Empty<PunctuationRow>();
    }
}
