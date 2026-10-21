using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// Checks against a real Host over a real named pipe.
/// </summary>
/// <remarks>
/// The endpoint is supplied by the caller, exactly as the existing Stage 4A integration
/// mode requires: a test process cannot isolate a Host it did not start, and connecting
/// to the installed product Host would write to the user's live configuration. Without
/// an explicit endpoint these checks skip rather than fall back.
/// </remarks>
internal static class IntegrationTests
{
    public static async Task RunAsync()
    {
        var configured = Environment.GetEnvironmentVariable("FUSION_SETTINGS_TEST_PIPE");
        if (string.IsNullOrWhiteSpace(configured))
        {
            Harness.Section("integration: SKIPPED (FUSION_SETTINGS_TEST_PIPE is not set)");
            Console.WriteLine("   the production endpoint is never used");
            return;
        }

        var endpoint = PipeEndpoint.Normalize(configured);
        if (endpoint is null)
        {
            throw new InvalidOperationException("FUSION_SETTINGS_TEST_PIPE is not a single pipe name");
        }

        if (PipeEndpoint.IsProduction(endpoint))
        {
            throw new InvalidOperationException(
                "FUSION_SETTINGS_TEST_PIPE names the production endpoint; refusing to touch it");
        }

        Harness.Section($"integration: real Host on {endpoint}");
        var client = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        var pinyinUri = "fcitx://config/inputmethod/pinyin";
        var first = await client.GetConfigAsync(pinyinUri, cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, first.Status, "GetConfig succeeds");
        if (!first.IsOk)
        {
            Console.WriteLine($"   Host said: {first.Error}");
            return;
        }

        var snapshot = first.Value!;
        Harness.Check(snapshot.Types.Count >= 2, "the reply carries the schema types");
        Harness.CheckEqual("PinyinEngineConfig", snapshot.Types[0].Name, "the root type is the engine config");
        Harness.Check(snapshot.FindOption("PrevPage") is not null, "PrevPage is described");

        var prevPage = KeyBindingList.FromValue(snapshot.Values.Children["PrevPage"]);
        Harness.CheckEqual(4, prevPage.Count, "PrevPage arrives with four bindings");
        Harness.CheckEqual("minus", prevPage[0].ToString(), "and they are in official order");

        Harness.Section("integration: incarnation fencing and a stable Host");
        var second = await client.GetConfigAsync(pinyinUri, cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(first.HostIncarnation, second.HostIncarnation,
            "two connections report the same Host incarnation");

        Harness.Section("integration: no-change save sends nothing");
        var draft = new SettingsDraft(ConfigIdentity.ForUri(pinyinUri), snapshot);
        Harness.Check(!draft.HasChanges, "a fresh draft over the live state has no changes");
        Harness.CheckEqual(0, draft.BuildChangeSet().Children.Count,
            "so there is no request to send (and no setter runs)");

        Harness.Section("integration: read, apply, read back");
        var pageSizeOption = snapshot.FindRootOption("PageSize");
        Harness.Check(pageSizeOption is not null, "PageSize is in the root schema");
        var original = snapshot.Values.Children["PageSize"].Scalar;
        var minimum = pageSizeOption!.IntMinimum ?? 1;
        var maximum = pageSizeOption.IntMaximum ?? 10;
        var target = int.TryParse(original, out var parsed) && parsed + 1 <= maximum
            ? (parsed + 1).ToString(System.Globalization.CultureInfo.InvariantCulture)
            : minimum.ToString(System.Globalization.CultureInfo.InvariantCulture);

        Harness.Check(draft.TrySet("PageSize", SettingsValue.Text(target)), "the edit is accepted");
        var apply = await client.SetConfigAsync(pinyinUri, draft.BuildChangeSet(), cancellation.Token)
            .ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, apply.Status, "SetConfig succeeds");
        if (apply.IsOk)
        {
            draft.AdoptConfirmed(apply.Value!);
            Harness.CheckEqual(target, apply.Value!.Values.Children["PageSize"].Scalar,
                "the Host's reply carries the new value");
            Harness.Check(!draft.HasChanges, "and the draft is clean afterwards");

            var reread = await client.GetConfigAsync(pinyinUri, cancellation.Token).ConfigureAwait(false);
            Harness.CheckEqual(target, reread.Value!.Values.Children["PageSize"].Scalar,
                "a fresh read confirms the value persisted");

            // Restore, so a reused isolated Host is left as it was found.
            var restore = new SettingsDraft(ConfigIdentity.ForUri(pinyinUri), reread.Value!);
            restore.TrySet("PageSize", SettingsValue.Text(original));
            var restored = await client.SetConfigAsync(pinyinUri, restore.BuildChangeSet(),
                                                       cancellation.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Ok, restored.Status, "the original value is restored");
        }

        Harness.Section("integration: a key list round trips through the Host");
        var current = await client.GetConfigAsync(pinyinUri, cancellation.Token).ConfigureAwait(false);
        var currentList = KeyBindingList.FromValue(current.Value!.Values.Children["PrevPage"]);
        var edited = currentList.Clone();
        if (edited.Count > 1)
        {
            edited.RemoveAt(edited.Count - 1);
        }
        else
        {
            edited.Add(KeyChord.Parse("Left")!);
        }

        var keyDraft = new SettingsDraft(ConfigIdentity.ForUri(pinyinUri), current.Value!);
        Harness.Check(keyDraft.TrySet("PrevPage", edited.ToValue()), "the key list edit is accepted");
        var keyApply = await client.SetConfigAsync(pinyinUri, keyDraft.BuildChangeSet(),
                                                   cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, keyApply.Status, "the key list applies");
        if (keyApply.IsOk)
        {
            var after = KeyBindingList.FromValue(keyApply.Value!.Values.Children["PrevPage"]);
            Harness.CheckEqual(edited.Count, after.Count, "the list kept its length");
            Harness.CheckEqual(edited[0].ToString(), after[0].ToString(), "and its first binding");

            var undo = new SettingsDraft(ConfigIdentity.ForUri(pinyinUri), keyApply.Value!);
            undo.TrySet("PrevPage", currentList.ToValue());
            var undone = await client.SetConfigAsync(pinyinUri, undo.BuildChangeSet(),
                                                     cancellation.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Ok, undone.Status, "the original list is restored");
        }

        Harness.Section("integration: the two views share one configuration");
        var shuangpin = await client.GetConfigAsync("fcitx://config/inputmethod/shuangpin",
                                                    cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, shuangpin.Status, "the shuangpin view is readable");
        if (shuangpin.IsOk)
        {
            Harness.CheckEqual(current.Value!.Values.Children.Count,
                shuangpin.Value!.Values.Children.Count,
                "both views return the same value tree");
            Harness.Check(shuangpin.Value.SchemaOptionNames.Contains("ShuangpinProfile"),
                "the shuangpin view describes ShuangpinProfile");
            Harness.Check(!current.Value.SchemaOptionNames.Contains("ShuangpinProfile"),
                "the pinyin view does not, so it cannot write it");
            Harness.CheckEqual(
                ConfigIdentity.ForUri(pinyinUri).Id,
                ConfigIdentity.ForUri("fcitx://config/inputmethod/shuangpin").Id,
                "and both map to one draft identity");
        }

        Harness.Section("integration: input-method list and selection");
        var list = await client.GetImListAsync(cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, list.Status, "GetImList succeeds");
        if (list.IsOk)
        {
            Harness.Check(list.Value!.Enabled.Count > 0, "the group has at least one method");
            Harness.CheckEqual(list.Value.Enabled[0], list.Value.Active,
                "Items/0 is the active method");
            Harness.Check(list.Value.Available.Contains("pinyin"), "pinyin is available");
            Harness.CheckEqual(list.Value.Active, list.Value.Enabled[0],
                "the active method is the first enabled one");
        }

        var selection = await client.GetSelectionAsync(cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(SettingsStatus.Ok, selection.Status, "GetSelection succeeds");
        if (selection.IsOk)
        {
            Harness.Check(selection.Value!.Available.Contains("pinyin"), "pinyin can be selected");
        }

        Harness.Section("integration: an unknown URI is refused by the Host, not guessed");
        var unknown = await client.GetConfigAsync("fcitx://config/addon/does-not-exist",
                                                   cancellation.Token).ConfigureAwait(false);
        Harness.Check(unknown.Status is SettingsStatus.Invalid or SettingsStatus.Unsupported,
            "the Host reports an invalid or unsupported URI");
        Harness.Check(unknown.Error.Length > 0, "with an explanation");
    }
}
