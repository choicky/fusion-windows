using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// One key combination — a single tag in the editor. A chord is never split across
/// tags: <c>Control+Return</c> is one binding, not "Control" plus "Return".
/// </summary>
/// <remarks>
/// Names follow Fcitx's portable form. Modifiers are written in the order Fcitx uses
/// (Control, Alt, Shift, Super, Hyper) so a parsed chord re-serialises to the exact
/// string the Host would have produced.
/// </remarks>
public sealed class KeyChord : IEquatable<KeyChord>
{
    /// <summary>Modifier order as emitted by <c>Key::toString</c>.</summary>
    public static readonly IReadOnlyList<string> ModifierOrder =
        new[] { "Control", "Alt", "Shift", "Super", "Hyper" };

    private static readonly Dictionary<string, string> CanonicalModifiers =
        ModifierOrder.ToDictionary(name => name, name => name, StringComparer.OrdinalIgnoreCase);

    private KeyChord(IReadOnlyList<string> modifiers, string keyName, string? opaqueText)
    {
        Modifiers = modifiers;
        KeyName = keyName;
        OpaqueText = opaqueText;
    }

    public IReadOnlyList<string> Modifiers { get; }

    /// <summary>The key itself, e.g. <c>space</c>, <c>KP_Enter</c>, <c>bracketleft</c>.</summary>
    public string KeyName { get; }

    /// <summary>
    /// The original text of a binding this client could not interpret, or null. Such a
    /// binding is carried through unchanged rather than replaced, so editing a neighbour
    /// cannot corrupt it.
    /// </summary>
    public string? OpaqueText { get; }

    /// <summary>True when the binding is kept verbatim because it could not be parsed.</summary>
    public bool IsOpaque => OpaqueText is not null;

    /// <summary>An empty chord carries no key at all (<c>FcitxKey_None</c>).</summary>
    public bool IsEmpty => !IsOpaque && KeyName.Length == 0;

    /// <summary>
    /// True when the binding is only a modifier (e.g. bare <c>Shift_L</c>). Such keys are
    /// reported as a limitation on Windows rather than silently accepted as working.
    /// </summary>
    public bool IsModifierOnly => IsModifierKeyName(KeyName);

    public static KeyChord Empty { get; } = new(Array.Empty<string>(), string.Empty, null);

    /// <summary>
    /// Wraps text this client cannot interpret so it survives a load/save round trip. The
    /// text is never normalised, because normalising it would be inventing an interpretation.
    /// </summary>
    public static KeyChord Opaque(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        return text.Length == 0 ? Empty : new KeyChord(Array.Empty<string>(), string.Empty, text);
    }

    public static bool IsModifierKeyName(string keyName) =>
        keyName is "Shift_L" or "Shift_R" or "Control_L" or "Control_R" or "Alt_L" or "Alt_R"
            or "Super_L" or "Super_R" or "Hyper_L" or "Hyper_R" or "Meta_L" or "Meta_R"
            or "Caps_Lock" or "Num_Lock";

    /// <summary>
    /// Parses one key string. Returns null when the text cannot be a chord, so a failed
    /// recording leaves the existing binding untouched instead of replacing it with
    /// something empty.
    /// </summary>
    public static KeyChord? Parse(string? text)
    {
        if (text is null)
        {
            return null;
        }

        var trimmed = text.Trim();
        if (trimmed.Length == 0)
        {
            return Empty;
        }

        var parts = trimmed.Split('+');
        var modifiers = new List<string>();
        string? keyName = null;
        for (var i = 0; i < parts.Length; i++)
        {
            var part = parts[i].Trim();
            if (part.Length == 0)
            {
                return null;
            }

            var isLast = i == parts.Length - 1;
            if (!isLast && CanonicalModifiers.TryGetValue(part, out var canonical))
            {
                if (!modifiers.Contains(canonical, StringComparer.Ordinal))
                {
                    modifiers.Add(canonical);
                }

                continue;
            }

            if (!isLast)
            {
                // "Control+Alt+P" is fine; "Foo+Bar" is not a modifier chain.
                return null;
            }

            keyName = part;
        }

        if (keyName is null)
        {
            return null;
        }

        modifiers.Sort((left, right) =>
            IndexOfModifier(left).CompareTo(IndexOfModifier(right)));
        return new KeyChord(modifiers, keyName, null);
    }

    private static int IndexOfModifier(string name)
    {
        for (var i = 0; i < ModifierOrder.Count; i++)
        {
            if (string.Equals(ModifierOrder[i], name, StringComparison.Ordinal))
            {
                return i;
            }
        }

        return ModifierOrder.Count;
    }

    public override string ToString() =>
        IsOpaque ? OpaqueText!
        : Modifiers.Count == 0 ? KeyName
        : string.Join("+", Modifiers) + "+" + KeyName;

    public bool Equals(KeyChord? other)
    {
        if (other is null)
        {
            return false;
        }

        if (IsOpaque || other.IsOpaque)
        {
            return string.Equals(OpaqueText, other.OpaqueText, StringComparison.Ordinal);
        }

        return string.Equals(KeyName, other.KeyName, StringComparison.Ordinal) &&
               Modifiers.SequenceEqual(other.Modifiers, StringComparer.Ordinal);
    }

    public override bool Equals(object? obj) => Equals(obj as KeyChord);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        if (IsOpaque)
        {
            hash.Add(OpaqueText, StringComparer.Ordinal);
            return hash.ToHashCode();
        }

        foreach (var modifier in Modifiers)
        {
            hash.Add(modifier, StringComparer.Ordinal);
        }

        hash.Add(KeyName, StringComparer.Ordinal);
        return hash.ToHashCode();
    }
}

/// <summary>
/// A <c>List|Key</c> value: the alternative bindings for one action, in their official
/// order.
/// </summary>
/// <remarks>
/// <para>
/// The wire shape is not a space-separated string. Verified against the running Host on
/// 2026-10-21: <c>PrevPage</c> arrives as the map <c>{"0":"minus","1":"Up","2":"KP_Up",
/// "3":"Page_Up"}</c>, and an empty list arrives as the scalar <c>""</c>. Fcitx's
/// marshaller writes a vector as indexed keys, so the client must preserve index order
/// rather than sort or de-duplicate.
/// </para>
/// <para>
/// Most of these lists are sets of alternatives for one action, so order is cosmetic.
/// A few are positional: in <c>ChooseCharFromPhrase</c> the N-th binding selects the
/// N-th character of the candidate, so order is meaning and is never normalised.
/// </para>
/// </remarks>
public sealed class KeyBindingList
{
    private readonly List<KeyChord> _items;

    private KeyBindingList(List<KeyChord> items, bool isPositional)
    {
        _items = items;
        IsPositional = isPositional;
    }

    public IReadOnlyList<KeyChord> Items => _items;

    public int Count => _items.Count;

    /// <summary>True when position carries meaning (e.g. <c>ChooseCharFromPhrase</c>).</summary>
    public bool IsPositional { get; }

    public KeyChord this[int index] => _items[index];

    public static KeyBindingList Empty(bool isPositional = false) => new(new List<KeyChord>(), isPositional);

    /// <summary>
    /// Reads a value into a binding list. A malformed entry is kept as an unparsable
    /// chord rather than dropped: silently discarding a user's binding would change
    /// their configuration without telling them.
    /// </summary>
    public static KeyBindingList FromValue(SettingsValue value, bool isPositional = false)
    {
        ArgumentNullException.ThrowIfNull(value);
        var items = new List<KeyChord>();
        if (!value.IsMap)
        {
            // An empty scalar is the canonical empty list. A non-empty one is not what
            // the Host produces for this type; split it the way Fcitx's
            // keyListFromString would rather than lose the binding.
            if (value.Scalar.Trim().Length == 0)
            {
                return new KeyBindingList(items, isPositional);
            }

            foreach (var token in value.Scalar.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
            {
                items.Add(KeyChord.Parse(token) ?? KeyChord.Opaque(token));
            }

            return new KeyBindingList(items, isPositional);
        }

        foreach (var (key, child) in value.Children
                     .OrderBy(entry => int.TryParse(entry.Key, out var index) ? index : int.MaxValue))
        {
            // An unparsable binding keeps its text. Replacing it with Empty would turn a
            // binding this client does not understand into a deletion on the next save.
            items.Add(KeyChord.Parse(child.Scalar) ?? KeyChord.Opaque(child.Scalar));
        }

        return new KeyBindingList(items, isPositional);
    }

    /// <summary>Writes the list back in the shape the Host expects.</summary>
    public SettingsValue ToValue()
    {
        if (_items.Count == 0)
        {
            return SettingsValue.Text(string.Empty);
        }

        var entries = new (string, SettingsValue)[_items.Count];
        for (var i = 0; i < _items.Count; i++)
        {
            entries[i] = (i.ToString(System.Globalization.CultureInfo.InvariantCulture),
                          SettingsValue.Text(_items[i].ToString()));
        }

        return SettingsValue.Map(entries);
    }

    /// <summary>
    /// Appends a binding verbatim, including an empty or unparsable one. This is a list
    /// model: it does not decide what may be added, because silently dropping an entry here
    /// would change a user's configuration on any copy or replacement path.
    /// </summary>
    public void Add(KeyChord chord)
    {
        ArgumentNullException.ThrowIfNull(chord);
        _items.Add(chord);
    }

    public void Clear() => _items.Clear();

    public bool RemoveAt(int index)
    {
        if (index < 0 || index >= _items.Count)
        {
            return false;
        }

        _items.RemoveAt(index);
        return true;
    }

    public bool Move(int from, int to)
    {
        if (from < 0 || from >= _items.Count || to < 0 || to >= _items.Count)
        {
            return false;
        }

        var item = _items[from];
        _items.RemoveAt(from);
        _items.Insert(to, item);
        return true;
    }

    public bool Contains(KeyChord chord) => _items.Any(item => item.Equals(chord));

    /// <summary>
    /// Indices of bindings that repeat an earlier one. Reported, never rewritten: which
    /// duplicate wins is Fcitx's business, and inventing a priority here would change
    /// behaviour the user did not ask to change.
    /// </summary>
    public IReadOnlyList<int> DuplicateIndices()
    {
        var duplicates = new List<int>();
        for (var i = 0; i < _items.Count; i++)
        {
            for (var j = 0; j < i; j++)
            {
                if (_items[j].Equals(_items[i]))
                {
                    duplicates.Add(i);
                    break;
                }
            }
        }

        return duplicates;
    }

    /// <summary>
    /// True when this list overlaps another action's list. Overlap is a warning only:
    /// the lists are not merged, reordered or trimmed.
    /// </summary>
    public bool Overlaps(KeyBindingList other) =>
        other is not null && _items.Any(item => other.Contains(item));

    public KeyBindingList Clone() => new(new List<KeyChord>(_items), IsPositional);
}
