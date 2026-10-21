using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;

namespace Fusion.Settings.Client;

/// <summary>
/// A settings conversation with one Host.
/// </summary>
/// <remarks>
/// <para>
/// The Host serves exactly one settings request per connection, so every call opens
/// its own connection, performs the handshake and then issues that one request. That
/// is a property of the protocol, not a limitation of this client: it means a reply
/// can never be matched to the wrong request.
/// </para>
/// <para>
/// <b>Mutation outcomes.</b> A read that fails simply failed. A write is different: once
/// the request frame has started down the pipe, the Host may apply it even if the reply is
/// lost, truncated, late or interrupted by the caller. Those cases return
/// <see cref="SettingsStatus.Indeterminate"/> with
/// <see cref="SettingsResult{T}.MutationMayHaveBeenApplied"/> set, which is the honest
/// label — the caller rereads and reconciles, and never replays. A failure before the frame
/// starts is a plain <see cref="SettingsStatus.Unreachable"/>: nothing was written.
/// </para>
/// <para>
/// <b>API compatibility.</b> The settings protocol is additive: a newer Host must keep the
/// messages that already existed. This client therefore requires a Host whose API is at
/// least its own and accepts a newer one, because a newer Host can still serve it. The
/// Host's own minimum accepted client version is the Host's business, not a floor for this
/// client.
/// </para>
/// </remarks>
public sealed class SettingsClient
{
    private readonly TimeSpan _timeout;

    public SettingsClient(string endpoint, TimeSpan? timeout = null)
    {
        var normalized = PipeEndpoint.Normalize(endpoint);
        if (normalized is null)
        {
            throw new ArgumentException(
                "The endpoint is not a single pipe name; refusing to fall back to the production pipe.",
                nameof(endpoint));
        }

        Endpoint = normalized;
        _timeout = timeout ?? TimeSpan.FromSeconds(5);
    }

    public string Endpoint { get; }

    public bool IsProductionEndpoint => PipeEndpoint.IsProduction(Endpoint);

    /// <summary>The settings API version this client requires the Host to speak at least.</summary>
    public static uint RequiredApiVersion => WireLimits.SettingsApiVersion;

    public Task<SettingsResult<SettingsSnapshot>> GetConfigAsync(string uri, CancellationToken cancellationToken) =>
        RunAsync<SettingsSnapshot>(MessageType.SettingsGetConfigRequest, SettingsCodec.EncodeGetConfig(uri),
                                   MessageType.SettingsGetConfigResponse, SettingsCodec.TryDecodeSnapshot,
                                   isMutation: false, cancellationToken);

    public Task<SettingsResult<SettingsSnapshot>> SetConfigAsync(string uri, SettingsValue values,
                                                                 CancellationToken cancellationToken) =>
        RunAsync<SettingsSnapshot>(MessageType.SettingsSetConfigRequest, SettingsCodec.EncodeSetConfig(uri, values),
                                   MessageType.SettingsSetConfigResponse, SettingsCodec.TryDecodeSnapshot,
                                   isMutation: true, cancellationToken);

    public Task<SettingsResult<ImList>> GetImListAsync(CancellationToken cancellationToken) =>
        RunAsync<ImList>(MessageType.SettingsGetImListRequest, Array.Empty<byte>(),
                         MessageType.SettingsGetImListResponse, SettingsCodec.TryDecodeImList,
                         isMutation: false, cancellationToken);

    public Task<SettingsResult<ImList>> SetImListAsync(IReadOnlyList<string> enabled,
                                                       CancellationToken cancellationToken) =>
        RunAsync<ImList>(MessageType.SettingsSetImListRequest, SettingsCodec.EncodeStringList(enabled),
                         MessageType.SettingsSetImListResponse, SettingsCodec.TryDecodeImList,
                         isMutation: true, cancellationToken);

    public Task<SettingsResult<SettingsSelection>> GetSelectionAsync(CancellationToken cancellationToken) =>
        RunAsync<SettingsSelection>(MessageType.SettingsGetSelectionRequest, Array.Empty<byte>(),
                                    MessageType.SettingsGetSelectionResponse,
                                    SettingsCodec.TryDecodeSelection, isMutation: false, cancellationToken);

    public Task<SettingsResult<SettingsSelection>> SelectInputMethodAsync(string method,
                                                                          CancellationToken cancellationToken) =>
        RunAsync<SettingsSelection>(MessageType.SettingsSelectInputMethodRequest,
                                    SettingsCodec.EncodeSelectInputMethod(method),
                                    MessageType.SettingsSelectInputMethodResponse,
                                    SettingsCodec.TryDecodeSelection, isMutation: true, cancellationToken);

    private delegate bool Decoder<T>(ProtoReader reader, out T value);

    private async Task<SettingsResult<T>> RunAsync<T>(
        MessageType requestType,
        byte[] requestPayload,
        MessageType expectedResponse,
        Decoder<T> decode,
        bool isMutation,
        CancellationToken cancellationToken)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(_timeout);
        var transmission = new TransmissionState();
        var phase = Phase.Connecting;

        try
        {
            await using var connection = await Connection.OpenAsync(Endpoint, deadline.Token)
                .ConfigureAwait(false);
            phase = Phase.Handshake;

            var hello = await connection.HandshakeAsync(deadline.Token).ConfigureAwait(false);
            if (hello.Status != SettingsStatus.Ok)
            {
                // The handshake precedes every request, so nothing was written.
                return SettingsResult.Failure<T>(hello.Status, hello.Error, hello.Incarnation);
            }

            phase = Phase.Request;
            var reply = await connection.RequestAsync(requestType, requestPayload, expectedResponse,
                                                      transmission, deadline.Token).ConfigureAwait(false);
            if (reply is null)
            {
                return Failed<T>(phase, isMutation, transmission, timedOut: false, connection.Incarnation,
                    "The Host closed the connection without a well-formed reply.");
            }

            var (payload, incarnation) = reply.Value;
            if (incarnation != connection.Incarnation)
            {
                // A reply stamped with another incarnation belongs to a different Host
                // process; accepting it would attribute another Host's state to this one.
                return Failed<T>(phase, isMutation, transmission, timedOut: false, connection.Incarnation,
                    "The reply came from a different Host incarnation.");
            }

            var reader = new ProtoReader(payload);
            if (!SettingsCodec.TryDecodeStatus(reader, out var status, out var error))
            {
                return Failed<T>(phase, isMutation, transmission, timedOut: false, incarnation,
                    "The Host reply could not be decoded.");
            }

            if (status != SettingsStatus.Ok)
            {
                // An authoritative refusal. A Host that answers Indeterminate has already
                // said the outcome is unknown; keep that rather than downgrading it.
                return SettingsResult.Failure<T>(status, error, incarnation,
                    mutationMayHaveBeenApplied: status == SettingsStatus.Indeterminate);
            }

            if (!decode(reader, out var value))
            {
                // The Host said Ok, so a mutation did happen; what failed is reading the
                // resulting state, which is exactly the Indeterminate case.
                return Failed<T>(phase, isMutation, transmission, timedOut: false, incarnation,
                    "The Host accepted the request but its reply could not be read; reread before retrying.",
                    authoritativeOk: true);
            }

            return SettingsResult.Ok(value, incarnation);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // The caller asked to stop. Before the mutation starts that is a clean
            // cancellation; after it starts the write may land, so the result says so
            // instead of throwing the uncertainty away.
            if (isMutation && transmission.Started)
            {
                return Failed<T>(phase, isMutation, transmission, timedOut: false, incarnation: 0,
                    "The request was cancelled after it had started; the Host may have applied it.",
                    cancelled: true);
            }

            throw;
        }
        catch (OperationCanceledException)
        {
            return Failed<T>(phase, isMutation, transmission, timedOut: true, incarnation: 0,
                "The Host did not answer within the request bound.");
        }
        catch (TimeoutException)
        {
            // A named-pipe connect to a pipe that nobody listens on waits for the bound and
            // then reports a timeout, so a timeout during the connect phase means "no Host",
            // not "a slow Host". The phase is what separates the two.
            return Failed<T>(phase, isMutation, transmission, timedOut: true, incarnation: 0,
                phase == Phase.Connecting
                    ? "No Host is listening on this endpoint."
                    : "The Host did not answer within the request bound.");
        }
        catch (IOException exception)
        {
            return Failed<T>(phase, isMutation, transmission, timedOut: false, incarnation: 0,
                exception.Message);
        }
        catch (UnauthorizedAccessException exception)
        {
            // Raised by the server identity check, before any request frame is written.
            return SettingsResult.Failure<T>(SettingsStatus.Unreachable, exception.Message);
        }
    }

    /// <summary>
    /// Classifies a failed attempt. For a mutation whose frame had already started, the
    /// outcome is unknown; before that the failure is definite and keeps its own status.
    /// </summary>
    private static SettingsResult<T> Failed<T>(Phase phase, bool isMutation,
                                               TransmissionState transmission, bool timedOut,
                                               ulong incarnation, string error,
                                               bool authoritativeOk = false, bool cancelled = false)
    {
        if (isMutation && (transmission.Started || authoritativeOk))
        {
            return SettingsResult.Failure<T>(SettingsStatus.Indeterminate, error, incarnation,
                mutationMayHaveBeenApplied: true, cancelled: cancelled);
        }

        // Nothing was written, so the status describes reaching the Host, not a write.
        var status = timedOut && phase != Phase.Connecting
            ? SettingsStatus.Timeout
            : SettingsStatus.Unreachable;
        return SettingsResult.Failure<T>(status, error, incarnation, cancelled: cancelled);
    }

    /// <summary>How far the request got, which decides what a failure can mean.</summary>
    private enum Phase
    {
        Connecting,
        Handshake,
        Request,
    }

    /// <summary>Whether the request frame has begun travelling to the Host.</summary>
    private sealed class TransmissionState
    {
        public bool Started { get; set; }
    }

    /// <summary>One connection: handshake, one request, then close.</summary>
    private sealed class Connection : IAsyncDisposable
    {
        private readonly NamedPipeClientStream _pipe;

        private Connection(NamedPipeClientStream pipe)
        {
            _pipe = pipe;
        }

        public ulong Incarnation { get; private set; }

        public static async Task<Connection> OpenAsync(string logicalName, CancellationToken cancellationToken)
        {
            // Asynchronous mode keeps the UI thread free: the pipe read is a real
            // overlapped operation, not a blocking call on a worker thread.
            var pipe = new NamedPipeClientStream(".", logicalName, PipeDirection.InOut,
                                                 PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(cancellationToken).ConfigureAwait(false);
            }
            catch
            {
                await pipe.DisposeAsync().ConfigureAwait(false);
                throw;
            }

            var connection = new Connection(pipe);
            if (!connection.ServerIsSameUserAndSession())
            {
                await connection.DisposeAsync().ConfigureAwait(false);
                throw new UnauthorizedAccessException(
                    "The pipe server is not this user in this session; refusing to talk to it.");
            }

            return connection;
        }

        /// <summary>
        /// Mirrors <c>verify_server_identity</c>: the server must be the same user and the
        /// same session. An unreadable identity is a refusal, not a pass.
        /// </summary>
        private bool ServerIsSameUserAndSession()
        {
            var serverPid = Native.ServerProcessId(_pipe.SafePipeHandle.DangerousGetHandle());
            if (serverPid is null)
            {
                return false;
            }

            var serverSid = Native.ProcessUserSidString(serverPid.Value);
            var serverSession = Native.SessionIdOf(serverPid.Value);
            var ownSid = Native.CurrentUserSidString();
            var ownSession = Native.CurrentSessionId();
            return serverSid is not null && ownSid is not null &&
                   serverSession is not null && ownSession is not null &&
                   string.Equals(serverSid, ownSid, StringComparison.OrdinalIgnoreCase) &&
                   serverSession.Value == ownSession.Value;
        }

        public async Task<HelloResult> HandshakeAsync(CancellationToken cancellationToken)
        {
            await SendAsync(MessageType.SettingsHelloRequest, SettingsCodec.EncodeHello(), null,
                            cancellationToken).ConfigureAwait(false);
            var reply = await ReceiveAsync(MessageType.SettingsHelloResponse, cancellationToken)
                .ConfigureAwait(false);
            if (reply is null)
            {
                return new HelloResult(SettingsStatus.Unreachable,
                    "The Host closed the connection during the handshake.", 0);
            }

            var (payload, incarnation) = reply.Value;
            Incarnation = incarnation;
            var reader = new ProtoReader(payload);
            if (!SettingsCodec.TryDecodeHello(reader, out var protocol, out _, out var api,
                                              out var status, out var error))
            {
                return new HelloResult(SettingsStatus.Unreachable,
                    "The Host handshake reply could not be decoded.", incarnation);
            }

            if (protocol != WireLimits.ProtocolVersion)
            {
                return new HelloResult(SettingsStatus.VersionMismatch,
                    error.Length > 0 ? error : "The Host IPC protocol version is not supported.",
                    incarnation);
            }

            // Additive contract: this client needs a Host at least as new as itself, and a
            // newer Host stays usable because it must keep the older messages intact. The
            // Host's minimum accepted client version is not a ceiling for this client.
            if (api < RequiredApiVersion)
            {
                return new HelloResult(SettingsStatus.VersionMismatch,
                    error.Length > 0
                        ? error
                        : $"The Host settings API is version {api}; this client requires " +
                          $"{RequiredApiVersion} or newer.",
                    incarnation);
            }

            return new HelloResult(status, error, incarnation);
        }

        public async Task<(byte[] Payload, ulong Incarnation)?> RequestAsync(
            MessageType requestType, byte[] payload, MessageType expectedResponse,
            TransmissionState transmission, CancellationToken cancellationToken)
        {
            await SendAsync(requestType, payload, transmission, cancellationToken).ConfigureAwait(false);
            return await ReceiveAsync(expectedResponse, cancellationToken).ConfigureAwait(false);
        }

        private async Task SendAsync(MessageType type, byte[] payload, TransmissionState? transmission,
                                     CancellationToken cancellationToken)
        {
            var frame = Frame.Encode(type, payload);
            // Marked before the first byte goes out: a write that fails part-way may still
            // have delivered a complete frame if the failure was on the acknowledgement.
            if (transmission is not null)
            {
                transmission.Started = true;
            }

            await _pipe.WriteAsync(frame, cancellationToken).ConfigureAwait(false);
            await _pipe.FlushAsync(cancellationToken).ConfigureAwait(false);
        }

        private async Task<(byte[] Payload, ulong Incarnation)?> ReceiveAsync(
            MessageType expected, CancellationToken cancellationToken)
        {
            var header = new byte[WireLimits.HeaderSize];
            if (!await TryReadExactlyAsync(header, cancellationToken).ConfigureAwait(false))
            {
                return null;
            }

            if (!Frame.TryParseHeader(header, out var length, out var type) || type != expected)
            {
                return null;
            }

            var body = new byte[length];
            if (!await TryReadExactlyAsync(body, cancellationToken).ConfigureAwait(false))
            {
                return null;
            }

            // Every response starts with the responding Host's incarnation.
            if (body.Length < 8)
            {
                return null;
            }

            var incarnation = BinaryPrimitives.ReadUInt64LittleEndian(body.AsSpan(0, 8));
            return (body[8..], incarnation);
        }

        private async Task<bool> TryReadExactlyAsync(byte[] buffer, CancellationToken cancellationToken)
        {
            var offset = 0;
            while (offset < buffer.Length)
            {
                var read = await _pipe.ReadAsync(buffer.AsMemory(offset), cancellationToken)
                    .ConfigureAwait(false);
                if (read == 0)
                {
                    return false;
                }

                offset += read;
            }

            return true;
        }

        public ValueTask DisposeAsync() => _pipe.DisposeAsync();
    }

    private readonly record struct HelloResult(SettingsStatus Status, string Error, ulong Incarnation);
}
