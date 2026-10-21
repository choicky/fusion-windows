using System;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// Client-level checks against a controlled pipe peer: how a mutation failure is
/// classified, and how the handshake negotiates versions.
/// </summary>
internal static class ClientTests
{
    public static async Task RunAsync()
    {
        await MutationOutcomesAsync().ConfigureAwait(false);
        await CompatibilityAsync().ConfigureAwait(false);
    }

    private static SettingsValue ChangeSet() =>
        SettingsValue.Map(("PageSize", SettingsValue.Text("9")));

    private static async Task MutationOutcomesAsync()
    {
        Harness.Section("mutation: a failure before transmission proves nothing was written");

        // No server at all: the connection cannot even be opened.
        var absent = new SettingsClient(FakeHost.UniqueName(), TimeSpan.FromSeconds(2));
        using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
        {
            var result = await absent.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Unreachable, result.Status,
                "an unreachable Host is Unreachable");
            Harness.Check(!result.MutationMayHaveBeenApplied,
                "and it does not claim the mutation may have been applied");
            Harness.Check(!result.IsUncertain, "so the outcome is not uncertain");
        }

        // The peer answers the handshake and then disconnects without reading the request.
        // That is not a deterministic mid-write failure: it covers "the connection went away
        // after the handshake", where the client cannot tell whether the request was read, so
        // the only honest answer is that the outcome is unknown.
        Harness.Section("mutation: the peer disconnects after the handshake");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.CloseAfterHello))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "a disconnect after the handshake leaves the outcome unknown");
            Harness.Check(result.MutationMayHaveBeenApplied,
                "because the request may already have been read");
            Harness.Check(result.IsUncertain, "so the caller rereads instead of retrying");
        }

        Harness.Section("mutation: the Host read the request, then closed without a reply");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenClose))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "the outcome is Indeterminate, not Unreachable");
            Harness.Check(result.MutationMayHaveBeenApplied,
                "and it records that the write may have landed");
            Harness.Check(result.IsUncertain, "which is what a caller reconciles against");
            Harness.CheckEqual(1, host.RequestFrames, "exactly one request frame was sent (no replay)");
        }

        Harness.Section("mutation: a timeout after the request was read");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenHold))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromMilliseconds(700));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "a mutation that timed out is Indeterminate, not Timeout");
            Harness.Check(result.MutationMayHaveBeenApplied, "and says the write may have landed");
            Harness.CheckEqual(1, host.RequestFrames, "still one frame");
        }

        Harness.Section("mutation: a truncated reply");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenTruncated))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "a truncated reply leaves the outcome unknown");
            Harness.Check(result.MutationMayHaveBeenApplied, "and marks the write as possibly applied");
        }

        Harness.Section("mutation: a reply that cannot be decoded");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenInvalid))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "an undecodable reply is not proof of failure");
            Harness.Check(result.MutationMayHaveBeenApplied, "and the write may have landed");
        }

        Harness.Section("mutation: the other two mutating calls classify the same way");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenClose))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var list = await client.SetImListAsync(new[] { "pinyin", "shuangpin" }, cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, list.Status, "SetImList is Indeterminate");
            Harness.Check(list.MutationMayHaveBeenApplied, "with the same flag");

            var select = await client.SelectInputMethodAsync("shuangpin", cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, select.Status,
                "SelectInputMethod is Indeterminate");
            Harness.Check(select.MutationMayHaveBeenApplied, "with the same flag");
        }

        Harness.Section("read-only: a failure keeps its own classification");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenClose))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.GetConfigAsync("fcitx://config/inputmethod/pinyin", cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Unreachable, result.Status,
                "a read that lost its reply is Unreachable");
            Harness.Check(!result.MutationMayHaveBeenApplied,
                "a read is not a mutation, so nothing is claimed about a write");
            Harness.Check(!result.IsUncertain, "and the outcome is not uncertain");
        }

        Harness.Section("read-only: a timeout stays a timeout");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenHold))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromMilliseconds(700));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.GetConfigAsync("fcitx://config/inputmethod/pinyin", cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Timeout, result.Status, "a read timeout is Timeout");
            Harness.Check(!result.MutationMayHaveBeenApplied, "and claims nothing about a write");
        }

        Harness.Section("mutation: the caller cancels after the request started");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenHold))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(30));
            using var cts = new CancellationTokenSource();
            var call = client.SetConfigAsync("fcitx://config/inputmethod/pinyin", ChangeSet(), cts.Token);
            await host.FrameReceived.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
            await cts.CancelAsync().ConfigureAwait(false);
            var result = await call.ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "cancelling mid-flight is not proof the write was rejected");
            Harness.Check(result.Cancelled, "the cancellation is still reported");
            Harness.Check(result.MutationMayHaveBeenApplied, "and the write may have landed");
            Harness.CheckEqual(1, host.RequestFrames, "no replay after the cancellation");
        }

        Harness.Section("mutation: the caller cancels before the request is sent");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReadThenHold))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(30));
            using var cts = new CancellationTokenSource();
            await cts.CancelAsync().ConfigureAwait(false);
            var threw = false;
            try
            {
                await client.SetConfigAsync("fcitx://config/inputmethod/pinyin", ChangeSet(), cts.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                threw = true;
            }

            Harness.Check(threw, "a cancellation before transmission still throws");
            Harness.CheckEqual(0, host.RequestFrames, "and nothing was sent");
        }

        Harness.Section("mutation: an authoritative reply is an authoritative reply");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyOk))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Ok, result.Status, "a success is Ok");
            Harness.Check(!result.MutationMayHaveBeenApplied, "and is not flagged as uncertain");
            Harness.CheckEqual("9", result.Value!.Values.Children["PageSize"].Scalar,
                "the reply's values are returned");
        }

        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyStatus,
                                               replyStatus: SettingsStatus.Busy))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Busy, result.Status, "a Busy refusal stays Busy");
            Harness.Check(!result.MutationMayHaveBeenApplied, "a refusal is not an uncertain outcome");
            Harness.CheckEqual("the Host refused", result.Error, "and its explanation is kept");
        }

        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyStatus,
                                               replyStatus: SettingsStatus.Indeterminate))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Indeterminate, result.Status,
                "a Host that says Indeterminate keeps saying it");
            Harness.Check(result.MutationMayHaveBeenApplied,
                "and the uncertainty is carried to the caller");
        }
    }

    private static async Task CompatibilityAsync()
    {
        Harness.Section("handshake: a newer Host is accepted");

        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyOk,
                                               api: WireLimits.SettingsApiVersion + 1))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.GetConfigAsync("fcitx://config/inputmethod/pinyin", cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.Ok, result.Status,
                "a Host one API version ahead is served, not rejected");
        }

        Harness.Section("handshake: a Host older than this client is rejected");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyOk,
                                               api: WireLimits.SettingsApiVersion - 1))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.GetConfigAsync("fcitx://config/inputmethod/pinyin", cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.VersionMismatch, result.Status,
                "an older Host cannot serve the messages this client sends");
            Harness.CheckEqual(0, host.RequestFrames,
                "and no request is attempted against it");
            Harness.Check(result.Error.Contains("requires", StringComparison.Ordinal),
                "the message names the requirement");
        }

        Harness.Section("handshake: the IPC protocol version is still exact");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyOk,
                                               protocol: WireLimits.ProtocolVersion - 1))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.GetConfigAsync("fcitx://config/inputmethod/pinyin", cts.Token)
                .ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.VersionMismatch, result.Status,
                "a different IPC protocol version is refused");
        }

        Harness.Section("handshake: an explicit Host refusal is propagated");
        await using (var host = FakeHost.Start(FakeHost.UniqueName(), FakeBehaviour.ReplyOk,
                                               helloStatus: SettingsStatus.VersionMismatch))
        {
            var client = new SettingsClient(host.Name, TimeSpan.FromSeconds(5));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            var result = await client.SetConfigAsync("fcitx://config/inputmethod/pinyin",
                ChangeSet(), cts.Token).ConfigureAwait(false);
            Harness.CheckEqual(SettingsStatus.VersionMismatch, result.Status,
                "the Host's own refusal is what the caller sees");
            Harness.Check(!result.MutationMayHaveBeenApplied,
                "and nothing was written, because the handshake came first");
            Harness.CheckEqual(0, host.RequestFrames, "no request frame was sent");
        }
    }
}
