using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// One draft over one underlying configuration, shared by every view that reaches it.
/// </summary>
/// <remarks>
/// <para>
/// A draft holds the <b>values</b> — a confirmed baseline and a set of pending edits — and
/// nothing else. The schema belongs to the view that is asking, and is passed in on every
/// call. That split is what keeps the two Pinyin views honest: they share one value
/// baseline (both URIs resolve to one <c>PinyinEngineConfig</c>) but describe it with
/// different schemas, so a draft that stored one view's schema would let the other view
/// edit — and submit — a field it does not display. Adopting a reply from one view
/// therefore cannot replace the other view's schema, because no schema is stored here.
/// </para>
/// <para>
/// The draft stores only <b>pending edits</b> against the confirmed baseline, which makes
/// the rest structural rather than guarded:
/// </para>
/// <list type="bullet">
/// <item>Only changed options are submitted, because the change set is built from the
/// pending map — the Host merges a submitted subtree into the live configuration.</item>
/// <item>A no-change save has an empty change set, so no request is sent at all and no
/// engine setter or learning file is touched.</item>
/// <item>Reconnecting refreshes the baseline and leaves the pending edits alone, so a
/// reconnect can never overwrite unsaved work.</item>
/// <item>An option the requesting view's schema does not describe cannot be edited here,
/// so a view never writes a field it does not show.</item>
/// </list>
/// </remarks>
public sealed class SettingsDraft
{
    private readonly Dictionary<string, SettingsValue> _pending = new(StringComparer.Ordinal);

    public SettingsDraft(ConfigIdentity identity, SettingsValue baselineValues)
    {
        Identity = identity ?? throw new ArgumentNullException(nameof(identity));
        BaselineValues = baselineValues ?? throw new ArgumentNullException(nameof(baselineValues));
    }

    /// <summary>Builds a draft from a reply, keeping the reply's values and discarding its schema.</summary>
    public static SettingsDraft FromSnapshot(ConfigIdentity identity, SettingsSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        return new SettingsDraft(identity, snapshot.Values);
    }

    public ConfigIdentity Identity { get; }

    /// <summary>
    /// The Host's last confirmed values. Never modified locally, and never carries a
    /// schema: the two views over one configuration keep their own.
    /// </summary>
    public SettingsValue BaselineValues { get; private set; }

    /// <summary>Increments on every accepted edit, so a stale reply can be detected.</summary>
    public long Revision { get; private set; }

    public IReadOnlyCollection<string> PendingPaths => _pending.Keys;

    public bool HasChanges => _pending.Count > 0;

    /// <summary>
    /// The value a view should show: the pending edit when there is one, otherwise the
    /// confirmed value. A missing value reads as an empty scalar, which is not the same
    /// thing as the option's default and is never treated as one.
    /// </summary>
    public SettingsValue Effective(string optionName)
    {
        if (_pending.TryGetValue(optionName, out var pending))
        {
            return pending;
        }

        return BaselineValues.TryGet(optionName, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);
    }

    /// <summary>
    /// Edits one top-level option through <paramref name="view"/>. Returns false when the
    /// option is not part of <b>that view's</b> schema: the client must not write a field
    /// the view does not describe, even when a sibling view describes it.
    /// </summary>
    public bool TrySet(SettingsSnapshot view, string optionName, SettingsValue value)
    {
        ArgumentNullException.ThrowIfNull(view);
        ArgumentNullException.ThrowIfNull(value);
        if (view.FindRootOption(optionName) is null)
        {
            return false;
        }

        // Compare against the confirmed baseline, not the effective value: setting an
        // edited option back to what the Host already has is not a change, even though
        // it differs from the pending edit it replaces.
        if (value.Equals(Confirmed(optionName)))
        {
            _pending.Remove(optionName);
        }
        else
        {
            _pending[optionName] = value;
        }

        Revision++;
        return true;
    }

    /// <summary>
    /// Edits a leaf inside a nested configuration, addressed by the nested type's name
    /// (e.g. <c>Fuzzy$FuzzyConfig</c>) as <paramref name="view"/> describes it. The stored
    /// path uses the <b>value tree</b> key of the owning option (<c>Fuzzy/VE_UE</c>),
    /// which is what the Host merges.
    /// </summary>
    public bool TrySetNested(SettingsSnapshot view, string nestedTypeName, string optionName,
                             SettingsValue value)
    {
        ArgumentNullException.ThrowIfNull(view);
        ArgumentNullException.ThrowIfNull(value);
        if (!TryNestedPath(view, nestedTypeName, optionName, out var path) ||
            !IsKnownNestedOption(view, nestedTypeName, optionName))
        {
            return false;
        }

        if (value.Equals(ConfirmedNested(view, nestedTypeName, optionName)))
        {
            _pending.Remove(path);
        }
        else
        {
            _pending[path] = value;
        }

        Revision++;
        return true;
    }

    public SettingsValue EffectiveNested(SettingsSnapshot view, string nestedTypeName,
                                         string optionName)
    {
        ArgumentNullException.ThrowIfNull(view);
        if (!TryNestedPath(view, nestedTypeName, optionName, out var path))
        {
            return SettingsValue.Text(string.Empty);
        }

        return _pending.TryGetValue(path, out var pending)
            ? pending
            : ConfirmedNested(view, nestedTypeName, optionName);
    }

    /// <summary>True when <paramref name="view"/> describes this nested option.</summary>
    public static bool IsKnownNestedOption(SettingsSnapshot view, string nestedTypeName,
                                           string optionName)
    {
        ArgumentNullException.ThrowIfNull(view);
        var type = view.Types.FirstOrDefault(t =>
            string.Equals(t.Name, nestedTypeName, StringComparison.Ordinal));
        return type is not null && type.Options.Any(o =>
            string.Equals(o.Name, optionName, StringComparison.Ordinal));
    }

    public void Revert(string optionName) => _pending.Remove(optionName);

    public void RevertAll() => _pending.Clear();

    /// <summary>
    /// The tree a save submits: only the pending edits, with their nesting preserved.
    /// Empty when nothing changed, which is the signal not to send a request at all.
    /// </summary>
    public SettingsValue BuildChangeSet() => BuildChangeSet(null);

    /// <summary>
    /// The same tree, limited to the given paths. A save uses this to split one shared draft
    /// across the URIs whose schemas actually describe each field: the Host refuses a field a
    /// URI does not describe, and both Pinyin views share this draft.
    /// </summary>
    public SettingsValue BuildChangeSet(IReadOnlySet<string>? paths)
    {
        if (_pending.Count == 0)
        {
            return SettingsValue.EmptyMap();
        }

        var changeSet = SettingsValue.EmptyMap();
        foreach (var (path, value) in _pending)
        {
            if (paths is not null && !paths.Contains(path))
            {
                continue;
            }

            var segments = path.Split('/');
            changeSet = changeSet.WithPath(segments, value);
        }

        return changeSet;
    }

    /// <summary>
    /// Adopts the Host's confirmed values after a save. Pending edits that the Host agreed
    /// with are dropped; edits it did not receive stay pending, so a partial success keeps
    /// the remaining work instead of pretending it was saved. The reply's schema is not
    /// kept: each view continues to render through its own.
    /// </summary>
    public void AdoptConfirmed(SettingsValue confirmedValues)
    {
        ArgumentNullException.ThrowIfNull(confirmedValues);
        AdoptConfirmed(new SettingsSnapshot(confirmedValues, Array.Empty<ConfigType>()));
    }

    /// <inheritdoc cref="AdoptConfirmed(SettingsValue)"/>
    public void AdoptConfirmed(SettingsSnapshot confirmed) => AdoptConfirmed(confirmed, null);

    /// <summary>
    /// Adopts a reply, dropping the edits it confirms.
    /// </summary>
    /// <param name="appliedPaths">
    /// The paths the Host accepted, when the caller knows them from a successful write. They are
    /// dropped unconditionally: a successful apply is authoritative, and comparing values would
    /// miss the cases where the stored form differs from the submitted one — an empty list is
    /// written as an empty map and read back as an empty scalar. Null means "compare instead",
    /// which is what an uncertain outcome needs.
    /// </param>
    public void AdoptConfirmed(SettingsSnapshot confirmed, IReadOnlySet<string>? appliedPaths)
    {
        ArgumentNullException.ThrowIfNull(confirmed);
        BaselineValues = confirmed.Values;

        foreach (var path in _pending.Keys.ToArray())
        {
            var drop = appliedPaths is not null
                ? appliedPaths.Contains(path)
                : ValueAt(confirmed.Values, path) is { } saved && SameStoredValue(saved, _pending[path]);
            if (drop)
            {
                _pending.Remove(path);
            }
        }

        Revision++;
    }

    /// <summary>
    /// Compares a confirmed value with a pending one, allowing for the Host's read/write
    /// asymmetry on lists: an empty list is written as an empty map and read back as the empty
    /// scalar, and those two describe the same stored state.
    /// </summary>
    private static bool SameStoredValue(SettingsValue confirmed, SettingsValue pending) =>
        confirmed.Equals(pending) ||
        (IsEmptyList(confirmed) && IsEmptyList(pending));

    private static bool IsEmptyList(SettingsValue value) =>
        (value.IsMap && value.Children.Count == 0) ||
        (!value.IsMap && value.Scalar.Length == 0);

    /// <summary>
    /// Refreshes the baseline from a fresh read without touching pending edits. Used
    /// after a reconnect or an uncertain write: reread and reconcile, never replay.
    /// </summary>
    public void RefreshBaseline(SettingsValue freshValues)
    {
        ArgumentNullException.ThrowIfNull(freshValues);
        BaselineValues = freshValues;
        Revision++;
    }

    /// <inheritdoc cref="RefreshBaseline(SettingsValue)"/>
    public void RefreshBaseline(SettingsSnapshot fresh)
    {
        ArgumentNullException.ThrowIfNull(fresh);
        RefreshBaseline(fresh.Values);
    }

    /// <summary>
    /// True when a reply produced from <paramref name="sentRevision"/> is still current.
    /// A late reply for an older revision must not overwrite newer edits.
    /// </summary>
    public bool IsCurrent(long sentRevision) => sentRevision == Revision;

    private SettingsValue ConfirmedNested(SettingsSnapshot view, string nestedTypeName,
                                          string optionName)
    {
        if (!TryNestedPath(view, nestedTypeName, optionName, out var path))
        {
            return SettingsValue.Text(string.Empty);
        }

        var segments = path.Split('/');
        return BaselineValues.TryGetPath(segments, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);
    }

    private SettingsValue Confirmed(string optionName) =>
        BaselineValues.TryGet(optionName, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);

    /// <summary>
    /// Maps a nested type plus option name to the value-tree path. The view's root option
    /// whose declared type is the nested type supplies the key, so no second table of
    /// nested container names is needed — and the mapping follows the view that is asking.
    /// </summary>
    private static bool TryNestedPath(SettingsSnapshot view, string nestedTypeName,
                                      string optionName, out string path)
    {
        path = string.Empty;
        var root = view.RootType;
        if (root is null)
        {
            return false;
        }

        foreach (var option in root.Options)
        {
            if (string.Equals(option.Type, nestedTypeName, StringComparison.Ordinal))
            {
                path = option.Name + "/" + optionName;
                return true;
            }
        }

        return false;
    }

    private static SettingsValue? ValueAt(SettingsValue root, string path)
    {
        var segments = path.Split('/');
        return root.TryGetPath(segments, out var value) ? value : null;
    }
}
