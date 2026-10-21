using System;
using System.Buffers.Binary;
using System.Linq;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

internal static class WireTests
{
    public static void Run()
    {
        Harness.Section("frame: header shape and bounds");

        var payload = new byte[] { 1, 2, 3 };
        var frame = Frame.Encode(MessageType.SettingsGetConfigRequest, payload);
        Harness.CheckEqual(WireLimits.HeaderSize + payload.Length, frame.Length, "frame length");
        Harness.CheckEqual(3u, BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(0, 4)),
            "length prefix is the payload length");
        Harness.CheckEqual((byte)0x0D, frame[4], "type byte follows the length");
        Harness.Check(Frame.TryParseHeader(frame, out var length, out var type), "own frame parses");
        Harness.CheckEqual(3, length, "parsed payload length");
        Harness.CheckEqual(MessageType.SettingsGetConfigRequest, type, "parsed type");

        Harness.Check(!Frame.TryParseHeader(new byte[4], out _, out _), "a 4-byte header is refused");
        var huge = new byte[WireLimits.HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(huge.AsSpan(0, 4), WireLimits.MaxMessagePayload + 1u);
        huge[4] = 0x0D;
        Harness.Check(!Frame.TryParseHeader(huge, out _, out _), "an oversize length is refused");
        var unknownType = new byte[WireLimits.HeaderSize];
        unknownType[4] = 0x7F;
        Harness.Check(!Frame.TryParseHeader(unknownType, out _, out _), "an unknown type is refused");

        Harness.Throws<ArgumentOutOfRangeException>(
            () => Frame.Encode(MessageType.SettingsGetConfigRequest, new byte[WireLimits.MaxMessagePayload + 1]),
            "encoding an oversize payload is refused");

        Harness.Section("codec: value tree round trip");
        var tree = SettingsValue.Map(
            ("scalar", SettingsValue.Text("hello")),
            ("empty", SettingsValue.Text(string.Empty)),
            ("nested", SettingsValue.Map(
                ("a", SettingsValue.Text("1")),
                ("deeper", SettingsValue.Map(("b", SettingsValue.Text("2")))))),
            ("unicode", SettingsValue.Text("你好")));
        var writer = new ProtoWriter();
        Harness.Check(WriteValue(writer, tree), "value encodes");
        var reader = new ProtoReader(writer.ToArray());
        Harness.Check(ReadValue(reader, out var decoded), "value decodes");
        Harness.Check(reader.AtEnd, "decoder consumed every byte");
        Harness.CheckEqual(tree, decoded, "value round trip is lossless");

        Harness.Section("codec: fail-closed decoders");
        var truncated = writer.ToArray().AsSpan(0, writer.Length - 2).ToArray();
        Harness.Check(!ReadValue(new ProtoReader(truncated), out _), "a truncated value is refused");
        var trailing = writer.ToArray().Concat(new byte[] { 0 }).ToArray();
        Harness.Check(!ReadValue(new ProtoReader(trailing), out _) ||
                      !new ProtoReader(trailing).AtEnd,
            "trailing bytes are visible to the caller");

        Harness.Section("codec: status and hello");
        var statusPayload = new ProtoWriter();
        statusPayload.U8((byte)SettingsStatus.Busy);
        statusPayload.Text("finish the composition");
        var statusReader = new ProtoReader(statusPayload.ToArray());
        Harness.Check(SettingsCodec.TryDecodeStatus(statusReader, out var status, out var error),
            "status decodes");
        Harness.CheckEqual(SettingsStatus.Busy, status, "status value");
        Harness.CheckEqual("finish the composition", error, "status error text");

        var badStatus = new ProtoWriter();
        badStatus.U8(99);
        badStatus.Text(string.Empty);
        Harness.Check(!SettingsCodec.TryDecodeStatus(new ProtoReader(badStatus.ToArray()), out _, out _),
            "an out-of-range status is refused");

        var hello = SettingsCodec.EncodeHello();
        Harness.CheckEqual(8, hello.Length, "hello payload is two u32 fields");
        var helloReader = new ProtoReader(hello);
        var protocolOk = helloReader.TryU32(out var protocol);
        var apiOk = helloReader.TryU32(out var api);
        Harness.Check(protocolOk && apiOk, "hello fields decode");
        Harness.CheckEqual(WireLimits.ProtocolVersion, protocol, "hello protocol version");
        Harness.CheckEqual(WireLimits.SettingsApiVersion, api, "hello api version");
    }

    /// <summary>Encodes one value exactly as the Host does, for round-trip checks.</summary>
    internal static bool WriteValue(ProtoWriter writer, SettingsValue value) =>
        WriteValue(writer, value, 0, new int[1]);

    private static bool WriteValue(ProtoWriter writer, SettingsValue value, int depth, int[] nodes)
    {
        if (depth > WireLimits.MaxSettingsDepth || ++nodes[0] > WireLimits.MaxSettingsNodes)
        {
            return false;
        }

        writer.U8(value.IsMap ? (byte)1 : (byte)0);
        if (!value.IsMap)
        {
            return writer.Text(value.Scalar);
        }

        writer.U32((uint)value.Children.Count);
        foreach (var (key, child) in value.Children)
        {
            if (!writer.Text(key) || !WriteValue(writer, child, depth + 1, nodes))
            {
                return false;
            }
        }

        return true;
    }

    internal static bool ReadValue(ProtoReader reader, out SettingsValue value) =>
        ReadValue(reader, 0, new int[1], out value);

    private static bool ReadValue(ProtoReader reader, int depth, int[] nodes, out SettingsValue value)
    {
        value = SettingsValue.EmptyMap();
        if (depth > WireLimits.MaxSettingsDepth || ++nodes[0] > WireLimits.MaxSettingsNodes)
        {
            return false;
        }

        if (!reader.TryU8(out var tag) || tag > 1)
        {
            return false;
        }

        if (tag == 0)
        {
            if (!reader.TryString(out var scalar))
            {
                return false;
            }

            value = SettingsValue.Text(scalar);
            return true;
        }

        if (!reader.TryU32(out var count) || count > WireLimits.MaxSettingsMapEntries)
        {
            return false;
        }

        var entries = new (string, SettingsValue)[count];
        for (var i = 0; i < count; i++)
        {
            if (!reader.TryString(out var key) || !ReadValue(reader, depth + 1, nodes, out var child))
            {
                return false;
            }

            entries[i] = (key, child);
        }

        value = SettingsValue.Map(entries);
        return true;
    }
}
