using System;
using System.Collections.Generic;

namespace Fusion.Settings.Model;

/// <summary>
/// The identity a draft is keyed by: the configuration object that actually stores the
/// values, not the public URI a view happens to use.
/// </summary>
/// <remarks>
/// <para>
/// Verified against the pinned sources and the running Host (2026-10-21):
/// <c>data/fcitx5/inputmethod/pinyin.conf</c> and <c>shuangpin.conf</c> both declare
/// <c>Addon=pinyin</c>, and a Host returns the <b>same 43-key value tree</b> for
/// <c>fcitx://config/inputmethod/pinyin</c> and <c>fcitx://config/inputmethod/shuangpin</c>
/// while describing them with different schema views (41 vs 43 options). Keying a draft
/// by the public URI would therefore create two independent drafts of one value, and
/// saving the stale one would revert the other.
/// </para>
/// <para>
/// This mapping is deliberately narrow. It is evidence about the pinyin addon, not a
/// universal rule that any two URIs sharing an addon may share a draft; a new entry
/// needs its own evidence.
/// </para>
/// </remarks>
public sealed class ConfigIdentity : IEquatable<ConfigIdentity>
{
    private static readonly Dictionary<string, string> SharedIdentityByUri =
        new(StringComparer.Ordinal)
        {
            // Both public URIs resolve to the single PinyinEngineConfig owned by the
            // pinyin addon. Shuangpin is a profile value inside that configuration.
            ["fcitx://config/inputmethod/pinyin"] = "addon:pinyin",
            ["fcitx://config/inputmethod/shuangpin"] = "addon:pinyin",

            // The global configuration is its own object.
            ["fcitx://config/global"] = "global",
        };

    private ConfigIdentity(string id, string publicUri)
    {
        Id = id;
        PublicUri = publicUri;
    }

    /// <summary>The underlying configuration identity, e.g. <c>addon:pinyin</c>.</summary>
    public string Id { get; }

    /// <summary>The URI this view reads and writes through.</summary>
    public string PublicUri { get; }

    public static ConfigIdentity ForUri(string publicUri)
    {
        ArgumentException.ThrowIfNullOrEmpty(publicUri);
        var id = SharedIdentityByUri.TryGetValue(publicUri, out var shared)
            ? shared
            : "uri:" + publicUri;
        return new ConfigIdentity(id, publicUri);
    }

    /// <summary>
    /// True when this identity shares its underlying values with <paramref name="other"/>.
    /// </summary>
    public bool SharesStorageWith(ConfigIdentity other) =>
        other is not null && string.Equals(Id, other.Id, StringComparison.Ordinal);

    public bool Equals(ConfigIdentity? other) =>
        other is not null && string.Equals(Id, other.Id, StringComparison.Ordinal);

    public override bool Equals(object? obj) => Equals(obj as ConfigIdentity);

    public override int GetHashCode() => StringComparer.Ordinal.GetHashCode(Id);

    public override string ToString() => Id;
}
