using System;
using System.Collections.Generic;
using Fusion.Settings.Model;

namespace Fusion.Settings.Ipc;

/// <summary>
/// Encoder/decoder for the settings messages, matching
/// <c>fusion::ipc::settings_protocol.hpp</c> field for field.
/// </summary>
/// <remarks>
/// Every decoder returns false rather than a partially built object, and every
/// decoder rejects trailing bytes. A malformed or truncated reply is therefore a
/// transport failure, never a silently wrong value.
/// </remarks>
public static class SettingsCodec
{
    public static byte[] EncodeHello() => EncodeHello(WireLimits.ProtocolVersion, WireLimits.SettingsApiVersion);

    public static byte[] EncodeHello(uint protocol, uint api)
    {
        var writer = new ProtoWriter();
        writer.U32(protocol);
        writer.U32(api);
        return writer.ToArray();
    }

    public static bool TryDecodeHello(ProtoReader reader, out uint protocol, out ulong session,
                                      out uint api, out SettingsStatus status, out string error)
    {
        protocol = 0;
        session = 0;
        api = 0;
        status = SettingsStatus.Error;
        error = string.Empty;
        if (!reader.TryU32(out protocol) || !reader.TryU64(out session) || !reader.TryU32(out api))
        {
            return false;
        }

        if (!TryDecodeStatus(reader, out status, out _, out error))
        {
            return false;
        }

        return reader.AtEnd;
    }

    public static bool TryDecodeStatus(ProtoReader reader, out SettingsStatus status,
                                       out SettingsDetail detail, out string error)
    {
        status = SettingsStatus.Error;
        detail = SettingsDetail.None;
        error = string.Empty;
        if (!reader.TryU8(out var raw) || raw > (byte)SettingsStatus.Pending)
        {
            return false;
        }

        if (!reader.TryU8(out var rawDetail) || rawDetail > (byte)SettingsDetail.UnsettledWork)
        {
            return false;
        }

        status = (SettingsStatus)raw;
        detail = (SettingsDetail)rawDetail;
        return reader.TryString(out error);
    }

    public static byte[] EncodeGetConfig(string uri)
    {
        var writer = new ProtoWriter();
        if (!writer.Text(uri))
        {
            throw new ArgumentException("the URI exceeds the wire bound", nameof(uri));
        }

        return writer.ToArray();
    }

    public static byte[] EncodeSetConfig(string uri, SettingsValue values)
    {
        var writer = new ProtoWriter();
        if (!writer.Text(uri) || !TryWriteValue(writer, values, 0, new NodeBudget()))
        {
            throw new ArgumentException("the settings request exceeds the wire bound", nameof(values));
        }

        var payload = writer.ToArray();
        if (payload.Length > WireLimits.MaxSettingsPayload)
        {
            throw new ArgumentException("the settings request exceeds the payload bound", nameof(values));
        }

        return payload;
    }

    public static byte[] EncodeStringList(IReadOnlyList<string> items)
    {
        var writer = new ProtoWriter();
        if (!TryWriteStringList(writer, items))
        {
            throw new ArgumentException("the list exceeds the wire bound", nameof(items));
        }

        return writer.ToArray();
    }

    public static byte[] EncodeSelectInputMethod(string method)
    {
        var writer = new ProtoWriter();
        if (!writer.Text(method))
        {
            throw new ArgumentException("the method name exceeds the wire bound", nameof(method));
        }

        return writer.ToArray();
    }

    public static bool TryDecodeSnapshot(ProtoReader reader, out SettingsSnapshot snapshot)
    {
        snapshot = SettingsSnapshot.Empty;
        var budget = new NodeBudget();
        if (!TryReadValue(reader, 0, budget, out var values) || !values.IsMap)
        {
            return false;
        }

        if (!reader.TryU32(out var typeCount) || typeCount > WireLimits.MaxSettingsTypes)
        {
            return false;
        }

        var types = new List<ConfigType>((int)typeCount);
        for (var i = 0; i < typeCount; i++)
        {
            if (!reader.TryString(out var typeName) || !reader.TryU32(out var optionCount) ||
                optionCount > WireLimits.MaxSettingsOptions)
            {
                return false;
            }

            var options = new List<OptionDescription>((int)optionCount);
            for (var j = 0; j < optionCount; j++)
            {
                if (!TryReadOption(reader, budget, out var option))
                {
                    return false;
                }

                options.Add(option);
            }

            types.Add(new ConfigType(typeName, options));
        }

        if (!reader.AtEnd)
        {
            return false;
        }

        snapshot = new SettingsSnapshot(values, types);
        return true;
    }

    public static bool TryDecodeSelection(ProtoReader reader, out SettingsSelection selection)
    {
        selection = new SettingsSelection(string.Empty, Array.Empty<string>());
        if (!reader.TryString(out var selected) || !reader.TryU32(out var count) ||
            count > WireLimits.MaxVectorLength)
        {
            return false;
        }

        var available = new List<string>((int)count);
        for (var i = 0; i < count; i++)
        {
            if (!reader.TryString(out var method))
            {
                return false;
            }

            available.Add(method);
        }

        if (!reader.AtEnd)
        {
            return false;
        }

        selection = new SettingsSelection(selected, available);
        return true;
    }

    public static bool TryDecodeImList(ProtoReader reader, out ImList list)
    {
        list = new ImList(string.Empty, string.Empty, Array.Empty<string>(), Array.Empty<string>());
        if (!reader.TryString(out var active) || !reader.TryString(out var fallback) ||
            !TryReadStringList(reader, out var enabled) || !TryReadStringList(reader, out var available))
        {
            return false;
        }

        if (!reader.AtEnd)
        {
            return false;
        }

        list = new ImList(active, fallback, enabled, available);
        return true;
    }

    private static bool TryReadOption(ProtoReader reader, NodeBudget budget, out OptionDescription option)
    {
        option = null!;
        if (!reader.TryString(out var name) || !reader.TryString(out var type) ||
            !reader.TryString(out var description))
        {
            return false;
        }

        if (!TryReadValue(reader, 0, budget, out var defaultValue) ||
            !TryReadValue(reader, 0, budget, out var properties) || !properties.IsMap)
        {
            return false;
        }

        option = new OptionDescription(name, type, description, defaultValue, properties);
        return true;
    }

    private static bool TryWriteValue(ProtoWriter writer, SettingsValue value, int depth, NodeBudget budget)
    {
        if (depth > WireLimits.MaxSettingsDepth || !budget.TryConsume())
        {
            return false;
        }

        writer.U8(value.IsMap ? (byte)1 : (byte)0);
        if (!value.IsMap)
        {
            return writer.Text(value.Scalar);
        }

        if (value.Children.Count > WireLimits.MaxSettingsMapEntries)
        {
            return false;
        }

        writer.U32((uint)value.Children.Count);
        foreach (var (key, child) in value.Children)
        {
            if (!writer.Text(key) || !TryWriteValue(writer, child, depth + 1, budget))
            {
                return false;
            }
        }

        return true;
    }

    private static bool TryReadValue(ProtoReader reader, int depth, NodeBudget budget,
                                     out SettingsValue value)
    {
        value = SettingsValue.EmptyMap();
        if (depth > WireLimits.MaxSettingsDepth || !budget.TryConsume())
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

        // A capacity check only: each child is counted when it is decoded, exactly as the
        // Host counts. Consuming `count` here as well would charge every child twice.
        if (!reader.TryU32(out var count) || count > WireLimits.MaxSettingsMapEntries ||
            !budget.HasCapacityFor((int)count))
        {
            return false;
        }

        var entries = new List<(string Key, SettingsValue Value)>((int)count);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < count; i++)
        {
            if (!reader.TryString(out var key) || !seen.Add(key) ||
                !TryReadValue(reader, depth + 1, budget, out var child))
            {
                return false;
            }

            entries.Add((key, child));
        }

        value = SettingsValue.Map(entries.ToArray());
        return true;
    }

    private static bool TryWriteStringList(ProtoWriter writer, IReadOnlyList<string> items)
    {
        if (items.Count > WireLimits.MaxVectorLength)
        {
            return false;
        }

        writer.U32((uint)items.Count);
        foreach (var item in items)
        {
            if (!writer.Text(item))
            {
                return false;
            }
        }

        return true;
    }

    private static bool TryReadStringList(ProtoReader reader, out IReadOnlyList<string> items)
    {
        items = Array.Empty<string>();
        if (!reader.TryU32(out var count) || count > WireLimits.MaxVectorLength)
        {
            return false;
        }

        var list = new List<string>((int)count);
        for (var i = 0; i < count; i++)
        {
            if (!reader.TryString(out var item))
            {
                return false;
            }

            list.Add(item);
        }

        items = list;
        return true;
    }

    /// <summary>
    /// Shared node budget for one reply. The Host applies the same ceiling, so a reply
    /// that exceeds it is malformed rather than merely large.
    /// </summary>
    private sealed class NodeBudget
    {
        private int _used;

        public bool TryConsume()
        {
            if (_used + 1 > WireLimits.MaxSettingsNodes)
            {
                return false;
            }

            _used++;
            return true;
        }

        /// <summary>
        /// True when <paramref name="count"/> further nodes still fit. It does not consume
        /// them: the recursion consumes one node per value it actually decodes.
        /// </summary>
        public bool HasCapacityFor(int count) => _used + count <= WireLimits.MaxSettingsNodes;
    }
}
