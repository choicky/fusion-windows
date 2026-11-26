using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Fusion.Settings.Model;

/// <summary>One row of the Fcitx zh_CN punctuation map.</summary>
/// <remarks>
/// <para>
/// A key may repeat: the shipped map has two rows for '#', giving it an ASCII and a full-width
/// candidate. Their order is the candidate order, and AltMapping is the second half of a paired
/// punctuation entry, not another row. None of that is modelled as a dictionary.
/// </para>
/// </remarks>
public sealed record PunctuationRow(string Key, string Mapping, string AltMapping);

/// <summary>
/// The lossless view of an <c>Entries</c> value tree. The schema, defaults and storage stay in
/// Fcitx; this only walks the indexed list shape the Host already sends for every List option.
/// </summary>
public static class PunctuationMap
{
    public const string Entries = "Entries";

    public static IReadOnlyList<PunctuationRow> Rows(SettingsValue root)
    {
        if (!root.TryGet(Entries, out var entries) || !entries.IsMap)
        {
            return Array.Empty<PunctuationRow>();
        }

        var result = new List<PunctuationRow>();
        for (var index = 0; index < entries.Children.Count; index++)
        {
            if (!entries.TryGet(index.ToString(CultureInfo.InvariantCulture), out var item) ||
                !item.IsMap)
            {
                throw new InvalidOperationException("Fcitx returned a non-contiguous Entries list.");
            }

            string Field(string name)
            {
                if (!item.TryGet(name, out var value) || value.IsMap)
                {
                    throw new InvalidOperationException("Fcitx returned a malformed punctuation row.");
                }
                return value.Scalar;
            }

            result.Add(new PunctuationRow(Field("Key"), Field("Mapping"), Field("AltMapping")));
        }
        return result;
    }

    /// <summary>
    /// Reads the value returned by <c>SettingsDraft.Effective(PunctuationMap.Entries)</c>.
    /// A draft's <c>Effective</c> returns the option's value itself, whereas
    /// <see cref="Rows"/> reads a whole configuration value tree; mixing the two silently
    /// produced an empty table.
    /// </summary>
    public static IReadOnlyList<PunctuationRow> RowsFromOption(SettingsValue optionValue) =>
        Rows(SettingsValue.Map((Entries, optionValue)));

    /// <summary>
    /// The value of the <see cref="Entries"/> option, ready for
    /// <c>SettingsDraft.TrySet</c>. It is deliberately not wrapped in a root
    /// <c>{ Entries: ... }</c> tree: <c>TrySet</c> already addresses the option by name, so
    /// wrapping here would make the Host see a one-child list and reject it as a gapped index.
    /// </summary>
    public static SettingsValue Value(IEnumerable<PunctuationRow> rows)
    {
        var entries = new SortedDictionary<string, SettingsValue>(StringComparer.Ordinal);
        var index = 0;
        foreach (var row in rows)
        {
            entries[index.ToString(CultureInfo.InvariantCulture)] = SettingsValue.Map(
                ("Key", SettingsValue.Text(row.Key)),
                ("Mapping", SettingsValue.Text(row.Mapping)),
                ("AltMapping", SettingsValue.Text(row.AltMapping)));
            index++;
        }
        return SettingsValue.Map(entries.Select(pair => (pair.Key, pair.Value)).ToArray());
    }

    /// <summary>The shipped table as Fcitx describes it, for the restore action.</summary>
    public static IReadOnlyList<PunctuationRow> Defaults(SettingsSnapshot snapshot)
    {
        foreach (var type in snapshot.Types)
        {
            var option = type.Options.FirstOrDefault(candidate =>
                string.Equals(candidate.Name, Entries, StringComparison.Ordinal));
            if (option is not null && option.DefaultValue.IsMap)
            {
                return Rows(SettingsValue.Map((Entries, option.DefaultValue)));
            }
        }
        return Array.Empty<PunctuationRow>();
    }

    /// <summary>
    /// The rules Fcitx's PunctuationProfile::set applies silently. Checking them before the
    /// draft is changed keeps a row the editor cannot store from looking accepted.
    /// </summary>
    public static string? InvalidReason(IReadOnlyList<PunctuationRow> rows)
    {
        foreach (var row in rows)
        {
            var runeCount = 0;
            foreach (var _ in row.Key.EnumerateRunes())
            {
                runeCount++;
                if (runeCount > 1)
                {
                    break;
                }
            }
            if (runeCount != 1)
            {
                return "键必须是一个字符。";
            }
            if (string.IsNullOrEmpty(row.Mapping))
            {
                return "映射不能为空。";
            }
            if (HasWhitespace(row) || ContainsSurrogate(row.Key))
            {
                return "键和映射中不能有空白，键必须是一个完整字符。";
            }
        }
        return null;
    }

    private static bool HasWhitespace(PunctuationRow row) =>
        row.Key.Any(char.IsWhiteSpace) || row.Mapping.Any(char.IsWhiteSpace) ||
        row.AltMapping.Any(char.IsWhiteSpace);

    private static bool ContainsSurrogate(string text) =>
        text.Any(character => char.IsSurrogate(character));
}
