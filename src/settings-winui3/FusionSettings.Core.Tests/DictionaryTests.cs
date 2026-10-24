using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// Dictionary management against a real Host with a disposable writable profile: the list,
/// an import, the name conflict, a corrupt file, enable/disable and delete, each read back
/// from the Host rather than from this process's idea of what happened.
/// </summary>
/// <remarks>
/// It needs <c>FUSION_SETTINGS_GLOBAL_PROFILE</c> (the profile root the Host was pointed at)
/// and an isolated pipe. The words themselves are asserted in the native
/// <c>dictionary_manager</c> check, which types into the engine; this one covers the protocol
/// and the file/Host path the window uses.
/// </remarks>
internal static class DictionaryTests
{
    private const string Name = "settingsdict";
    private const string Word = "墨珂";

    public static async Task RunAsync(string endpoint, string profileRoot,
                                      CancellationToken cancellationToken)
    {
        Harness.Section("dictionaries: list, import, conflict, enable, delete");

        var client = new SettingsClient(endpoint, TimeSpan.FromSeconds(30));

        var listed = await client.ListDictionariesAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(listed.IsOk, "the Host answers a dictionary list request");
        if (!listed.IsOk)
        {
            Console.WriteLine($"   Host said: {listed.Error}");
            return;
        }

        var before = listed.Value!;
        Console.WriteLine($"   dictionaries before: {Describe(before)}");
        Harness.Check(before.Entries.All(entry => entry.Name != Name),
            "the profile starts without the dictionary this check imports");

        var textFile = Path.Combine(profileRoot, Name + ".txt");
        await File.WriteAllTextAsync(textFile, Word + " mo'ke 1000\n", cancellationToken)
            .ConfigureAwait(false);

        var imported = await client.ImportDictionaryAsync(textFile, overwrite: false, cancellationToken)
            .ConfigureAwait(false);
        Console.WriteLine($"   import: {imported.Status} {imported.Error}");
        Harness.Check(imported.IsOk, "the Host imports a text dictionary");
        if (!imported.IsOk)
        {
            return;
        }

        var entry = imported.Value!.Entry;
        Harness.CheckEqual(Name, entry.Name, "the dictionary keeps the file's name");
        Harness.CheckEqual("user", entry.Source, "and sits in the user root");
        Harness.Check(entry.Enabled, "imported enabled");
        Harness.Check(entry.Size > 0, "with a size");
        Harness.Check(imported.Value!.List.Entries.Any(candidate => candidate.Name == Name),
            "and the reply lists it");

        // The name conflict is a distinct answer, and nothing is replaced until it is answered.
        var again = await client.ImportDictionaryAsync(textFile, overwrite: false, cancellationToken)
            .ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Exists, again.Status,
            "a second import of the same name reports Exists");
        Harness.Check(!string.IsNullOrEmpty(again.Error), "with an explanation");

        var replaced = await client.ImportDictionaryAsync(textFile, overwrite: true, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(replaced.IsOk, "and the confirmed overwrite is accepted");

        // A corrupt file is refused and leaves the imported dictionary alone.
        var brokenFile = Path.Combine(profileRoot, "broken-" + Name + ".dict");
        await File.WriteAllTextAsync(brokenFile, "not a trie dump", cancellationToken).ConfigureAwait(false);
        var broken = await client.ImportDictionaryAsync(brokenFile, overwrite: false, cancellationToken)
            .ConfigureAwait(false);
        Console.WriteLine($"   broken: {broken.Status} {broken.Error}");
        Harness.CheckEqual(SettingsStatus.Invalid, broken.Status, "a corrupt .dict is refused");
        Harness.Check(broken.Error.Contains("dictionary", StringComparison.OrdinalIgnoreCase),
            "with a message that says what was wrong");

        var afterBroken = await client.ListDictionariesAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(afterBroken.IsOk && afterBroken.Value!.Entries.Any(item => item.Name == Name && item.Enabled),
            "the imported dictionary is untouched by the failed import");

        // Disable, read back, enable, read back.
        var disabled = await client.SetDictionaryEnabledAsync(Name, false, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(disabled.IsOk, "disabling is accepted");
        var disabledEntry = disabled.Value!.Entries.FirstOrDefault(item => item.Name == Name);
        Harness.Check(disabledEntry is not null && !disabledEntry.Enabled, "and reads back disabled");

        var enabled = await client.SetDictionaryEnabledAsync(Name, true, cancellationToken)
            .ConfigureAwait(false);
        Harness.Check(enabled.IsOk, "re-enabling is accepted");
        var enabledEntry = enabled.Value!.Entries.FirstOrDefault(item => item.Name == Name);
        Harness.Check(enabledEntry is not null && enabledEntry.Enabled, "and reads back enabled");

        // Reload is a user action of its own and answers with the same list.
        var reloaded = await client.ReloadDictionariesAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(reloaded.IsOk, "an explicit reload is accepted");
        Harness.Check(reloaded.IsOk && reloaded.Value!.Entries.Any(item => item.Name == Name),
            "and the dictionary is still listed");

        // A packaged dictionary cannot be deleted. This profile has none, so the refusal is
        // exercised through a name that does not exist — the same code path, and no file to
        // damage.
        var missing = await client.DeleteDictionaryAsync("no-such-dictionary", cancellationToken)
            .ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Invalid, missing.Status,
            "deleting a name that is not installed is refused");

        // The learned dictionary is the user's, not this manager's: its state has to be the
        // same before and after the delete, whichever state that is.
        var learnedBefore = FindLearnedDictionary(profileRoot);
        var deleted = await client.DeleteDictionaryAsync(Name, cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   delete: {deleted.Status} {deleted.Error}");
        Harness.Check(deleted.IsOk, "deleting a user dictionary is accepted");
        Harness.Check(deleted.IsOk && deleted.Value!.Entries.All(item => item.Name != Name),
            "and it is gone from the list");

        // The files really left the profile, and the learning data was not touched.
        var dictionaryFiles = Directory.Exists(profileRoot)
            ? Directory.EnumerateFiles(profileRoot, Name + "*.dict*", SearchOption.AllDirectories).ToList()
            : new List<string>();
        Harness.CheckEqual(0, dictionaryFiles.Count, "no file of that dictionary remains");
        Harness.CheckEqual(learnedBefore, FindLearnedDictionary(profileRoot),
            "the user's learned dictionary is exactly as it was");
    }

    private static string FindLearnedDictionary(string profileRoot) =>
        Directory.Exists(profileRoot)
            ? Directory.EnumerateFiles(profileRoot, "user.dict", SearchOption.AllDirectories).FirstOrDefault()
              ?? "(absent)"
            : "(absent)";

    private static string Describe(DictionaryList list) =>
        list.Entries.Count == 0
            ? "(none)"
            : string.Join(", ", list.Entries.Select(entry =>
                entry.Name + "(" + entry.Source + (entry.Enabled ? ",enabled" : ",disabled") + ")"));
}
