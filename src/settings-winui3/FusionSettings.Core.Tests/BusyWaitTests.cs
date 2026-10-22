using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// One click on 保存, and a Host that answers <c>Busy</c> because a buffer is live.
/// </summary>
/// <remarks>
/// <para>
/// <c>Busy</c> is the one answer this Host gives without having written anything: the
/// Pinyin/Shuangpin configuration and the input-method list are stored by the same official call
/// that applies them, and that call ends the composition, so the request is refused while the
/// user is mid-word. The window therefore waits for the Host and resubmits, which is what these
/// sections check — against a scripted peer, because the interesting part is a specific sequence
/// of answers (busy, busy, stored) rather than whatever a real Host happens to do.
/// </para>
/// <para>
/// The other half is what is *not* retried. A timeout, a dropped connection, an
/// <c>Indeterminate</c> outcome and an ordinary refusal must each end the same save in exactly
/// one request, so that the wait is a property of <c>Busy</c> alone.
/// </para>
/// </remarks>
internal static class BusyWaitTests
{
    private static readonly string[] OldOrder = { "pinyin", "shuangpin" };
    private static readonly string[] RequestedOrder = { "shuangpin", "pinyin" };

    public static async Task RunAsync()
    {
        await WaitsThenSavesAsync().ConfigureAwait(false);
        await DoesNotResubmitASavedPageAsync().ConfigureAwait(false);
        await CanBeStoppedAsync().ConfigureAwait(false);
        await DoesNotRetryOtherAnswersAsync().ConfigureAwait(false);
        foreach (var failure in new[] { SettingsStatus.Error, SettingsStatus.Invalid,
                                        SettingsStatus.Indeterminate })
        {
            await AnotherPageFailsWhileOneWaitsAsync(failure).ConfigureAwait(false);
        }

        await DoesNotResubmitTheFailedGroupOfTheSharedDraftAsync().ConfigureAwait(false);
    }

    /// <summary>
    /// The Host says busy twice and then stores the list. One call to <see cref="SettingsWorkspace.SaveAsync"/>
    /// finishes the job; the user never presses 保存 a second time.
    /// </summary>
    private static async Task WaitsThenSavesAsync()
    {
        Harness.Section("busy: one save waits for the Host and finishes by itself");

        var name = FakeHost.UniqueName();
        var listWrites = 0;
        var stored = OldOrder.ToList();

        await using var peer = FakeHost.StartScripted(name, (type, payload) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return SnapshotBody();
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(stored.ToArray(), OldOrder);
                case MessageType.SettingsSetImListRequest:
                    ++listWrites;
                    if (listWrites <= 2)
                    {
                        // A buffer is live: the official setter cannot be called yet, and the
                        // Host wrote nothing.
                        return BusyBody();
                    }

                    stored = DecodeImListPayload(payload);
                    return ImListBody(stored.ToArray(), OldOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the user reorders the list");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");

        Harness.CheckEqual(3, listWrites, "the busy answers are retried until the Host is free");
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", stored),
            "the order the user asked for is what the Host stored");
        Harness.Check(!workspace.Methods!.HasChanges, "and the window has nothing left to save");
        Harness.Check(!workspace.IsWaitingForHost, "the wait is over once it lands");
        Harness.Check(workspace.SaveSummary.Contains("已保存", StringComparison.Ordinal),
            "the bar says saved, because it is");
        Harness.Check(!workspace.SaveSummary.Contains("尚未保存", StringComparison.Ordinal),
            "and no longer says it is waiting");
    }

    /// <summary>
    /// A page that landed on the first attempt is not sent again while another page waits.
    /// </summary>
    private static async Task DoesNotResubmitASavedPageAsync()
    {
        Harness.Section("busy: a page that already saved is not submitted again");

        var name = FakeHost.UniqueName();
        var configWrites = 0;
        var listWrites = 0;
        var stored = OldOrder.ToList();
        // The value the Host holds; the check moves it forward just before the save, the way a
        // real Host would after storing the request.
        var storedPageSize = "5";

        await using var peer = FakeHost.StartScripted(name, (type, payload) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return ConfigurableSnapshotBody(storedPageSize);
                case MessageType.SettingsSetConfigRequest:
                    ++configWrites;
                    return ConfigurableSnapshotBody(storedPageSize);
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(stored.ToArray(), OldOrder);
                case MessageType.SettingsSetImListRequest:
                    ++listWrites;
                    if (listWrites == 1)
                    {
                        return BusyBody();
                    }

                    stored = DecodeImListPayload(payload);
                    return ImListBody(stored.ToArray(), OldOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.TrySet(SettingsTarget.Global, "PageSize", SettingsValue.Text("6")),
            "a global value is changed");
        storedPageSize = "6";
        Harness.Check(workspace.MoveMethod(0, 1), "and the list is reordered in the same save");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);

        Harness.CheckEqual(1, configWrites,
            "the page that was stored is not sent again when the other one is retried");
        Harness.CheckEqual(2, listWrites, "only the page that was refused is resubmitted");
        Harness.Check(!workspace.HasChanges, "both pages end up saved");
        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("全局配置：已保存", StringComparison.Ordinal) &&
                      workspace.SaveSummary.Contains("输入方式列表：已保存", StringComparison.Ordinal),
            "and each page keeps exactly one sentence");
    }

    /// <summary>
    /// The user stops waiting. The retries end, the edit stays unsaved, and the window is idle
    /// again — a request already sent is awaited rather than abandoned.
    /// </summary>
    private static async Task CanBeStoppedAsync()
    {
        Harness.Section("busy: the wait can be stopped, and what is left is unsaved");

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
                    ++listWrites;
                    return BusyBody();
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the user reorders the list");

        var waiting = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnChanged(object? sender, PropertyChangedEventArgs args)
        {
            if (args.PropertyName == nameof(SettingsWorkspace.IsWaitingForHost) &&
                workspace.IsWaitingForHost)
            {
                waiting.TrySetResult();
            }
        }

        workspace.PropertyChanged += OnChanged;
        Task save;
        try
        {
            save = workspace.SaveAsync(cancellation.Token);
            var reached = await Task.WhenAny(waiting.Task, save) == waiting.Task;
            Harness.Check(reached, "the save reports that it is waiting for the Host");
            Harness.Check(workspace.IsWaitingForHost, "and offers to stop waiting");
            Console.WriteLine($"   while waiting it said: {workspace.SaveSummary.Replace("\n", " / ")}");
            Harness.Check(workspace.SaveSummary.Contains("尚未保存", StringComparison.Ordinal),
                "the bar says the change is not saved, not that the Host took it");
            Harness.Check(!workspace.SaveSummary.Contains("已保存", StringComparison.Ordinal),
                "and never calls a wait a save");
            Harness.Check(workspace.Methods!.HasChanges, "the edit is still an unsaved change");

            workspace.StopWaiting();
        }
        finally
        {
            workspace.PropertyChanged -= OnChanged;
        }

        await save.ConfigureAwait(false);
        Console.WriteLine($"   after stopping it said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.IsWaitingForHost, "stopping the wait ends it");
        Harness.Check(workspace.Methods!.HasChanges, "and leaves the edit unsaved");
        Harness.Check(workspace.SaveSummary.Contains("停止自动重试", StringComparison.Ordinal),
            "the bar says the retries were stopped");

        var writesAtStop = listWrites;
        await Task.Delay(900).ConfigureAwait(false);
        Harness.CheckEqual(writesAtStop, listWrites,
            "no request is sent after the wait was stopped");

        // And the window is usable again: a reload is not blocked by a stopped wait.
        Harness.Check(await workspace.ReloadAsync(preserveEdits: true, cancellation.Token)
                .ConfigureAwait(false),
            "a reload still works after a stopped wait");
        Harness.Check(!workspace.IsWaitingForHost, "and does not resume the wait");
    }

    /// <summary>
    /// Nothing except <c>Busy</c> is retried: an ordinary refusal ends the save in one request.
    /// </summary>
    private static async Task DoesNotRetryOtherAnswersAsync()
    {
        Harness.Section("busy: only a refusal that wrote nothing is retried");

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
                    ++listWrites;
                    return StatusBody(SettingsStatus.Error, "the profile could not be written");
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.MoveMethod(0, 1), "the user reorders the list");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);

        Harness.CheckEqual(1, listWrites, "a write that failed is not retried by itself");
        Harness.Check(workspace.Methods!.HasChanges, "the edit is kept as unsaved");
        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal) &&
                      !workspace.SaveSummary.Contains("等待", StringComparison.Ordinal),
            "and the bar does not claim to be waiting for anything");
    }

    /// <summary>
    /// One save, two pages: one is waiting for the Host, the other has already failed. The page
    /// that failed is submitted exactly once — a retry belongs to the <c>Busy</c> page, not to
    /// the save — and it keeps its edit, so the user can see what is still unsaved.
    /// </summary>
    /// <remarks>
    /// Parameterized over the three ways a page can fail without being worth another attempt:
    /// a plain refusal, a schema rejection, and an outcome the Host could not report. None of
    /// them may ride along on another page's wait.
    /// </remarks>
    private static async Task AnotherPageFailsWhileOneWaitsAsync(SettingsStatus failure)
    {
        Harness.Section($"busy: another page failing with {failure} is not resubmitted while one waits");

        var name = FakeHost.UniqueName();
        var configWrites = 0;
        var listWrites = 0;
        var stored = OldOrder.ToList();
        var storedPageSize = "5";

        await using var peer = FakeHost.StartScripted(name, (type, payload) =>
        {
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return ConfigurableSnapshotBody(storedPageSize);
                case MessageType.SettingsSetConfigRequest:
                    ++configWrites;
                    return StatusBody(failure, "the page could not be written");
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(stored.ToArray(), OldOrder);
                case MessageType.SettingsSetImListRequest:
                    ++listWrites;
                    if (listWrites == 1)
                    {
                        return BusyBody();
                    }

                    stored = DecodeImListPayload(payload);
                    return ImListBody(stored.ToArray(), OldOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.TrySet(SettingsTarget.Global, "PageSize", SettingsValue.Text("6")),
            "a global value is changed");
        Harness.Check(workspace.MoveMethod(0, 1), "and the list is reordered in the same save");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");

        Harness.CheckEqual(2, listWrites, "the page that was waiting is retried until it lands");
        Harness.CheckEqual(1, configWrites,
            $"the page that answered {failure} is submitted once and never re-sent");
        Harness.Check(workspace.GlobalDraft!.HasChanges,
            "and its edit is still an unsaved change");
        Harness.Check(!workspace.Methods!.HasChanges, "while the page that landed has nothing left");
        Harness.Check(workspace.SaveSummary.Contains("输入方式列表：已保存", StringComparison.Ordinal),
            "the bar reports the page that was written as saved");
        // Each failure keeps its own sentence — an uncertain outcome stays "may have applied",
        // a refusal stays "not saved" — and none of them becomes "waiting" or "saved".
        Harness.Check(!workspace.SaveSummary.Contains("全局配置：已保存", StringComparison.Ordinal),
            "the page that failed is never reported as saved");
        Harness.Check(workspace.SaveSummary.Contains(
                failure == SettingsStatus.Indeterminate ? "全局配置：写入结果不确定"
                                                       : "全局配置：未保存",
                StringComparison.Ordinal),
            "and it keeps its own words rather than the waiting page's");
    }

    /// <summary>
    /// The one shared Pinyin/Shuangpin draft is submitted as two requests, so it is retried as
    /// two: the group that is waiting is resubmitted, and the group that failed is not — even
    /// though both live in one draft and both are still unsaved.
    /// </summary>
    private static async Task DoesNotResubmitTheFailedGroupOfTheSharedDraftAsync()
    {
        Harness.Section("busy: the failed half of the shared draft is not resubmitted");

        const string pinyinUri = SettingsWorkspace.PinyinUri;
        const string shuangpinUri = SettingsWorkspace.ShuangpinUri;
        var name = FakeHost.UniqueName();
        var pinyinWrites = 0;
        var shuangpinWrites = 0;
        var storedPageSize = "5";

        await using var peer = FakeHost.StartScripted(name, (type, payload) =>
        {
            var uri = UriOf(payload);
            switch (type)
            {
                case MessageType.SettingsGetConfigRequest:
                    return FamilyBody(storedPageSize, uri == shuangpinUri);
                case MessageType.SettingsSetConfigRequest when uri == shuangpinUri:
                    ++shuangpinWrites;
                    return StatusBody(SettingsStatus.Error, "the Shuangpin page could not be written");
                case MessageType.SettingsSetConfigRequest when uri == pinyinUri:
                    ++pinyinWrites;
                    if (pinyinWrites == 1)
                    {
                        return BusyBody();
                    }

                    storedPageSize = "6";
                    return FamilyBody(storedPageSize, shuangpin: false);
                case MessageType.SettingsGetImListRequest:
                    return ImListBody(OldOrder, OldOrder);
                default:
                    return null;
            }
        });

        var workspace = new SettingsWorkspace(name);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        Harness.Check(await workspace.InitializeAsync(cancellation.Token).ConfigureAwait(false),
            "the workspace loads");
        Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, "PageSize", SettingsValue.Text("6")),
            "a field the Pinyin schema describes is changed");
        Harness.Check(workspace.TrySet(SettingsTarget.Shuangpin, "FuzzyOne", SettingsValue.Text("2")),
            "and a field only the Shuangpin schema describes is changed in the same draft");

        await workspace.SaveAsync(cancellation.Token).ConfigureAwait(false);
        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");

        Harness.CheckEqual(2, pinyinWrites, "the group that was waiting is retried");
        Harness.CheckEqual(1, shuangpinWrites,
            "the group that failed is submitted once, although it shares the draft");
        Harness.CheckEqual("2", workspace.PinyinDraft!.Effective("FuzzyOne").Scalar,
            "and its edit is still there, unsaved");
        Harness.Check(workspace.PinyinDraft.HasChanges,
            "so the draft still reports something to save");
        Harness.CheckEqual("6", workspace.PinyinDraft.Effective("PageSize").Scalar,
            "while the group that landed keeps the value the user asked for");
        Harness.Check(workspace.SaveSummary.Contains("双拼：未保存", StringComparison.Ordinal),
            "the bar reports the failed group as unsaved");
        Harness.Check(workspace.SaveSummary.Contains("拼音/双拼：已保存", StringComparison.Ordinal),
            "and the retried group as saved");
    }

    private static string UriOf(byte[] payload)
    {
        var reader = new ProtoReader(payload);
        return reader.TryString(out var uri) ? uri : string.Empty;
    }

    /// <summary>
    /// One configuration's values and schema, disjoint between the two Pinyin URIs so the shared
    /// draft really has to split: the Pinyin schema describes <c>PageSize</c> and the Shuangpin
    /// schema describes <c>FuzzyOne</c>, and neither describes the other's field.
    /// </summary>
    private static byte[] FamilyBody(string pageSize, bool shuangpin)
    {
        var values = Bytes.Map(("PageSize", Bytes.Scalar(pageSize)),
                               ("FuzzyOne", Bytes.Scalar("1")));
        return shuangpin
            ? Bytes.SnapshotPayload(values, ("ShuangpinConfig", new[]
            {
                Bytes.Option("FuzzyOne", "Integer", Bytes.Scalar("1"), Bytes.EmptyProperties()),
            }))
            : Bytes.SnapshotPayload(values, ("PinyinConfig", new[]
            {
                Bytes.Option("PageSize", "Integer", Bytes.Scalar("5"), Bytes.EmptyProperties()),
            }));
    }

    private static byte[] SnapshotBody() =>
        Bytes.SnapshotPayload(Bytes.Map(("PageSize", Bytes.Scalar("5"))));

    /// <summary>Values with a schema, so a check can make an ordinary configuration edit.</summary>
    private static byte[] ConfigurableSnapshotBody(string pageSize) =>
        Bytes.SnapshotPayload(
            Bytes.Map(("PageSize", Bytes.Scalar(pageSize))),
            ("GlobalConfig", new[]
            {
                Bytes.Option("PageSize", "Integer", Bytes.Scalar("5"), Bytes.EmptyProperties()),
            }));

    private static byte[] BusyBody() => StatusBody(SettingsStatus.Busy,
        "A buffer is live, so nothing was written.");

    private static byte[] StatusBody(SettingsStatus status, string error) =>
        Bytes.Concat(Bytes.U8((byte)status), Bytes.Str(error));

    private static byte[] ImListBody(string[] enabled, string[] available) =>
        Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty),
                     Bytes.Str(enabled.Length == 0 ? string.Empty : enabled[0]),
                     Bytes.Str("pinyin"),
                     Bytes.U32((uint)enabled.Length),
                     Bytes.Concat(enabled.Select(Bytes.Str).ToArray()),
                     Bytes.U32((uint)available.Length),
                     Bytes.Concat(available.Select(Bytes.Str).ToArray()));

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
