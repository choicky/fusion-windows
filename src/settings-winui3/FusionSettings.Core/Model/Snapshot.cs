using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>Result status, mirroring <c>fusion::SettingsStatus</c> plus a client-only failure.</summary>
public enum SettingsStatus
{
    Ok = 0,
    Error = 1,
    Busy = 2,
    Unsupported = 3,
    Invalid = 4,
    Timeout = 5,
    Indeterminate = 6,
    VersionMismatch = 7,

    /// <summary>
    /// Client-only: the Host could not be reached, or the connection failed before a
    /// reply. It is never sent by the Host and it never means "the write happened".
    /// </summary>
    Unreachable = 100,
}

/// <summary>
/// A lossless view of one option as Fcitx describes it, plus helpers that read the
/// description without inventing a second schema.
/// </summary>
public sealed class OptionDescription
{
    public OptionDescription(string name, string type, string description,
                             SettingsValue defaultValue, SettingsValue properties)
    {
        Name = name;
        Type = type;
        Description = description;
        DefaultValue = defaultValue;
        Properties = properties;
    }

    /// <summary>The stored option name, which is also the key in the value tree.</summary>
    public string Name { get; }

    /// <summary>
    /// Fcitx's type string: <c>Boolean</c>, <c>Integer</c>, <c>String</c>, <c>Enum</c>,
    /// <c>Key</c>, <c>List|Key</c>, <c>List|String</c>, <c>External</c>, or
    /// <c>&lt;Type&gt;$&lt;Name&gt;</c> for a nested configuration.
    /// </summary>
    public string Type { get; }

    public string Description { get; }

    public SettingsValue DefaultValue { get; }

    public SettingsValue Properties { get; }

    public bool IsKeyList => Type == "List|Key";

    public bool IsSingleKey => Type == "Key";

    public bool IsExternal => Type == "External";

    public bool IsNested => Type.Contains('$', StringComparison.Ordinal);

    /// <summary>Enum labels in index order, or an empty list when this is not an enum.</summary>
    public IReadOnlyList<string> EnumLabels
    {
        get
        {
            if (Type != "Enum" || !Properties.TryGet("Enum", out var map) || !map.IsMap)
            {
                return Array.Empty<string>();
            }

            return map.Children
                .OrderBy(entry => int.TryParse(entry.Key, out var index) ? index : int.MaxValue)
                .Select(entry => entry.Value.Scalar)
                .ToArray();
        }
    }

    /// <summary>Localised enum labels when the Host supplied them, else the plain ones.</summary>
    public IReadOnlyList<string> LocalizedEnumLabels
    {
        get
        {
            if (Properties.TryGet("EnumI18n", out var map) && map.IsMap && map.Children.Count > 0)
            {
                return map.Children
                    .OrderBy(entry => int.TryParse(entry.Key, out var index) ? index : int.MaxValue)
                    .Select(entry => entry.Value.Scalar)
                    .ToArray();
            }

            return EnumLabels;
        }
    }

    /// <summary>The integer lower bound, or null when the option has none.</summary>
    public int? IntMinimum =>
        Properties.TryGet("IntMin", out var value) && int.TryParse(value.Scalar, out var parsed)
            ? parsed
            : null;

    public int? IntMaximum =>
        Properties.TryGet("IntMax", out var value) && int.TryParse(value.Scalar, out var parsed)
            ? parsed
            : null;

    /// <summary>The sub-configuration URI of an External option, or null.</summary>
    public string? ExternalUri =>
        Properties.TryGet("External", out var value) && !string.IsNullOrEmpty(value.Scalar)
            ? value.Scalar
            : null;

    /// <summary>True when activating the link opens a sub-configuration page rather than a dialog.</summary>
    public bool LaunchesSubConfig =>
        Properties.TryGet("LaunchSubConfig", out var value) &&
        string.Equals(value.Scalar, "True", StringComparison.OrdinalIgnoreCase);
}

/// <summary>One configuration type: its name and the options it exposes in this view.</summary>
public sealed class ConfigType
{
    public ConfigType(string name, IReadOnlyList<OptionDescription> options)
    {
        Name = name;
        Options = options;
    }

    public string Name { get; }

    public IReadOnlyList<OptionDescription> Options { get; }
}

/// <summary>
/// One Host reply: the value tree and the schema that describes it. The schema is
/// authoritative — the client never keeps its own copy of types or defaults.
/// </summary>
public sealed class SettingsSnapshot
{
    public SettingsSnapshot(SettingsValue values, IReadOnlyList<ConfigType> types)
    {
        Values = values;
        Types = types;
    }

    public SettingsValue Values { get; }

    public IReadOnlyList<ConfigType> Types { get; }

    public static SettingsSnapshot Empty { get; } =
        new(SettingsValue.EmptyMap(), Array.Empty<ConfigType>());

    /// <summary>
    /// The configuration type a view edits directly. Every other type in the reply is a
    /// nested configuration reached through one of its options.
    /// </summary>
    public ConfigType? RootType => Types.Count > 0 ? Types[0] : null;

    /// <summary>
    /// An option of the root type. A top-level edit must go through this rather than
    /// <see cref="FindOption"/>, or a nested option such as <c>VE_UE</c> would look
    /// editable at the top level where it does not exist.
    /// </summary>
    public OptionDescription? FindRootOption(string name)
    {
        var root = RootType;
        if (root is null)
        {
            return null;
        }

        foreach (var option in root.Options)
        {
            if (string.Equals(option.Name, name, StringComparison.Ordinal))
            {
                return option;
            }
        }

        return null;
    }

    /// <summary>
    /// The option with this name across every type in the view. A name is unique
    /// within one configuration, so a linear search over the (bounded) schema is
    /// enough and avoids a second index that could drift.
    /// </summary>
    public OptionDescription? FindOption(string name)
    {
        foreach (var type in Types)
        {
            foreach (var option in type.Options)
            {
                if (string.Equals(option.Name, name, StringComparison.Ordinal))
                {
                    return option;
                }
            }
        }

        return null;
    }

    /// <summary>The type that owns <paramref name="name"/>, or null.</summary>
    public ConfigType? FindTypeOf(string name)
    {
        foreach (var type in Types)
        {
            if (type.Options.Any(option => string.Equals(option.Name, name, StringComparison.Ordinal)))
            {
                return type;
            }
        }

        return null;
    }

    /// <summary>
    /// Every option name this view exposes. A value the view does not describe is
    /// never submitted from that view: the pinyin view, for example, does not describe
    /// <c>ShuangpinProfile</c>, and writing it from there would change the other
    /// method's setting behind its back.
    /// </summary>
    public IReadOnlySet<string> SchemaOptionNames
    {
        get
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var type in Types)
            {
                foreach (var option in type.Options)
                {
                    names.Add(option.Name);
                }
            }

            return names;
        }
    }
}

/// <summary>Which input method the Host currently follows, and which ones exist.</summary>
public sealed class SettingsSelection
{
    public SettingsSelection(string selected, IReadOnlyList<string> available)
    {
        Selected = selected;
        Available = available;
    }

    public string Selected { get; }

    public IReadOnlyList<string> Available { get; }
}

/// <summary>
/// One input-method group's membership. <see cref="Active"/> is Items/0, the method a
/// bound context follows; <see cref="Fallback"/> is Fcitx's DefaultIM and is not the
/// active method; <see cref="Enabled"/> is the ordered Items list, so
/// <c>Enabled[0] == Active</c> when it is non-empty.
/// </summary>
public sealed class ImList
{
    public ImList(string active, string fallback, IReadOnlyList<string> enabled,
                  IReadOnlyList<string> available)
    {
        Active = active;
        Fallback = fallback;
        Enabled = enabled;
        Available = available;
    }

    public string Active { get; }

    public string Fallback { get; }

    public IReadOnlyList<string> Enabled { get; }

    public IReadOnlyList<string> Available { get; }
}

/// <summary>The Host's answer, with the incarnation that produced it.</summary>
public sealed class SettingsResult<T>
{
    public SettingsResult(SettingsStatus status, string error, T? value, ulong incarnation,
                          bool mutationMayHaveBeenApplied = false, bool cancelled = false)
    {
        Status = status;
        Error = error;
        Value = value;
        HostIncarnation = incarnation;
        MutationMayHaveBeenApplied = mutationMayHaveBeenApplied;
        Cancelled = cancelled;
    }

    public SettingsStatus Status { get; }

    public string Error { get; }

    public T? Value { get; }

    public ulong HostIncarnation { get; }

    /// <summary>
    /// True when this result is about a mutation whose request had already started down the
    /// pipe, so the Host may have applied it even though no usable reply came back. It is
    /// the signal to reread and reconcile, and never a reason to replay.
    /// </summary>
    public bool MutationMayHaveBeenApplied { get; }

    /// <summary>
    /// True when the caller's own cancellation token ended the request. A cancellation that
    /// arrives after a mutation started does not prove the write was rejected, so the result
    /// keeps that fact instead of throwing it away.
    /// </summary>
    public bool Cancelled { get; }

    public bool IsOk => Status == SettingsStatus.Ok;

    /// <summary>
    /// True when the outcome is unknown: either the Host said so, or the request may have
    /// reached it without a usable reply. Both mean "reread before deciding", not "retry".
    /// </summary>
    public bool IsUncertain => Status == SettingsStatus.Indeterminate || MutationMayHaveBeenApplied;

}

/// <summary>Factories for <see cref="SettingsResult{T}"/>.</summary>
public static class SettingsResult
{
    public static SettingsResult<T> Ok<T>(T value, ulong incarnation) =>
        new(SettingsStatus.Ok, string.Empty, value, incarnation);

    public static SettingsResult<T> Failure<T>(SettingsStatus status, string error,
                                               ulong incarnation = 0,
                                               bool mutationMayHaveBeenApplied = false,
                                               bool cancelled = false) =>
        new(status, error, default, incarnation, mutationMayHaveBeenApplied, cancelled);
}
