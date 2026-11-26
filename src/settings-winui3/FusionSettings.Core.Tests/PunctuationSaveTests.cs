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
        Harness.Section("punctuation: official subconfig, file-backed save, pending rows and empty table");

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
        var initial = workspace.PunctuationRows.ToList();
        Harness.Check(initial.SequenceEqual(defaults),
            "a fresh profile reads the shipped map");

        var file = Path.Combine(profileRoot, RelativeFile.Replace('/', Path.DirectorySeparatorChar));
        var edited = initial.Where(row => !(row.Key == "#" && row.Mapping == "＃")).ToList();
        Harness.Check(workspace.SetPunctuationRows(edited),
            "the full-width # row is deleted in the one editable table");
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

        // A valid edit followed by an incomplete row: the visible table is authoritative. A
        // save must refuse the punctuation page rather than submit the older valid draft.
        var incomplete = edited.Concat(new[] { new PunctuationRow("", "", string.Empty) }).ToList();
        Harness.Check(workspace.SetPunctuationRows(incomplete),
            "an incomplete row is retained in the editable table");
        Harness.Check(workspace.PunctuationRows.Count == incomplete.Count &&
                      workspace.PunctuationRows[^1].Key.Length == 0,
            "the incomplete row is still what the page would rebuild");
        Harness.Check(workspace.PunctuationInvalidReason is not null,
            "the table is invalid while that row is incomplete");
        Harness.Check(workspace.HasChanges,
            "the incomplete row still counts as an unsaved change");
        Harness.Check(!workspace.PunctuationDraft!.HasChanges,
            "the incomplete row is not put into the draft as a second table");
        var beforeIncomplete = File.ReadAllText(file);
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   incomplete save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the incomplete table is reported as not saved");
        Harness.Check(workspace.PunctuationRows.Count == incomplete.Count,
            "the user's incomplete input is retained");
        Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false))
                .Count == edited.Count,
            "the Host still holds the previous map");
        Harness.CheckEqual(beforeIncomplete, File.ReadAllText(file),
            "and the file is byte-for-byte unchanged");

        // Correct the row and save again. This is the same draft, not a replay of the older
        // table that happened to be valid before the incomplete row was typed.
        Harness.Check(workspace.SetPunctuationRows(edited),
            "the correction returns the table to a valid state");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the corrected table is saved");

        // Discard must clear the punctuation table too, not only the shared settings drafts.
        var extra = edited.Concat(new[] { new PunctuationRow("=", "＝", string.Empty) }).ToList();
        Harness.Check(workspace.SetPunctuationRows(extra),
            "a valid extra row is pending before discard");
        Harness.Check(workspace.HasChanges, "and the save bar sees it");
        workspace.Discard();
        Harness.Check(workspace.PunctuationRows.SequenceEqual(edited),
            "discard restores the last saved punctuation table");
        Harness.Check(!workspace.HasChanges, "and clears the punctuation dirty state");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(workspace.SaveSummary.Contains("没有需要保存", StringComparison.Ordinal),
            "discard followed by save sends nothing");

        // A filesystem refusal must not publish an in-memory value either. Holding the Fcitx
        // file open without delete sharing makes its atomic rename fail on Windows.
        Harness.Check(workspace.SetPunctuationRows(extra),
            "a valid extra row is made while the file cannot be replaced");
        using (new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Console.WriteLine($"   refused save said: {workspace.SaveSummary.Replace("\n", " / ")}");
            Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "a write the filesystem refuses is reported as not saved");
            Harness.Check(workspace.PunctuationRows.Count == extra.Count,
                "and the editor keeps the user's row");
            Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false))
                    .Count == edited.Count,
                "the Host still reports the old map, not the value it failed to persist");
            Harness.CheckEqual(beforeIncomplete, File.ReadAllText(file),
                "and the canonical file is unchanged");
        }

        // Retrying after the lock is released is a real save, not a no-change shortcut.
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the same row saves after the file is writable");
        Harness.Check(File.ReadAllText(file).Contains("= ＝\n", StringComparison.Ordinal),
            "and it reaches the canonical file");
        Harness.Check(workspace.SetPunctuationRows(edited),
            "the extra row is removed again for the empty-table check");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);

        // A deliberately empty table is a valid value, distinct from a missing Entries option.
        Harness.Check(workspace.SetPunctuationRows(Array.Empty<PunctuationRow>()),
            "an empty punctuation table is accepted");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   empty save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the empty table is reported as saved");
        Harness.CheckEqual(0L, new FileInfo(file).Length,
            "and the canonical file is empty");
        Harness.Check((await ReadRowsAsync(endpoint, cancellationToken).ConfigureAwait(false)).Count == 0,
            "the Host reads the empty table as empty, not as missing");
        Console.WriteLine("PUNCTUATION_EXPECT=empty");
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
        var rows = workspace.PunctuationRows;
        if (string.Equals(expectation, "empty", StringComparison.Ordinal))
        {
            Harness.Check(rows.Count == 0,
                "the deliberately empty table survived the restart");

            var defaults = PunctuationMap.Defaults(snapshot);
            Harness.Check(workspace.SetPunctuationRows(defaults),
                "defaults are restored from the schema after the restart");
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "the restore is saved");
            var restored = workspace.PunctuationRows;
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
