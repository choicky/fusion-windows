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
/// The handshake negotiates both the IPC protocol version and the settings API version.
/// An older client is served and a newer one is refused, so this client requires a Host
/// whose API is at least <see cref="WireLimits.SettingsApiMinVersion"/> and refuses to
/// guess when it is not.
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

    public Task<SettingsResult<SettingsSnapshot>> GetConfigAsync(string uri, CancellationToken cancellationToken) =>
        RunAsync<SettingsSnapshot>(MessageType.SettingsGetConfigRequest, SettingsCodec.EncodeGetConfig(uri),
                 MessageType.SettingsGetConfigResponse, SettingsCodec.TryDecodeSnapshot, cancellationToken);

    public Task<SettingsResult<SettingsSnapshot>> SetConfigAsync(string uri, SettingsValue values,
                                                                 CancellationToken cancellationToken) =>
        RunAsync<SettingsSnapshot>(MessageType.SettingsSetConfigRequest, SettingsCodec.EncodeSetConfig(uri, values),
                 MessageType.SettingsSetConfigResponse, SettingsCodec.TryDecodeSnapshot, cancellationToken);

    public Task<SettingsResult<ImList>> GetImListAsync(CancellationToken cancellationToken) =>
        RunAsync<ImList>(MessageType.SettingsGetImListRequest, Array.Empty<byte>(),
                 MessageType.SettingsGetImListResponse, SettingsCodec.TryDecodeImList, cancellationToken);

    public Task<SettingsResult<ImList>> SetImListAsync(IReadOnlyList<string> enabled,
                                                       CancellationToken cancellationToken) =>
        RunAsync<ImList>(MessageType.SettingsSetImListRequest, SettingsCodec.EncodeStringList(enabled),
                 MessageType.SettingsSetImListResponse, SettingsCodec.TryDecodeImList, cancellationToken);

    public Task<SettingsResult<SettingsSelection>> GetSelectionAsync(CancellationToken cancellationToken) =>
        RunAsync<SettingsSelection>(MessageType.SettingsGetSelectionRequest, Array.Empty<byte>(),
                 MessageType.SettingsGetSelectionResponse, SettingsCodec.TryDecodeSelection, cancellationToken);

    public Task<SettingsResult<SettingsSelection>> SelectInputMethodAsync(string method,
                                                                          CancellationToken cancellationToken) =>
        RunAsync<SettingsSelection>(MessageType.SettingsSelectInputMethodRequest,
                 SettingsCodec.EncodeSelectInputMethod(method),
                 MessageType.SettingsSelectInputMethodResponse, SettingsCodec.TryDecodeSelection,
                 cancellationToken);

    private delegate bool Decoder<T>(ProtoReader reader, out T value);

    private async Task<SettingsResult<T>> RunAsync<T>(
        MessageType requestType,
        byte[] requestPayload,
        MessageType expectedResponse,
        Decoder<T> decode,
        CancellationToken cancellationToken)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(_timeout);

        try
        {
            await using var connection = await Connection.OpenAsync(Endpoint, deadline.Token)
                .ConfigureAwait(false);

            var hello = await connection.HandshakeAsync(deadline.Token).ConfigureAwait(false);
            if (hello.Status != SettingsStatus.Ok)
            {
                return SettingsResult.Failure<T>(hello.Status, hello.Error, hello.Incarnation);
            }

            var reply = await connection.RequestAsync(requestType, requestPayload, expectedResponse,
                                                      deadline.Token).ConfigureAwait(false);
            if (reply is null)
            {
                return SettingsResult.Failure<T>(SettingsStatus.Unreachable,
                    "The Host closed the connection without a well-formed reply.",
                    connection.Incarnation);
            }

            var (payload, incarnation) = reply.Value;
            if (incarnation != connection.Incarnation)
            {
                // A reply stamped with another incarnation belongs to a different Host
                // process; accepting it would attribute another Host's state to this one.
                return SettingsResult.Failure<T>(SettingsStatus.Unreachable,
                    "The reply came from a different Host incarnation.", connection.Incarnation);
            }

            var reader = new ProtoReader(payload);
            if (!SettingsCodec.TryDecodeStatus(reader, out var status, out var error))
            {
                return SettingsResult.Failure<T>(SettingsStatus.Unreachable,
                    "The Host reply could not be decoded.", incarnation);
            }

            if (status != SettingsStatus.Ok)
            {
                return SettingsResult.Failure<T>(status, error, incarnation);
            }

            if (!decode(reader, out var value))
            {
                return SettingsResult.Failure<T>(SettingsStatus.Unreachable,
                    "The Host reply could not be decoded.", incarnation);
            }

            return SettingsResult.Ok(value, incarnation);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            return SettingsResult.Failure<T>(SettingsStatus.Timeout,
                "The Host did not answer within the request bound.");
        }
        catch (TimeoutException)
        {
            return SettingsResult.Failure<T>(SettingsStatus.Timeout,
                "The Host did not answer within the request bound.");
        }
        catch (IOException exception)
        {
            return SettingsResult.Failure<T>(SettingsStatus.Unreachable, exception.Message);
        }
        catch (UnauthorizedAccessException exception)
        {
            return SettingsResult.Failure<T>(SettingsStatus.Unreachable, exception.Message);
        }
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
            await SendAsync(MessageType.SettingsHelloRequest, SettingsCodec.EncodeHello(), cancellationToken)
                .ConfigureAwait(false);
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

            if (api < WireLimits.SettingsApiMinVersion || api > WireLimits.SettingsApiVersion)
            {
                return new HelloResult(SettingsStatus.VersionMismatch,
                    error.Length > 0 ? error : "The Host settings API version is not supported.",
                    incarnation);
            }

            return new HelloResult(status, error, incarnation);
        }

        public async Task<(byte[] Payload, ulong Incarnation)?> RequestAsync(
            MessageType requestType, byte[] payload, MessageType expectedResponse,
            CancellationToken cancellationToken)
        {
            await SendAsync(requestType, payload, cancellationToken).ConfigureAwait(false);
            return await ReceiveAsync(expectedResponse, cancellationToken).ConfigureAwait(false);
        }

        private async Task SendAsync(MessageType type, byte[] payload, CancellationToken cancellationToken)
        {
            var frame = Frame.Encode(type, payload);
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
