using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Fusion.Settings.Ipc;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// Byte fixtures built by hand, so the production codec is verified against an independent
/// encoding rather than against a second copy of itself.
/// </summary>
internal static class Bytes
{
    public static byte[] U8(byte value) => new[] { value };

    public static byte[] U32(uint value)
    {
        var buffer = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(buffer, value);
        return buffer;
    }

    public static byte[] U64(ulong value)
    {
        var buffer = new byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(buffer, value);
        return buffer;
    }

    public static byte[] Str(string value)
    {
        var body = Encoding.UTF8.GetBytes(value);
        return Concat(U32((uint)body.Length), body);
    }

    /// <summary>A value tag 0 followed by its text.</summary>
    public static byte[] Scalar(string value) => Concat(U8(0), Str(value));

    /// <summary>A value tag 1, the entry count, then each key and child value.</summary>
    public static byte[] Map(params (string Key, byte[] Value)[] entries) =>
        Concat(new[] { Concat(U8(1), U32((uint)entries.Length)) }
            .Concat(entries.SelectMany(entry => new[] { Str(entry.Key), entry.Value }))
            .ToArray());

    /// <summary>A map whose entries are numbered "0".."n-1", the shape a key list uses.</summary>
    public static byte[] IndexedMap(params string[] values) =>
        Map(values.Select((value, index) =>
                (index.ToString(System.Globalization.CultureInfo.InvariantCulture), Scalar(value)))
            .ToArray());

    public static byte[] Concat(params byte[][] parts)
    {
        var length = parts.Sum(part => part.Length);
        var buffer = new byte[length];
        var offset = 0;
        foreach (var part in parts)
        {
            Buffer.BlockCopy(part, 0, buffer, offset, part.Length);
            offset += part.Length;
        }

        return buffer;
    }

    /// <summary>A complete snapshot payload: status, error, values, then the schema types.</summary>
    public static byte[] SnapshotPayload(byte[] values, params (string Name, byte[][] Options)[] types)
    {
        var parts = new List<byte[]> { U8(0), Str(string.Empty), values, U32((uint)types.Length) };
        foreach (var (name, options) in types)
        {
            parts.Add(Str(name));
            parts.Add(U32((uint)options.Length));
            parts.AddRange(options);
        }

        return Concat(parts.ToArray());
    }

    /// <summary>One option description: name, type, description, default, properties.</summary>
    public static byte[] Option(string name, string type, byte[] defaultValue, byte[] properties) =>
        Concat(Str(name), Str(type), Str(name + " description"), defaultValue, properties);

    public static byte[] EmptyProperties() => Map();

    /// <summary>Builds a tree of <paramref name="maps"/> maps with <paramref name="leaves"/> leaves each.</summary>
    public static byte[] WideTree(int maps, int leaves)
    {
        var inner = Map(Enumerable.Range(0, leaves)
            .Select(index => ("k" + index, Scalar("v" + index)))
            .ToArray());
        return Map(Enumerable.Range(0, maps)
            .Select(index => ("m" + index, inner))
            .ToArray());
    }

    /// <summary>Builds a tree nested <paramref name="depth"/> maps deep with one leaf at the bottom.</summary>
    public static byte[] DeepTree(int depth)
    {
        var current = Scalar("leaf");
        for (var i = 0; i < depth; i++)
        {
            current = Map(("n" + i, current));
        }

        return current;
    }
}

internal static class WireTests
{
    /// <summary>
    /// Decodes a complete reply payload the way the client does: the status prefix first,
    /// then the snapshot body. Both are production entry points.
    /// </summary>
    private static bool TryDecodeSnapshot(byte[] payload, out SettingsSnapshot snapshot)
    {
        var reader = new ProtoReader(payload);
        if (!SettingsCodec.TryDecodeStatus(reader, out _, out _))
        {
            snapshot = SettingsSnapshot.Empty;
            return false;
        }

        return SettingsCodec.TryDecodeSnapshot(reader, out snapshot);
    }

    public static void Run()
    {
        Framing();
        SnapshotDecoding();
        Budgets();
        Malformed();
        RequestEncoding();
        OtherReplies();
    }

    private static void Framing()
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
            () => Frame.Encode(MessageType.SettingsGetConfigRequest,
                               new byte[WireLimits.MaxMessagePayload + 1]),
            "encoding an oversize payload is refused");
    }

    private static void SnapshotDecoding()
    {
        Harness.Section("codec: snapshot decoding through the production entry point");

        var values = Bytes.Map(
            ("scalar", Bytes.Scalar("hello")),
            ("empty", Bytes.Scalar(string.Empty)),
            ("unicode", Bytes.Scalar("你好")),
            ("nested", Bytes.Map(("a", Bytes.Scalar("1")), ("deeper", Bytes.Map(("b", Bytes.Scalar("2")))))),
            ("PrevPage", Bytes.IndexedMap("minus", "Up", "KP_Up", "Page_Up")));

        var option = Bytes.Option("PageSize", "Integer", Bytes.Scalar("7"),
                                  Bytes.Map(("IntMin", Bytes.Scalar("3")), ("IntMax", Bytes.Scalar("10"))));
        var payload = Bytes.SnapshotPayload(values, ("PinyinEngineConfig", new[] { option }));

        var reader = new ProtoReader(payload);
        Harness.Check(SettingsCodec.TryDecodeStatus(reader, out var status, out var error),
            "status prefix decodes");
        Harness.CheckEqual(SettingsStatus.Ok, status, "status is Ok");
        Harness.CheckEqual(string.Empty, error, "error text is empty");
        Harness.Check(SettingsCodec.TryDecodeSnapshot(reader, out var snapshot), "snapshot decodes");
        Harness.Check(reader.AtEnd, "the decoder consumed every byte");

        Harness.CheckEqual("hello", snapshot.Values.Children["scalar"].Scalar, "a scalar survives");
        Harness.CheckEqual(string.Empty, snapshot.Values.Children["empty"].Scalar,
            "an empty scalar is not confused with a missing key");
        Harness.CheckEqual("你好", snapshot.Values.Children["unicode"].Scalar, "UTF-8 survives");
        Harness.CheckEqual("2",
            snapshot.Values.Children["nested"].Children["deeper"].Children["b"].Scalar,
            "nesting survives");

        var keyList = KeyBindingList.FromValue(snapshot.Values.Children["PrevPage"]);
        Harness.CheckEqual(4, keyList.Count, "the indexed key list decodes as four bindings");
        Harness.CheckEqual("Page_Up", keyList[3].ToString(), "in index order");

        Harness.CheckEqual(1, snapshot.Types.Count, "one type decoded");
        var decoded = snapshot.FindOption("PageSize");
        Harness.Check(decoded is not null, "the option is described");
        Harness.CheckEqual(3, decoded!.IntMinimum, "the constraint survives");
        Harness.CheckEqual(10, decoded.IntMaximum, "both bounds survive");
    }

    private static void Budgets()
    {
        Harness.Section("codec: node budget counts each node once");

        // 8 maps x 400 leaves = 3 209 nodes: legal, and the case the former double count
        // rejected (it charged every child twice, reaching roughly 9 600).
        var wide = Bytes.WideTree(maps: 8, leaves: 400);
        var legal = Bytes.SnapshotPayload(wide);
        Harness.Check(TryDecodeSnapshot(legal, out var wideSnapshot),
            "a legal 3 209-node tree decodes");
        Harness.CheckEqual(8, wideSnapshot.Values.Children.Count, "every map is present");
        Harness.CheckEqual(400, wideSnapshot.Values.Children["m0"].Children.Count,
            "every leaf is present");

        // 9 maps x 500 leaves = 4 510 nodes: over the 4 096 ceiling.
        Harness.Check(WireLimits.MaxSettingsNodes < 1 + 9 + (9 * 500),
            "the over-budget fixture really exceeds the ceiling");
        var over = Bytes.SnapshotPayload(Bytes.WideTree(maps: 9, leaves: 500));
        Harness.Check(!TryDecodeSnapshot(over, out _),
            "an over-budget tree is refused");

        Harness.Section("codec: depth ceiling");
        var atLimit = Bytes.SnapshotPayload(Bytes.DeepTree(WireLimits.MaxSettingsDepth));
        Harness.Check(TryDecodeSnapshot(atLimit, out _),
            "a tree at the depth limit decodes");
        var tooDeep = Bytes.SnapshotPayload(Bytes.DeepTree(WireLimits.MaxSettingsDepth + 2));
        Harness.Check(!TryDecodeSnapshot(tooDeep, out _),
            "a tree past the depth limit is refused");
    }

    private static void Malformed()
    {
        Harness.Section("codec: malformed input is refused, not partially accepted");

        var good = Bytes.SnapshotPayload(Bytes.Map(("a", Bytes.Scalar("1")), ("b", Bytes.Scalar("2"))));

        Harness.Check(!TryDecodeSnapshot(good[..^2], out _),
            "a truncated payload is refused");

        var trailing = Bytes.Concat(good, Bytes.U8(0));
        Harness.Check(!TryDecodeSnapshot(trailing, out _),
            "trailing bytes are refused");

        var duplicate = Bytes.SnapshotPayload(Bytes.Map(("a", Bytes.Scalar("1")), ("a", Bytes.Scalar("2"))));
        Harness.Check(!TryDecodeSnapshot(duplicate, out _),
            "duplicate map keys are refused");

        var badTag = Bytes.SnapshotPayload(Bytes.Concat(Bytes.U8(7), Bytes.Str("x")));
        Harness.Check(!TryDecodeSnapshot(badTag, out _),
            "an unknown value tag is refused");

        var rootScalar = Bytes.SnapshotPayload(Bytes.Scalar("not a map"));
        Harness.Check(!TryDecodeSnapshot(rootScalar, out _),
            "a non-map root is refused");

        var tooManyEntries = Bytes.SnapshotPayload(
            Bytes.Concat(Bytes.U8(1), Bytes.U32(WireLimits.MaxSettingsMapEntries + 1)));
        Harness.Check(!TryDecodeSnapshot(tooManyEntries, out _),
            "a map over the entry bound is refused");

        var overLongString = Bytes.SnapshotPayload(
            Bytes.Map(("k", Bytes.Concat(Bytes.U8(0), Bytes.U32(WireLimits.MaxStringLength + 1)))));
        Harness.Check(!TryDecodeSnapshot(overLongString, out _),
            "an over-long string is refused");

        var badStatus = Bytes.Concat(Bytes.U8(99), Bytes.Str(string.Empty), Bytes.Map(), Bytes.U32(0));
        Harness.Check(!SettingsCodec.TryDecodeStatus(new ProtoReader(badStatus), out _, out _),
            "an out-of-range status is refused");

        var noTypes = Bytes.SnapshotPayload(Bytes.Map());
        Harness.Check(TryDecodeSnapshot(noTypes, out _),
            "a snapshot with no types decodes");
        var overTypes = Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty), Bytes.Map(),
                                     Bytes.U32(WireLimits.MaxSettingsTypes + 1));
        Harness.Check(!TryDecodeSnapshot(overTypes, out _),
            "a snapshot over the type bound is refused");
    }

    private static void RequestEncoding()
    {
        Harness.Section("codec: request encoding");

        var getConfig = SettingsCodec.EncodeGetConfig("fcitx://config/global");
        var reader = new ProtoReader(getConfig);
        Harness.Check(reader.TryString(out var uri) && reader.AtEnd, "a get request is exactly one string");
        Harness.CheckEqual("fcitx://config/global", uri, "the URI is carried verbatim");

        var setConfig = SettingsCodec.EncodeSetConfig("fcitx://config/inputmethod/pinyin",
            SettingsValue.Map(("PageSize", SettingsValue.Text("9"))));
        var setReader = new ProtoReader(setConfig);
        Harness.Check(setReader.TryString(out var setUri), "a set request starts with its URI");
        Harness.CheckEqual("fcitx://config/inputmethod/pinyin", setUri, "the URI is the view's own");
        Harness.Check(setReader.TryU8(out var tag) && tag == 1, "the values are a map");
        Harness.Check(setReader.TryU32(out var count) && count == 1, "with one entry");
        Harness.Check(setReader.TryString(out var key) && key == "PageSize", "the changed key");
        Harness.Check(setReader.TryU8(out var leafTag) && leafTag == 0, "a scalar leaf");
        Harness.Check(setReader.TryString(out var leaf) && leaf == "9", "with the new value");
        Harness.Check(setReader.AtEnd, "and nothing else");

        var hello = SettingsCodec.EncodeHello();
        var helloReader = new ProtoReader(hello);
        var protocolOk = helloReader.TryU32(out var protocol);
        var apiOk = helloReader.TryU32(out var api);
        Harness.Check(protocolOk && apiOk && helloReader.AtEnd, "hello is two u32 fields");
        Harness.CheckEqual(WireLimits.ProtocolVersion, protocol, "hello carries the IPC protocol version");
        Harness.CheckEqual(WireLimits.SettingsApiVersion, api, "and the API version this client requires");

        var list = SettingsCodec.EncodeStringList(new[] { "pinyin", "shuangpin" });
        var listReader = new ProtoReader(list);
        Harness.Check(listReader.TryU32(out var listCount) && listCount == 2, "a list request counts its items");
        Harness.Check(listReader.TryString(out var first) && first == "pinyin", "in order");

        var select = SettingsCodec.EncodeSelectInputMethod("shuangpin");
        var selectReader = new ProtoReader(select);
        Harness.Check(selectReader.TryString(out var method) && method == "shuangpin",
            "a select request is one method name");
    }

    private static void OtherReplies()
    {
        Harness.Section("codec: selection, input-method list and hello replies");

        var selection = Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty), Bytes.Str("pinyin"),
                                     Bytes.U32(2), Bytes.Str("pinyin"), Bytes.Str("shuangpin"));
        var selectionReader = new ProtoReader(selection);
        Harness.Check(SettingsCodec.TryDecodeStatus(selectionReader, out _, out _), "selection status");
        Harness.Check(SettingsCodec.TryDecodeSelection(selectionReader, out var decodedSelection),
            "selection decodes");
        Harness.CheckEqual("pinyin", decodedSelection.Selected, "the selected method");
        Harness.CheckEqual(2, decodedSelection.Available.Count, "the available methods");

        var imList = Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty),
                                  Bytes.Str("pinyin"), Bytes.Str("pinyin"),
                                  Bytes.U32(2), Bytes.Str("pinyin"), Bytes.Str("shuangpin"),
                                  Bytes.U32(2), Bytes.Str("pinyin"), Bytes.Str("shuangpin"));
        var imReader = new ProtoReader(imList);
        Harness.Check(SettingsCodec.TryDecodeStatus(imReader, out _, out _), "IM list status");
        Harness.Check(SettingsCodec.TryDecodeImList(imReader, out var decodedList), "IM list decodes");
        Harness.CheckEqual("pinyin", decodedList.Active, "Items/0 is the active method");
        Harness.CheckEqual("pinyin", decodedList.Fallback, "the fallback is carried separately");
        Harness.CheckEqual(2, decodedList.Enabled.Count, "the ordered list");
        Harness.CheckEqual(2, decodedList.Available.Count, "the available set");

        var truncatedList = Bytes.Concat(Bytes.U8(0), Bytes.Str(string.Empty), Bytes.Str("pinyin"));
        var truncatedReader = new ProtoReader(truncatedList);
        SettingsCodec.TryDecodeStatus(truncatedReader, out _, out _);
        Harness.Check(!SettingsCodec.TryDecodeImList(truncatedReader, out _),
            "a truncated IM list is refused");

        var hello = Bytes.Concat(Bytes.U32(WireLimits.ProtocolVersion), Bytes.U64(42), Bytes.U32(3),
                                 Bytes.U8(0), Bytes.Str(string.Empty));
        Harness.Check(SettingsCodec.TryDecodeHello(new ProtoReader(hello), out var protocol,
                out var session, out var api, out var status, out _),
            "a hello reply decodes");
        Harness.CheckEqual(WireLimits.ProtocolVersion, protocol, "the IPC protocol version");
        Harness.CheckEqual(42ul, session, "the backend session id");
        Harness.CheckEqual(3u, api, "a newer Host's API version is readable, not rejected here");
        Harness.CheckEqual(SettingsStatus.Ok, status, "the status");

        var helloTrailing = Bytes.Concat(hello, Bytes.U8(0));
        Harness.Check(!SettingsCodec.TryDecodeHello(new ProtoReader(helloTrailing), out _, out _, out _,
                out _, out _), "a hello reply with trailing bytes is refused");
    }
}
