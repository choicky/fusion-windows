using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// The edit/save/recovery path itself, driven against a real Host on an isolated pipe.
/// </summary>
/// <remarks>
/// The core checks prove the protocol and the draft model; they say nothing about whether a
/// control's edit reaches the right configuration, or whether a Shuangpin-only field can be
/// saved. Those are the workspace's job, so they are checked here through the same type the
/// window uses.
/// </remarks>
internal static class WorkspaceTests
{
    public static async Task RunAsync()
    {
        await RetryPathAsync().ConfigureAwait(false);

        var configured = Environment.GetEnvironmentVariable("FUSION_SETTINGS_TEST_PIPE");
        if (string.IsNullOrWhiteSpace(configured))
        {
            Harness.Section("workspace: SKIPPED (FUSION_SETTINGS_TEST_PIPE is not set)");
            return;
        }

        var endpoint = PipeEndpoint.Normalize(configured);
        if (endpoint is null || PipeEndpoint.IsProduction(endpoint))
        {
            throw new InvalidOperationException(
                "FUSION_SETTINGS_TEST_PIPE must name an isolated endpoint, not the production one");
        }

        Harness.Section($"workspace: real Host on {endpoint}");
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(120));
        var workspace = new SettingsWorkspace(endpoint);
        var loaded = await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false);
        Harness.Check(loaded, "the workspace loads");
        if (!loaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        Harness.Check(workspace.PinyinDraft is not null, "the Pinyin draft exists");
        Harness.Check(workspace.GlobalDraft is not null, "the Global draft exists");
        Harness.Check(workspace.Methods is not null, "the input-method draft exists");
        Harness.Check(!workspace.HasChanges, "a fresh workspace has nothing pending");

        await GlobalRoutingAsync(workspace, endpoint, cancellation.Token).ConfigureAwait(false);
        await ShuangpinSaveAsync(workspace, endpoint, cancellation.Token).ConfigureAwait(false);
        await ClearListAsync(workspace, endpoint, cancellation.Token).ConfigureAwait(false);
        await MethodListAsync(workspace, endpoint, cancellation.Token).ConfigureAwait(false);
    }

    private static async Task RetryPathAsync()
    {
        Harness.Section("workspace: a failed start offers a retry");

        var workspace = new SettingsWorkspace("fusion-settings-absent-" + Guid.NewGuid().ToString("N")[..6]);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var loaded = await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false);
        Harness.Check(!loaded, "a Host that is not there does not load");
        Harness.Check(workspace.NeedsRetry, "and the window is told a retry is meaningful");
        Harness.Check(workspace.PinyinDraft is null, "with no draft invented from nothing");
    }

    /// <summary>
    /// A Global control must reach the Global draft, not the Pinyin one.
    /// </summary>
    /// <remarks>
    /// This is the defect the correction fixes, so it is checked directly. A successful global
    /// <b>save</b> cannot be exercised on this Host: the isolated Host runs without a user
    /// config path, and the Host's global apply ends in <c>safeSave()</c>, which needs one — it
    /// answers "Fcitx could not save the global configuration; reload before retrying." That is
    /// a Host/environment constraint, not a client one, and the client's handling of it (report
    /// the failure, keep the edits) is what is asserted here instead.
    /// </remarks>
    private static async Task GlobalRoutingAsync(SettingsWorkspace workspace, string endpoint,
                                                 CancellationToken cancellationToken)
    {
        Harness.Section("workspace: a Global edit goes to the Global draft");

        var snapshot = workspace.SnapshotFor(SettingsTarget.Global)!;
        var behavior = snapshot.Types.First(type => type.Name == "Behavior$BehaviorConfig");
        var option = behavior.Options.First(o => o.Type == "Boolean");
        var current = workspace.GlobalDraft!.EffectiveNested(snapshot, behavior.Name, option.Name);
        var flipped = current.Scalar == "True" ? "False" : "True";

        Harness.Check(
            workspace.TrySetNested(SettingsTarget.Global, behavior.Name, option.Name,
                SettingsValue.Text(flipped)),
            "the Global edit is accepted");
        Harness.Check(workspace.GlobalDraft.HasChanges, "it lands in the Global draft");
        Harness.Check(!workspace.PinyinDraft!.HasChanges,
            "and not in the Pinyin draft, which is where it used to go");
        Harness.CheckEqual(flipped, workspace.GlobalDraft.EffectiveNested(snapshot, behavior.Name, option.Name).Scalar,
            "and the Global draft reports the new value");

        // A Pinyin edit still goes to the shared draft, and both drafts stay separate.
        var pinyinSnapshot = workspace.SnapshotFor(workspace.CurrentTarget)!;
        var pageSize = pinyinSnapshot.FindRootOption("PageSize")!;
        var size = workspace.PinyinDraft.Effective("PageSize").Scalar;
        var next = int.TryParse(size, out var parsed) && parsed + 1 <= (pageSize.IntMaximum ?? 10)
            ? (parsed + 1).ToString(System.Globalization.CultureInfo.InvariantCulture)
            : (pageSize.IntMinimum ?? 3).ToString(System.Globalization.CultureInfo.InvariantCulture);
        Harness.Check(workspace.TrySet(workspace.CurrentTarget, "PageSize", SettingsValue.Text(next)),
            "a Pinyin edit is accepted");
        Harness.Check(workspace.PinyinDraft.HasChanges, "and it is pending in the shared draft");
        Harness.CheckEqual(1, workspace.GlobalDraft.PendingPaths.Count,
            "while the Global draft still holds exactly its own edit");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.PinyinDraft.HasChanges,
            "the Pinyin edit saves and is confirmed");
        Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the Global save is reported as a failure rather than as done");
        Harness.Check(workspace.GlobalDraft.HasChanges,
            "and the Global edit is kept for a Host that can persist it");

        // Read back the Pinyin value through a fresh client: it really changed.
        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var pinyin = await reader.GetConfigAsync(SettingsWorkspace.PinyinUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.CheckEqual(next, pinyin.Value!.Values.Children["PageSize"].Scalar,
            "the Pinyin value was written to the Host");

        workspace.Discard();
        Harness.Check(!workspace.HasChanges, "discard clears the remaining Global edit");
    }

    /// <summary>
    /// A Shuangpin-only field must save even though it is not in the Pinyin schema: the save
    /// path has to submit it through the URI that describes it.
    /// </summary>
    private static async Task ShuangpinSaveAsync(SettingsWorkspace workspace, string endpoint,
                                                 CancellationToken cancellationToken)
    {
        Harness.Section("workspace: a Shuangpin-only field saves through the Shuangpin URI");

        workspace.EditMethod("shuangpin");
        var snapshot = workspace.SnapshotFor(SettingsTarget.Shuangpin)!;
        Harness.Check(snapshot.FindRootOption("ShowShuangpinMode") is not null,
            "the Shuangpin view describes ShowShuangpinMode");
        Harness.Check(workspace.SnapshotFor(SettingsTarget.Pinyin)!.FindRootOption("ShowShuangpinMode") is null,
            "and the Pinyin view does not");

        var current = workspace.PinyinDraft!.Effective("ShowShuangpinMode").Scalar;
        var flipped = current == "True" ? "False" : "True";

        Harness.Check(workspace.TrySet(SettingsTarget.Shuangpin, "ShowShuangpinMode",
            SettingsValue.Text(flipped)), "the Shuangpin-only edit is accepted");
        Harness.Check(workspace.PinyinDraft.HasChanges, "it is pending in the shared draft");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the save reports no failure: the field went through a URI whose schema has it");
        Harness.Check(!workspace.PinyinDraft.HasChanges, "and the edit is confirmed");

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.ShuangpinUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.CheckEqual(flipped, reread.Value!.Values.Children["ShowShuangpinMode"].Scalar,
            "the Host stored the Shuangpin-only value");

        // The other view must still render it as unsaved-free: same value, different schema.
        workspace.EditMethod("pinyin");
        Harness.Check(!workspace.TrySet(SettingsTarget.Pinyin, "ShowShuangpinMode",
                SettingsValue.Text("True")),
            "the Pinyin view still refuses to edit it");

        workspace.EditMethod("shuangpin");
        workspace.TrySet(SettingsTarget.Shuangpin, "ShowShuangpinMode", SettingsValue.Text(current));
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        var restored = await reader.GetConfigAsync(SettingsWorkspace.ShuangpinUri, cancellationToken)
            .ConfigureAwait(false);
        Harness.CheckEqual(current, restored.Value!.Values.Children["ShowShuangpinMode"].Scalar,
            "and the original value is restored");
        workspace.EditMethod("pinyin");
    }

    /// <summary>Clearing the last binding must follow the Host's write contract, not its read shape.</summary>
    private static async Task ClearListAsync(SettingsWorkspace workspace, string endpoint,
                                             CancellationToken cancellationToken)
    {
        Harness.Section("workspace: clearing the last binding saves");

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var before = await reader.GetConfigAsync(SettingsWorkspace.PinyinUri, cancellationToken)
            .ConfigureAwait(false);
        var original = before.Value!.Values.Children["ThirdCandidate"];

        // ThirdCandidate is empty by default; give it one binding first so "clear" is a change.
        var list = KeyBindingList.FromValue(original);
        list.Add(KeyChord.Parse("F9")!);
        Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, "ThirdCandidate", list.ToValue()),
            "a binding is added");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "adding a binding saves");

        var afterAdd = await reader.GetConfigAsync(SettingsWorkspace.PinyinUri, cancellationToken)
            .ConfigureAwait(false);
        var storedList = KeyBindingList.FromValue(afterAdd.Value!.Values.Children["ThirdCandidate"]);
        Harness.CheckEqual(1, storedList.Count, "the Host stored one binding");

        // Now clear it: an empty list, which must be written as an empty map.
        Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, "ThirdCandidate",
            KeyBindingList.Empty().ToValue()), "the binding is cleared in the draft");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "clearing the last binding saves: an empty list is written as a map");

        var afterClear = await reader.GetConfigAsync(SettingsWorkspace.PinyinUri, cancellationToken)
            .ConfigureAwait(false);
        var cleared = KeyBindingList.FromValue(afterClear.Value!.Values.Children["ThirdCandidate"]);
        Harness.CheckEqual(0, cleared.Count, "the Host stored an empty list");
        Harness.Check(!workspace.PinyinDraft!.HasChanges, "and nothing is pending afterwards");
    }

    /// <summary>The method list is a draft too: editing it must enable Save and be saved.</summary>
    private static async Task MethodListAsync(SettingsWorkspace workspace, string endpoint,
                                              CancellationToken cancellationToken)
    {
        Harness.Section("workspace: input-method edits are pending and save");

        var methods = workspace.Methods!;
        if (methods.Enabled.Count < 2)
        {
            Harness.Check(true, "only one method is enabled, so there is nothing to reorder");
            return;
        }

        var original = methods.Enabled.ToList();
        Harness.Check(workspace.MoveMethod(0, 1), "the list can be reordered");
        Harness.Check(workspace.HasChanges,
            "a list-only edit makes the workspace dirty, which is what enables Save");

        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the list saves");
        Harness.Check(!workspace.HasChanges, "and nothing is pending afterwards");

        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetImListAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual(original[1], reread.Value!.Enabled[0],
            "the Host stored the new first entry");

        // Restore the original order.
        Harness.Check(workspace.MoveMethod(0, 1), "the order is put back");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        var restored = await reader.GetImListAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual(original[0], restored.Value!.Enabled[0], "and the original order is stored");
    }
}
