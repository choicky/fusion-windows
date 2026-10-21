using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>What the fake Host does once it has read the client's request frame.</summary>
internal enum FakeBehaviour
{
    /// <summary>Answer with a well-formed Ok reply.</summary>
    ReplyOk,

    /// <summary>Answer with the configured status and no body.</summary>
    ReplyStatus,

    /// <summary>Answer the handshake, then close without reading the request.</summary>
    CloseAfterHello,

    /// <summary>Read the request, then close without a reply.</summary>
    ReadThenClose,

    /// <summary>Read the request, then write a partial reply and close.</summary>
    ReadThenTruncated,

    /// <summary>Read the request, then write a reply whose body cannot be decoded.</summary>
    ReadThenInvalid,

    /// <summary>Read the request, then never reply.</summary>
    ReadThenHold,

    /// <summary>Answer each request with what the test's script returns for it.</summary>
    Scripted,
}

/// <summary>
/// A pipe peer that speaks just enough of the settings protocol to exercise the client's
/// failure classification. It runs in the test process, so the client's same-user
/// same-session check still applies.
/// </summary>
internal sealed class FakeHost : IAsyncDisposable
{
    private readonly string _name;
    private readonly FakeBehaviour _behaviour;
    private readonly uint _protocol;
    private readonly uint _api;
    private readonly SettingsStatus _helloStatus;
    private readonly SettingsStatus _replyStatus;
    private readonly ulong _incarnation;
    private readonly Func<MessageType, byte[], byte[]?>? _script;
    private readonly CancellationTokenSource _stop = new();
    private readonly TaskCompletionSource _frameReceived = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Task _loop;
    private NamedPipeServerStream? _server;
    private int _requestFrames;

    private FakeHost(string name, FakeBehaviour behaviour, uint protocol, uint api,
                     SettingsStatus helloStatus, SettingsStatus replyStatus, ulong incarnation,
                     Func<MessageType, byte[], byte[]?>? script)
    {
        _name = name;
        _behaviour = behaviour;
        _protocol = protocol;
        _api = api;
        _helloStatus = helloStatus;
        _replyStatus = replyStatus;
        _incarnation = incarnation;
        _script = script;
        _server = CreateServer();
        _loop = AcceptAsync();
    }

    /// <summary>The pipe name the client should connect to.</summary>
    public string Name => _name;

    /// <summary>Completes once a request frame (not the handshake) has been read.</summary>
    public Task FrameReceived => _frameReceived.Task;

    /// <summary>How many request frames have been read, across every connection.</summary>
    public int RequestFrames => Volatile.Read(ref _requestFrames);

    public static FakeHost Start(string name, FakeBehaviour behaviour = FakeBehaviour.ReplyOk,
                                 uint protocol = WireLimits.ProtocolVersion,
                                 uint api = WireLimits.SettingsApiVersion,
                                 SettingsStatus helloStatus = SettingsStatus.Ok,
                                 SettingsStatus replyStatus = SettingsStatus.Invalid,
                                 ulong incarnation = 0x5EED_1234_ABCD_0001)
    {
        return new FakeHost(name, behaviour, protocol, api, helloStatus, replyStatus, incarnation, null);
    }

    /// <summary>
    /// A peer whose answer depends on the request, so a sequence such as "the write is uncertain,
    /// then the reread says X" can be driven. The script returns the reply body without the
    /// incarnation envelope, or null to close the connection instead of answering.
    /// </summary>
    public static FakeHost StartScripted(string name, Func<MessageType, byte[], byte[]?> script,
                                         ulong incarnation = 0x5EED_1234_ABCD_0002)
    {
        ArgumentNullException.ThrowIfNull(script);
        return new FakeHost(name, FakeBehaviour.Scripted, WireLimits.ProtocolVersion,
                            WireLimits.SettingsApiVersion, SettingsStatus.Ok, SettingsStatus.Invalid,
                            incarnation, script);
    }

    public static string UniqueName() => "fusion-test-" + Guid.NewGuid().ToString("N")[..10];

    private NamedPipeServerStream CreateServer() =>
        new(_name, PipeDirection.InOut, 4, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);

    private async Task AcceptAsync()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                var server = _server ??= CreateServer();
                await server.WaitForConnectionAsync(_stop.Token).ConfigureAwait(false);
                _server = null;
                try
                {
                    await HandleAsync(server).ConfigureAwait(false);
                }
                finally
                {
                    await server.DisposeAsync().ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Disposal ended the loop.
        }
        catch (ObjectDisposedException)
        {
            // Disposal closed the listening instance this loop was waiting on.
        }
        catch (IOException)
        {
            // A client that disappears mid-frame is one of the cases under test.
        }
    }

    private async Task HandleAsync(NamedPipeServerStream server)
    {
        if (await TryReadFrameAsync(server, MessageType.SettingsHelloRequest).ConfigureAwait(false) is null)
        {
            return;
        }

        await WriteFrameAsync(server, MessageType.SettingsHelloResponse,
            Bytes.Concat(Bytes.U32(_protocol), Bytes.U64(7), Bytes.U32(_api),
                         Bytes.U8((byte)_helloStatus), Bytes.Str(string.Empty))).ConfigureAwait(false);

        if (_behaviour == FakeBehaviour.CloseAfterHello)
        {
            return;
        }

        var request = await TryReadFrameAsync(server, expected: null).ConfigureAwait(false);
        if (request is null)
        {
            return;
        }

        Interlocked.Increment(ref _requestFrames);
        _frameReceived.TrySetResult();

        switch (_behaviour)
        {
            case FakeBehaviour.ReadThenClose:
                return;
            case FakeBehaviour.ReadThenHold:
                await Task.Delay(Timeout.Infinite, _stop.Token).ConfigureAwait(false);
                return;
            case FakeBehaviour.ReadThenTruncated:
                // A header promising more bytes than follow, then close.
                await server.WriteAsync(new byte[] { 200, 0, 0, 0, 0x0E }, _stop.Token)
                    .ConfigureAwait(false);
                await server.FlushAsync(_stop.Token).ConfigureAwait(false);
                return;
            case FakeBehaviour.ReadThenInvalid:
                // A complete frame whose body is not a decodable reply.
                await WriteFrameAsync(server, request.Value.Type switch
                {
                    MessageType.SettingsGetConfigRequest => MessageType.SettingsGetConfigResponse,
                    MessageType.SettingsSetConfigRequest => MessageType.SettingsSetConfigResponse,
                    MessageType.SettingsGetImListRequest => MessageType.SettingsGetImListResponse,
                    MessageType.SettingsSetImListRequest => MessageType.SettingsSetImListResponse,
                    MessageType.SettingsGetSelectionRequest => MessageType.SettingsGetSelectionResponse,
                    _ => MessageType.SettingsSelectInputMethodResponse,
                }, Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty), Bytes.U8(9))).ConfigureAwait(false);
                return;
            case FakeBehaviour.ReplyStatus:
                await WriteReplyAsync(server, request.Value.Type,
                    Bytes.Concat(Bytes.U8((byte)_replyStatus), Bytes.Str("the Host refused"))).ConfigureAwait(false);
                return;
            case FakeBehaviour.Scripted:
                var scripted = _script!(request.Value.Type, request.Value.Payload);
                if (scripted is null)
                {
                    // Closing without a reply is how a lost answer is produced.
                    return;
                }

                await WriteReplyAsync(server, request.Value.Type, scripted).ConfigureAwait(false);
                return;
            default:
                await WriteReplyAsync(server, request.Value.Type, SuccessBody(request.Value.Type))
                    .ConfigureAwait(false);
                return;
        }
    }

    private static byte[] SuccessBody(MessageType requestType) => requestType switch
    {
        MessageType.SettingsGetConfigRequest or MessageType.SettingsSetConfigRequest =>
            Bytes.SnapshotPayload(Bytes.Map(("PageSize", Bytes.Scalar("9")))),
        MessageType.SettingsGetImListRequest or MessageType.SettingsSetImListRequest =>
            Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty),
                         Bytes.Str("pinyin"), Bytes.Str("pinyin"),
                         Bytes.U32(1), Bytes.Str("pinyin"),
                         Bytes.U32(1), Bytes.Str("pinyin")),
        _ => Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty), Bytes.Str("pinyin"),
                          Bytes.U32(1), Bytes.Str("pinyin")),
    };

    private Task WriteReplyAsync(NamedPipeServerStream server, MessageType requestType, byte[] body)
    {
        var responseType = requestType switch
        {
            MessageType.SettingsGetConfigRequest => MessageType.SettingsGetConfigResponse,
            MessageType.SettingsSetConfigRequest => MessageType.SettingsSetConfigResponse,
            MessageType.SettingsGetImListRequest => MessageType.SettingsGetImListResponse,
            MessageType.SettingsSetImListRequest => MessageType.SettingsSetImListResponse,
            MessageType.SettingsGetSelectionRequest => MessageType.SettingsGetSelectionResponse,
            _ => MessageType.SettingsSelectInputMethodResponse,
        };
        return WriteFrameAsync(server, responseType, body);
    }

    private async Task WriteFrameAsync(NamedPipeServerStream server, MessageType type, byte[] body)
    {
        // Every response carries the responding incarnation.
        var enveloped = Bytes.Concat(Bytes.U64(_incarnation), body);
        var frame = Frame.Encode(type, enveloped);
        await server.WriteAsync(frame, _stop.Token).ConfigureAwait(false);
        await server.FlushAsync(_stop.Token).ConfigureAwait(false);
    }

    private async Task<(MessageType Type, byte[] Payload)?> TryReadFrameAsync(
        NamedPipeServerStream server, MessageType? expected)
    {
        var header = new byte[WireLimits.HeaderSize];
        if (!await TryReadExactlyAsync(server, header).ConfigureAwait(false))
        {
            return null;
        }

        if (!Frame.TryParseHeader(header, out var length, out var type))
        {
            return null;
        }

        if (expected is not null && type != expected)
        {
            return null;
        }

        var payload = new byte[length];
        if (!await TryReadExactlyAsync(server, payload).ConfigureAwait(false))
        {
            return null;
        }

        return (type, payload);
    }

    private async Task<bool> TryReadExactlyAsync(Stream stream, byte[] buffer)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(offset), _stop.Token).ConfigureAwait(false);
            if (read == 0)
            {
                return false;
            }

            offset += read;
        }

        return true;
    }

    public async ValueTask DisposeAsync()
    {
        await _stop.CancelAsync().ConfigureAwait(false);

        // Release the name as well: a check that brings a Host back on the same endpoint needs
        // this instance to stop listening, not just to stop answering.
        try
        {
            _server?.Dispose();
        }
        catch (ObjectDisposedException)
        {
            // Already closed by the accept loop.
        }

        try
        {
            await _loop.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Expected.
        }

        _stop.Dispose();
    }
}
