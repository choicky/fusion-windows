using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// Transport representation of Fcitx's runtime configuration tree, matching
/// <c>fusion::SettingsValue</c> on the Host side: either a scalar string or a map of
/// named children. It carries values only; it never describes an option schema.
/// </summary>
/// <remarks>
/// Children are held in ordinal order because the C++ side serialises a
/// <c>std::map&lt;std::string, SettingsValue&gt;</c>. Option names are ASCII, so ordinal
/// and byte ordering agree; the sorted container keeps a round trip byte-identical.
/// </remarks>
public sealed class SettingsValue : IEquatable<SettingsValue>
{
    private readonly SortedDictionary<string, SettingsValue>? _children;

    private SettingsValue(string scalar)
    {
        Scalar = scalar;
    }

    private SettingsValue(SortedDictionary<string, SettingsValue> children)
    {
        _children = children;
    }

    /// <summary>The scalar text, or an empty string for a map.</summary>
    public string Scalar { get; } = string.Empty;

    public bool IsMap => _children is not null;

    public IReadOnlyDictionary<string, SettingsValue> Children =>
        _children ?? (IReadOnlyDictionary<string, SettingsValue>)EmptyChildren;

    private static readonly SortedDictionary<string, SettingsValue> EmptyChildren =
        new(StringComparer.Ordinal);

    public static SettingsValue Text(string value) => new(value ?? string.Empty);

    public static SettingsValue EmptyMap() => new(new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal));

    public static SettingsValue Map(params (string Key, SettingsValue Value)[] entries)
    {
        var map = new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal);
        foreach (var (key, value) in entries)
        {
            map[key] = value;
        }

        return new SettingsValue(map);
    }

    public bool TryGet(string key, out SettingsValue value)
    {
        if (_children is not null && _children.TryGetValue(key, out var found))
        {
            value = found;
            return true;
        }

        value = Text(string.Empty);
        return false;
    }

    /// <summary>Looks a value up by a slash-separated path of map keys.</summary>
    public bool TryGetPath(IReadOnlyList<string> path, out SettingsValue value)
    {
        SettingsValue current = this;
        for (var i = 0; i < path.Count; i++)
        {
            if (!current.TryGet(path[i], out var next))
            {
                value = Text(string.Empty);
                return false;
            }

            current = next;
        }

        value = current;
        return true;
    }

    public SettingsValue Clone()
    {
        if (_children is null)
        {
            return new SettingsValue(Scalar);
        }

        var copy = new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal);
        foreach (var (key, child) in _children)
        {
            copy[key] = child.Clone();
        }

        return new SettingsValue(copy);
    }

    /// <summary>
    /// Returns a copy with the scalar at <paramref name="path"/> replaced, creating
    /// intermediate maps as needed. The receiver is not modified: a draft never
    /// mutates the baseline it was taken from.
    /// </summary>
    public SettingsValue WithPath(IReadOnlyList<string> path, SettingsValue value)
    {
        if (path.Count == 0)
        {
            return value;
        }

        var copy = _children is null
            ? new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal)
            : new SortedDictionary<string, SettingsValue>(_children, StringComparer.Ordinal);

        var head = path[0];
        var rest = path.Skip(1).ToArray();
        var child = copy.TryGetValue(head, out var existing) ? existing : EmptyMap();
        copy[head] = rest.Length == 0 ? value : child.WithPath(rest, value);
        return new SettingsValue(copy);
    }

    /// <summary>Every leaf as a (slash path, scalar) pair, in ordinal key order.</summary>
    public IEnumerable<(string Path, string Scalar)> Leaves(string prefix = "")
    {
        if (_children is null)
        {
            yield return (prefix, Scalar);
            yield break;
        }

        foreach (var (key, child) in _children)
        {
            var path = prefix.Length == 0 ? key : prefix + "/" + key;
            foreach (var leaf in child.Leaves(path))
            {
                yield return leaf;
            }
        }
    }

    /// <summary>
    /// A tree containing only the leaves whose path is in <paramref name="paths"/>,
    /// with the original nesting preserved. This is what a save submits: the Host
    /// merges a submitted subtree into the live configuration, so sending only what
    /// changed is both supported and the reason a stale view cannot revert a field
    /// it never touched.
    /// </summary>
    public SettingsValue SubsetOf(IReadOnlySet<string> paths, string prefix = "")
    {
        if (_children is null)
        {
            return paths.Contains(prefix) ? Clone() : EmptyMap();
        }

        var subset = new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal);
        foreach (var (key, child) in _children)
        {
            var path = prefix.Length == 0 ? key : prefix + "/" + key;
            var childSubset = child.SubsetOf(paths, path);
            if (childSubset.IsMap)
            {
                if (childSubset.Children.Count > 0)
                {
                    subset[key] = childSubset;
                }
            }
            else
            {
                subset[key] = childSubset;
            }
        }

        return new SettingsValue(subset);
    }

    public bool Equals(SettingsValue? other)
    {
        if (other is null)
        {
            return false;
        }

        if (ReferenceEquals(this, other))
        {
            return true;
        }

        if (IsMap != other.IsMap)
        {
            return false;
        }

        if (!IsMap)
        {
            return string.Equals(Scalar, other.Scalar, StringComparison.Ordinal);
        }

        if (Children.Count != other.Children.Count)
        {
            return false;
        }

        foreach (var (key, child) in Children)
        {
            if (!other.TryGet(key, out var otherChild) || !child.Equals(otherChild))
            {
                return false;
            }
        }

        return true;
    }

    public override bool Equals(object? obj) => Equals(obj as SettingsValue);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        hash.Add(IsMap);
        if (_children is null)
        {
            hash.Add(Scalar, StringComparer.Ordinal);
        }
        else
        {
            foreach (var (key, child) in _children)
            {
                hash.Add(key, StringComparer.Ordinal);
                hash.Add(child);
            }
        }

        return hash.ToHashCode();
    }

    public override string ToString() => IsMap ? $"{{{string.Join(",", Children.Keys)}}}" : Scalar;
}
