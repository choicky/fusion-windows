using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;

namespace Fusion.Settings.Ipc;

/// <summary>Wire message types, mirroring <c>fusion::ipc::MessageType</c>.</summary>
public enum MessageType : byte
{
    HelloRequest = 0x01,
    HelloResponse = 0x02,
    SettingsHelloRequest = 0x0B,
    SettingsHelloResponse = 0x0C,
    SettingsGetConfigRequest = 0x0D,
    SettingsGetConfigResponse = 0x0E,
    SettingsSetConfigRequest = 0x0F,
    SettingsSetConfigResponse = 0x10,
    SettingsGetSelectionRequest = 0x11,
    SettingsGetSelectionResponse = 0x12,
    SettingsSelectInputMethodRequest = 0x13,
    SettingsSelectInputMethodResponse = 0x14,
    SettingsGetImListRequest = 0x15,
    SettingsGetImListResponse = 0x16,
    SettingsSetImListRequest = 0x17,
    SettingsSetImListResponse = 0x18,
}

/// <summary>
/// Bounds shared with the Host. They are the client's own defence too: a bound peer
/// must not be able to drive unbounded allocation, and every reader fails closed.
/// </summary>
public static class WireLimits
{
    // Must match `fusion::ipc::protocol_version`. Both ends check it exactly and refuse a
    // mismatch with a version error, so this is a compatibility constant, not a hint: a
    // client that guessed would be served nothing at all.
    public const uint ProtocolVersion = 8;
    public const uint SettingsApiVersion = 2;
    public const uint SettingsApiMinVersion = 1;

    public const int HeaderSize = 5;
    public const int MaxMessagePayload = 65536;
    public const int MaxStringLength = 8192;
    public const int MaxVectorLength = 64;

    public const int MaxSettingsPayload = 60 * 1024;
    public const int MaxSettingsNodes = 4096;
    public const int MaxSettingsMapEntries = 512;
    public const int MaxSettingsTypes = 64;
    public const int MaxSettingsOptions = 512;
    public const int MaxSettingsDepth = 16;
}

/// <summary>Little-endian writer with the same bounds as the Host encoder.</summary>
public sealed class ProtoWriter
{
    private readonly List<byte> _buffer = new(256);

    public int Length => _buffer.Count;

    public byte[] ToArray() => _buffer.ToArray();

    public void U8(byte value) => _buffer.Add(value);

    public void U32(uint value)
    {
        Span<byte> scratch = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(scratch, value);
        _buffer.AddRange(scratch.ToArray());
    }

    public void U64(ulong value)
    {
        Span<byte> scratch = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(scratch, value);
        _buffer.AddRange(scratch.ToArray());
    }

    /// <summary>Writes a length-prefixed UTF-8 string; refuses one over the wire bound.</summary>
    public bool Text(string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length > WireLimits.MaxStringLength)
        {
            return false;
        }

        U32((uint)bytes.Length);
        _buffer.AddRange(bytes);
        return true;
    }
}

/// <summary>Little-endian reader that fails closed on a short buffer or trailing bytes.</summary>
public sealed class ProtoReader
{
    private readonly byte[] _data;
    private int _position;

    public ProtoReader(byte[] data)
    {
        _data = data;
    }

    public int Position => _position;

    public bool AtEnd => _position == _data.Length;

    public bool TryU8(out byte value)
    {
        if (_position + 1 > _data.Length)
        {
            value = 0;
            return false;
        }

        value = _data[_position++];
        return true;
    }

    public bool TryU32(out uint value)
    {
        if (_position + 4 > _data.Length)
        {
            value = 0;
            return false;
        }

        value = BinaryPrimitives.ReadUInt32LittleEndian(_data.AsSpan(_position, 4));
        _position += 4;
        return true;
    }

    public bool TryU64(out ulong value)
    {
        if (_position + 8 > _data.Length)
        {
            value = 0;
            return false;
        }

        value = BinaryPrimitives.ReadUInt64LittleEndian(_data.AsSpan(_position, 8));
        _position += 8;
        return true;
    }

    public bool TryString(out string value)
    {
        value = string.Empty;
        if (!TryU32(out var length) || length > WireLimits.MaxStringLength)
        {
            return false;
        }

        if (_position + (int)length > _data.Length)
        {
            return false;
        }

        value = Encoding.UTF8.GetString(_data, _position, (int)length);
        _position += (int)length;
        return true;
    }
}

/// <summary>
/// The 5-byte frame header: <c>[u32 payload length][u8 type][payload]</c>. Responses
/// carry an 8-byte Host incarnation before their own payload (D072 §13), so a result
/// is never accepted as belonging to a different Host incarnation than the connection.
/// </summary>
public static class Frame
{
    public static byte[] Encode(MessageType type, byte[] payload)
    {
        ArgumentNullException.ThrowIfNull(payload);
        if (payload.Length > WireLimits.MaxMessagePayload)
        {
            throw new ArgumentOutOfRangeException(nameof(payload), "payload exceeds the wire bound");
        }

        var frame = new byte[WireLimits.HeaderSize + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(0, 4), (uint)payload.Length);
        frame[4] = (byte)type;
        payload.CopyTo(frame, WireLimits.HeaderSize);
        return frame;
    }

    public static bool TryParseHeader(ReadOnlySpan<byte> header, out int payloadLength, out MessageType type)
    {
        payloadLength = 0;
        type = default;
        if (header.Length < WireLimits.HeaderSize)
        {
            return false;
        }

        var length = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(0, 4));
        if (length > WireLimits.MaxMessagePayload)
        {
            return false;
        }

        var typeByte = header[4];
        if (!Enum.IsDefined(typeof(MessageType), typeByte))
        {
            return false;
        }

        payloadLength = (int)length;
        type = (MessageType)typeByte;
        return true;
    }
}
