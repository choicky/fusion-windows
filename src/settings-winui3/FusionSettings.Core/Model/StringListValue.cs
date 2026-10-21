using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// A <c>List|String</c> value: one entry per line, stored as an indexed map.
/// </summary>
/// <remarks>
/// <para>
/// The read and write shapes of a Fcitx list are deliberately different, and this type exists
/// because of that asymmetry. The Host writes a vector as numbered keys, so a non-empty list
/// arrives as <c>{"0":"…","1":"…"}</c> and an empty one arrives as the empty <b>scalar</b>
/// <c>""</c> (a <c>RawConfig</c> with no sub-items is serialised as its own value). On write
/// the Host's <c>merge_schema_values</c> requires a <c>List|</c> option to be a <b>map</b> and
/// answers "A list-valued Fcitx option has an invalid value" to anything else — including the
/// empty scalar it just produced. An empty list must therefore be written as an empty
/// <b>map</b>.
/// </para>
/// <para>
/// The three empty-looking values are not the same thing and are never collapsed here:
/// </para>
/// <list type="bullet">
/// <item>an empty scalar is what a scalar option holds, and what a list reads back as;</item>
/// <item>an empty map is an empty list, which is what this type writes for "no entries";</item>
/// <item>a map whose entry <c>i</c> is the empty string is a list with an empty positional
/// entry, which is preserved as its own position.</item>
/// </list>
/// <para>
/// A control that edits the list as free text cannot express the difference between "no
/// entries" and "one empty entry": empty text is the empty list. That is a limit of the text
/// representation, not a reinterpretation of every empty string — scalar options keep going
/// through <see cref="SettingsValue.Text"/> and are untouched.
/// </para>
/// </remarks>
public static class StringListValue
{
    private static readonly string[] LineBreaks = { "\r\n", "\r", "\n" };

    /// <summary>
    /// The entries of a value that is already a list, in index order. A non-empty scalar is
    /// split the way Fcitx's own list reader would, so a value written by an older client is
    /// not silently shown as empty.
    /// </summary>
    public static IReadOnlyList<string> Lines(SettingsValue value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.IsMap)
        {
            return value.Children
                .OrderBy(entry => int.TryParse(entry.Key, NumberStyles.Integer,
                                               CultureInfo.InvariantCulture, out var index)
                    ? index
                    : int.MaxValue)
                .Select(entry => entry.Value.Scalar)
                .ToArray();
        }

        if (value.Scalar.Trim().Length == 0)
        {
            return Array.Empty<string>();
        }

        return value.Scalar.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
    }

    /// <summary>
    /// The value to submit for a list of lines: always an indexed map, never a scalar.
    /// </summary>
    public static SettingsValue ToValue(IEnumerable<string> lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        var kept = lines.Select(line => line.TrimEnd('\r')).ToList();

        // Empty text is the empty list. It is written as an empty map, which is the only
        // shape the Host accepts for a List| option.
        if (kept.Count == 0 || (kept.Count == 1 && kept[0].Length == 0))
        {
            return SettingsValue.EmptyMap();
        }

        var entries = new (string, SettingsValue)[kept.Count];
        for (var i = 0; i < kept.Count; i++)
        {
            entries[i] = (i.ToString(CultureInfo.InvariantCulture), SettingsValue.Text(kept[i]));
        }

        return SettingsValue.Map(entries);
    }

    /// <summary>
    /// The lines of the text a control holds.
    /// </summary>
    /// <remarks>
    /// All three line breaks are accepted because a WinUI text box does not give back what it was
    /// given: text set with CRLF is read back with CR only. Splitting on <c>\n</c> alone then
    /// found no line break at all and wrote four patterns as one entry containing a literal
    /// <c>\r</c> — measured, not assumed.
    /// </remarks>
    public static string[] SplitLines(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return Array.Empty<string>();
        }

        return text.Split(LineBreaks, StringSplitOptions.None);
    }

    /// <summary>
    /// The text a multi-line control shows for <paramref name="value"/>.
    /// </summary>
    /// <remarks>
    /// CRLF, not LF: a WinUI text box keeps line breaks as <c>\r</c>, and text joined with a bare
    /// <c>\n</c> collapses into one line — three patterns arrived as the first one followed by
    /// nothing. <see cref="SplitLines"/> reads back either form, so the round trip is exact.
    /// </remarks>
    public static string ToText(SettingsValue value) => string.Join("\r\n", Lines(value));
}
