using System;
using System.Collections.Generic;
using System.Globalization;

namespace Fusion.Settings.Model;

/// <summary>
/// One pinyin dictionary the engine can load, as the Host reports it: the file's stem, which
/// data root it came from, whether the official disable marker is present, and its size.
/// </summary>
/// <remarks>
/// No word count is carried. Nothing on disk reports one without loading the dictionary, and
/// a number this layer computed would not be the engine's, so the page does not show one.
/// </remarks>
public sealed record DictionaryEntry(string Name, string Source, bool Enabled, uint Size)
{
    /// <summary>True when the file lives in the writable per-user root.</summary>
    public bool IsUser => string.Equals(Source, "user", StringComparison.Ordinal);

    public string SourceLabel => IsUser ? "用户词库" : "随包提供";

    public string StateLabel => Enabled ? "已启用" : "已停用";

    public string SizeLabel => Size switch
    {
        0 => "—",
        < 1024 => string.Create(CultureInfo.InvariantCulture, $"{Size} B"),
        < 1024 * 1024 => string.Create(CultureInfo.InvariantCulture, $"{Size / 1024.0:0.0} KB"),
        _ => string.Create(CultureInfo.InvariantCulture, $"{Size / (1024.0 * 1024.0):0.0} MB"),
    };

    /// <summary>Only a user dictionary can be deleted; a packaged one can be disabled.</summary>
    public bool CanDelete => IsUser;
}

/// <summary>The list the Host reports, in the order it reports it (sorted by name).</summary>
public sealed record DictionaryList(IReadOnlyList<DictionaryEntry> Entries)
{
    public static readonly DictionaryList Empty = new(Array.Empty<DictionaryEntry>());
}

/// <summary>
/// The reply to an import: the dictionary that was placed, plus the list it left behind, so
/// the window can refresh from the same reply that reports the result.
/// </summary>
public sealed record DictionaryImport(DictionaryEntry Entry, DictionaryList List);
