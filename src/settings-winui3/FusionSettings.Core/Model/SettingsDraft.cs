using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// One draft over one underlying configuration, shared by every view that reaches it.
/// </summary>
/// <remarks>
/// <para>
/// The draft stores only <b>pending edits</b>, keyed by option path, against a baseline
/// that is the Host's last confirmed state. That shape is what makes the required
/// behaviour fall out rather than need guarding:
/// </para>
/// <list type="bullet">
/// <item>Only changed options are submitted, because the change set is built from the
/// pending map — the Host merges a submitted subtree into the live configuration.</item>
/// <item>A no-change save has an empty change set, so no request is sent at all and no
/// engine setter or learning file is touched.</item>
/// <item>Reconnecting refreshes the baseline and leaves the pending edits alone, so a
/// reconnect can never overwrite unsaved work.</item>
/// <item>An option the view's schema does not describe cannot be edited here, so a view
/// never writes a field it does not show.</item>
/// </list>
/// </remarks>
public sealed class SettingsDraft
{
    private readonly Dictionary<string, SettingsValue> _pending = new(StringComparer.Ordinal);

    public SettingsDraft(ConfigIdentity identity, SettingsSnapshot baseline)
    {
        Identity = identity ?? throw new ArgumentNullException(nameof(identity));
        Baseline = baseline ?? throw new ArgumentNullException(nameof(baseline));
    }

    public ConfigIdentity Identity { get; }

    /// <summary>The Host's last confirmed state. Never modified locally.</summary>
    public SettingsSnapshot Baseline { get; private set; }

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

        return Baseline.Values.TryGet(optionName, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);
    }

    /// <summary>
    /// Edits one top-level option. Returns false when the option is not part of this
    /// view's schema: the client must not write a field the view does not describe.
    /// </summary>
    public bool TrySet(string optionName, SettingsValue value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (Baseline.FindRootOption(optionName) is null)
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
    /// (e.g. <c>Fuzzy$FuzzyConfig</c>). The stored path uses the <b>value tree</b> key of
    /// the owning option (<c>Fuzzy/VE_UE</c>), which is what the Host merges.
    /// </summary>
    public bool TrySetPath(string nestedTypeName, string optionName, SettingsValue value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (!TryNestedPath(nestedTypeName, optionName, out var path) ||
            !IsKnownNestedOption(nestedTypeName, optionName))
        {
            return false;
        }

        if (value.Equals(ConfirmedNested(nestedTypeName, optionName)))
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

    public SettingsValue EffectiveNested(string nestedTypeName, string optionName)
    {
        if (!TryNestedPath(nestedTypeName, optionName, out var path))
        {
            return SettingsValue.Text(string.Empty);
        }

        return _pending.TryGetValue(path, out var pending) ? pending : ConfirmedNested(nestedTypeName, optionName);
    }

    private SettingsValue ConfirmedNested(string nestedTypeName, string optionName)
    {
        if (!TryNestedPath(nestedTypeName, optionName, out var path))
        {
            return SettingsValue.Text(string.Empty);
        }

        var segments = path.Split('/');
        return Baseline.Values.TryGetPath(segments, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);
    }

    private SettingsValue Confirmed(string optionName) =>
        Baseline.Values.TryGet(optionName, out var confirmed)
            ? confirmed
            : SettingsValue.Text(string.Empty);

    /// <summary>
    /// Maps a nested type plus option name to the value-tree path. The root option whose
    /// declared type is the nested type supplies the key, so no second table of nested
    /// container names is needed.
    /// </summary>
    private bool TryNestedPath(string nestedTypeName, string optionName, out string path)
    {
        path = string.Empty;
        var root = Baseline.RootType;
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

    public bool IsKnownNestedOption(string nestedTypeName, string optionName)
    {
        var type = Baseline.Types.FirstOrDefault(t =>
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
    public SettingsValue BuildChangeSet()
    {
        if (_pending.Count == 0)
        {
            return SettingsValue.EmptyMap();
        }

        var changeSet = SettingsValue.EmptyMap();
        foreach (var (path, value) in _pending)
        {
            var segments = path.Split('/');
            changeSet = changeSet.WithPath(segments, value);
        }

        return changeSet;
    }

    /// <summary>
    /// Adopts the Host's confirmed state after a save. Pending edits that the Host
    /// agreed with are dropped; edits it did not receive stay pending, so a partial
    /// success keeps the remaining work instead of pretending it was saved.
    /// </summary>
    public void AdoptConfirmed(SettingsSnapshot confirmed)
    {
        ArgumentNullException.ThrowIfNull(confirmed);
        Baseline = confirmed;

        foreach (var path in _pending.Keys.ToArray())
        {
            if (ValueAt(confirmed.Values, path) is { } saved && saved.Equals(_pending[path]))
            {
                _pending.Remove(path);
            }
        }

        Revision++;
    }

    /// <summary>
    /// Refreshes the baseline from a fresh read without touching pending edits. Used
    /// after a reconnect or an uncertain write: reread and reconcile, never replay.
    /// </summary>
    public void RefreshBaseline(SettingsSnapshot fresh)
    {
        ArgumentNullException.ThrowIfNull(fresh);
        Baseline = fresh;
        Revision++;
    }

    /// <summary>
    /// True when a reply produced from <paramref name="sentRevision"/> is still current.
    /// A late reply for an older revision must not overwrite newer edits.
    /// </summary>
    public bool IsCurrent(long sentRevision) => sentRevision == Revision;

    private static SettingsValue? ValueAt(SettingsValue root, string path)
    {
        var segments = path.Split('/');
        return root.TryGetPath(segments, out var value) ? value : null;
    }
}
