using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// The two paths that decide whether a user's work survives: an uncertain method-list write,
/// and a connection that comes back.
/// </summary>
/// <remarks>
/// Both are driven through <see cref="SettingsWorkspace"/> — the same type the window uses —
/// against a scripted pipe peer, because the interesting cases are a specific sequence of
/// replies rather than whatever a real Host happens to do. A transport-level check would only
/// prove the client classified one reply; these prove what the workspace then holds.
/// </remarks>
internal static class RecoveryTests
{
    private static readonly string[] OldOrder = { "pinyin", "shuangpin" };
    private static readonly string[] RequestedOrder = { "shuangpin", "pinyin" };

    public static async Task RunAsync()
    {
        await UncertainMethodWriteAsync().ConfigureAwait(false);
        await ReconnectAsync().ConfigureAwait(false);
        await CleanListReconnectAsync().ConfigureAwait(false);
        await PendingListReconnectAsync().ConfigureAwait(false);
        await PendingListAlreadyOnHostAsync().ConfigureAwait(false);
        await RetryAfterFailedStartAsync().ConfigureAwait(false);
    }

    /// <summary>
    /// An uncertain write, followed by a reread that either shows the request or does not.
    /// </summary>
    private static async Task UncertainMethodWriteAsync()
    {
        await UncertainCaseAsync("applied", "shows the request").ConfigureAwait(false);
        await UncertainCaseAsync("not applied", "shows the old list").ConfigureAwait(false);
        await UncertainCaseAsync("reread failed", "fails").ConfigureAwait(false);
    }

    private static async Task UncertainCaseAsync(string scenario, string description)
    {
        Harness.Section($"method list: uncertain write, then a reread that {description}");

        var name = FakeHost.UniqueName();
        var listReads = 0;
        var writes = 0;

        await using var peer = FakeHost.StartScripted(name, (type, _) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return SnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    listReads++;
                    if (listReads == 1)
                    {
                        // The load.
                        return ImListBody(OldOrder, OldOrder);
                    }

                    return scenario switch
                    {
                        "applied" => ImListBody(RequestedOrder, RequestedOrder),
                        "not applied" => ImListBody(OldOrder, OldOrder),
                        // A reread that never answers: the outcome stays unresolved.
                        _ => null,
                    };
                case MessageType.SettingsSetImListRequest:
                    writes++;
                    // The Host accepted the request but the reply did not arrive, so whether it
                    // applied the list is unknown to the client.
                    return StatusBody(SettingsStatus.Indeterminate, "the reply was lost");
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the order is changed in the draft");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);

        Harness.CheckEqual(1, writes, "the mutation is sent exactly once and never replayed");
        Harness.Check(workspace.SaveSummary.Contains("不确定", StringComparison.Ordinal),
            "the outcome is described as uncertain rather than as saved");

        switch (scenario)
        {
            case "applied":
                Harness.Check(!workspace.Methods!.HasChanges,
                    "a reread that shows the request clears the pending change");
                Harness.CheckEqual("shuangpin", workspace.Methods.Enabled[0],
                    "and the draft shows what the Host now holds");
                Harness.Check(workspace.SaveSummary.Contains("已生效", StringComparison.Ordinal),
                    "the summary says the reread shows it applied");
                break;

            case "not applied":
                Harness.Check(workspace.Methods!.HasChanges,
                    "a reread that does not show the request keeps it pending");
                Harness.CheckEqual("shuangpin", workspace.Methods.Enabled[0],
                    "with the order the user asked for, not the one the Host still has");
                Harness.CheckEqual(2, workspace.Methods.Enabled.Count, "and no entry is lost");
                Harness.Check(workspace.SaveSummary.Contains("尚未生效", StringComparison.Ordinal),
                    "the summary says it has not applied");
                Harness.Check(!workspace.SaveSummary.Contains("已保存", StringComparison.Ordinal),
                    "and does not describe it as saved");
                break;

            default:
                Harness.Check(workspace.Methods!.HasChanges,
                    "a failed reread keeps the request pending");
                Harness.CheckEqual("shuangpin", workspace.Methods.Enabled[0],
                    "with the requested order intact");
                Harness.Check(workspace.SaveSummary.Contains("重新读取也失败", StringComparison.Ordinal),
                    "the summary reports the unresolved state instead of success");
                break;
        }
    }

    /// <summary>
    /// A connection lost with edits pending, then a Host that comes back.
    /// </summary>
    private static async Task ReconnectAsync()
    {
        Harness.Section("reconnect: a lost connection keeps the edits and the retry saves them");

        var name = FakeHost.UniqueName();
        var stored = OldOrder.ToList();
        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        var first = FakeHost.StartScripted(name, (type, _) => type switch
        {
            MessageType.SettingsGetConfigRequest => SnapshotBody(),
            MessageType.SettingsGetImListRequest => ImListBody(OldOrder, OldOrder),
            _ => null,
        });

        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads from the first Host");
        Harness.Check(workspace.IsConnected, "and reports itself connected");
        Harness.Check(!workspace.NeedsRetry, "so no retry is offered yet");

        await first.DisposeAsync().ConfigureAwait(false);

        Harness.Check(workspace.MoveMethod(0, 1), "an order change is pending");
        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);

        Harness.Check(!workspace.IsConnected, "a save that cannot reach the Host marks the connection lost");
        Harness.Check(workspace.NeedsRetry, "which is what makes the retry action visible");
        Harness.Check(workspace.HasChanges, "the pending change survives the failed save");
        Harness.CheckEqual("shuangpin", workspace.Methods!.Enabled[0],
            "with the order the user asked for");
        Harness.Check(workspace.IsLoaded, "and the window still has state to show");

        // A Host comes back on the same endpoint and still holds the old order.
        await using var second = FakeHost.StartScripted(name, (type, payload) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return SnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(stored.ToArray(), OldOrder);
                case MessageType.SettingsSetImListRequest:
                    stored = DecodeImListPayload(payload);
                    return ImListBody(stored.ToArray(), OldOrder);
                default:
                    return null;
            }
        });

        Harness.Check(await workspace.RetryAsync(cancellation.Token).ConfigureAwait(false),
            "the retry reconnects");
        Harness.Check(workspace.IsConnected, "the connection is back");
        Harness.Check(!workspace.NeedsRetry, "so the retry action goes away");
        Harness.Check(workspace.HasChanges, "and the pending change is still pending");
        Harness.CheckEqual("shuangpin", workspace.Methods!.Enabled[0],
            "with the requested order, because the Host had not applied it");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Harness.Check(!workspace.HasChanges, "saving after the reconnect applies the change");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", stored),
            "and the Host holds the requested order");
    }

    /// <summary>
    /// A list nobody edited is not user intent. When the Host's order has changed, the reconnect
    /// must adopt it; keeping the cached order would present it as a local edit and let a later
    /// save submit a reversal nobody asked for.
    /// </summary>
    private static async Task CleanListReconnectAsync()
    {
        Harness.Section("reconnect: a clean list adopts the Host's order instead of becoming an edit");

        var name = FakeHost.UniqueName();
        var hostOrder = OldOrder;
        var listWrites = 0;
        var configWrites = 0;

        await using var peer = FakeHost.StartScripted(name, (type, _) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return ConfigurableSnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(hostOrder, hostOrder);
                case MessageType.SettingsSetImListRequest:
                    listWrites++;
                    return ImListBody(hostOrder, hostOrder);
                case MessageType.SettingsSetConfigRequest:
                    configWrites++;
                    return ConfigurableSnapshotBody();
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.CheckEqual("pinyin,shuangpin", string.Join(",", workspace.Methods!.Enabled),
            "with the list the Host holds");
        Harness.Check(!workspace.HasChanges, "and nothing pending: the list was never edited");

        // The Host's authoritative list changes while this window is away.
        hostOrder = RequestedOrder;

        Harness.Check(await workspace.RetryAsync(cancellation.Token).ConfigureAwait(false),
            "the retry reconnects");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", workspace.Methods!.Enabled),
            "the displayed list adopts the Host's new order");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", workspace.Methods.Baseline.Enabled),
            "and the baseline moves with it");
        Harness.Check(!workspace.Methods.HasChanges,
            "the cached list is not turned into a local edit");
        Harness.Check(!workspace.HasChanges, "so the workspace has nothing to save");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(0, listWrites, "a save with nothing pending sends no SetImList");
        Harness.Check(workspace.SaveSummary.Contains("没有需要保存", StringComparison.Ordinal),
            "and says there is nothing to save");

        // A configuration-only edit after the clean reconnect must not drag the list along.
        Harness.Check(workspace.TrySet(SettingsTarget.Global, "PageSize", SettingsValue.Text("6")),
            "a Global edit is accepted");
        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Harness.CheckEqual(1, configWrites, "the configuration edit is saved");
        Harness.CheckEqual(0, listWrites, "and it sends no SetImList");
        Harness.Check(!workspace.HasChanges, "with nothing left pending");
    }

    /// <summary>
    /// A list the user really did edit is intent. The reconnect moves the baseline to the Host and
    /// leaves the request pending, and the reconnect itself sends nothing.
    /// </summary>
    private static async Task PendingListReconnectAsync()
    {
        Harness.Section("reconnect: a pending list keeps the request while the baseline moves");

        var name = FakeHost.UniqueName();
        var listWrites = 0;

        await using var peer = FakeHost.StartScripted(name, (type, _) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return SnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(OldOrder, OldOrder);
                case MessageType.SettingsSetImListRequest:
                    listWrites++;
                    return ImListBody(OldOrder, OldOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the user reorders the list");
        Harness.Check(workspace.Methods!.HasChanges, "so the list has a pending request");

        Harness.Check(await workspace.RetryAsync(cancellation.Token).ConfigureAwait(false),
            "the retry reconnects");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", workspace.Methods!.Enabled),
            "the requested order is still what the window shows");
        Harness.CheckEqual("pinyin,shuangpin", string.Join(",", workspace.Methods.Baseline.Enabled),
            "while the baseline reflects what the Host actually holds");
        Harness.Check(workspace.Methods.HasChanges, "so the request is still pending");
        Harness.Check(workspace.HasChanges, "and the workspace still has work to save");
        Harness.CheckEqual(0, listWrites, "the retry itself sends no mutation");
    }

    /// <summary>
    /// The pending request and the Host's list agree, so the request is no longer a change — and
    /// nothing is replayed to get there.
    /// </summary>
    private static async Task PendingListAlreadyOnHostAsync()
    {
        Harness.Section("reconnect: a pending request the Host already reflects becomes clean");

        var name = FakeHost.UniqueName();
        var hostOrder = OldOrder;
        var listWrites = 0;

        await using var peer = FakeHost.StartScripted(name, (type, _) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return SnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(hostOrder, hostOrder);
                case MessageType.SettingsSetImListRequest:
                    listWrites++;
                    return ImListBody(hostOrder, hostOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the user reorders the list");

        // The write landed through another path, or the Host applied it before the reply was lost.
        hostOrder = RequestedOrder;

        Harness.Check(await workspace.RetryAsync(cancellation.Token).ConfigureAwait(false),
            "the retry reconnects");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", workspace.Methods!.Enabled),
            "the list still shows the requested order");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", workspace.Methods.Baseline.Enabled),
            "which is now also the baseline");
        Harness.Check(!workspace.Methods.HasChanges,
            "so the request is no longer a change");
        Harness.Check(!workspace.HasChanges, "and the workspace has nothing left to save");
        Harness.CheckEqual(0, listWrites, "nothing was replayed to reach that state");
    }

    /// <summary>
    /// The first load never succeeded, so there are no drafts for a retry to preserve.
    /// </summary>
    private static async Task RetryAfterFailedStartAsync()
    {
        Harness.Section("reconnect: a retry after a failed start creates the drafts");

        var name = FakeHost.UniqueName();
        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));

        Harness.Check(!await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "a Host that is not there does not load");
        Harness.Check(workspace.NeedsRetry, "the window is told a retry is meaningful");
        Harness.Check(!workspace.HasDrafts, "with no draft invented from nothing");
        Harness.Check(!workspace.IsLoaded, "and no state to show");

        await using var late = FakeHost.StartScripted(name, (type, _) => type switch
        {
            MessageType.SettingsGetConfigRequest => SnapshotBody(),
            MessageType.SettingsGetImListRequest => ImListBody(OldOrder, OldOrder),
            _ => null,
        });

        Harness.Check(await workspace.RetryAsync(cancellation.Token).ConfigureAwait(false),
            "the retry connects once the Host exists");
        Harness.Check(workspace.HasDrafts, "and creates the drafts it did not have");
        Harness.Check(workspace.IsLoaded, "so the window has state to show");
        Harness.Check(!workspace.HasChanges, "with nothing pending");
    }

    private static byte[] SnapshotBody() =>
        Bytes.SnapshotPayload(Bytes.Map(("PageSize", Bytes.Scalar("5"))));

    /// <summary>
    /// The same values with a schema, so a check can make an ordinary configuration edit. The
    /// reconnect checks for the list itself need no schema; the configuration-only save does,
    /// because the draft refuses a field the view does not describe.
    /// </summary>
    private static byte[] ConfigurableSnapshotBody() =>
        Bytes.SnapshotPayload(
            Bytes.Map(("PageSize", Bytes.Scalar("5"))),
            ("GlobalConfig", new[]
            {
                Bytes.Option("PageSize", "Integer", Bytes.Scalar("5"), Bytes.EmptyProperties()),
            }));

    private static byte[] StatusBody(SettingsStatus status, string error) =>
        Bytes.Concat(Bytes.U8((byte)status), Bytes.U8(0), Bytes.Str(error));

    private static byte[] ImListBody(string[] enabled, string[] available) =>
        Bytes.Concat(Bytes.U8(0), Bytes.U8(0), Bytes.Str(string.Empty),
                     Bytes.Str(enabled.Length == 0 ? string.Empty : enabled[0]),
                     Bytes.Str("pinyin"),
                     Bytes.U32((uint)enabled.Length),
                     Bytes.Concat(enabled.Select(Bytes.Str).ToArray()),
                     Bytes.U32((uint)available.Length),
                     Bytes.Concat(available.Select(Bytes.Str).ToArray()));

    /// <summary>Reads the list a SetImList request carries: a count, then that many strings.</summary>
    private static List<string> DecodeImListPayload(byte[] payload)
    {
        var reader = new ProtoReader(payload);
        if (!reader.TryU32(out var count) || count > WireLimits.MaxVectorLength)
        {
            return new List<string>();
        }

        var items = new List<string>((int)count);
        for (var i = 0; i < count; i++)
        {
            if (!reader.TryString(out var item))
            {
                return new List<string>();
            }

            items.Add(item);
        }

        return items;
    }
}
