using System;
using System.Collections.Generic;
using System.Linq;

namespace Fusion.Settings.Model;

/// <summary>
/// Membership and order of one input-method group, edited with Fcitx's own semantics.
/// </summary>
/// <remarks>
/// <para>
/// <c>Enabled</c> is the ordered Items list and its first entry is the method a bound
/// context follows. <c>Fallback</c> is Fcitx's DefaultIM, which is a different thing and
/// is never written from here. Removing a method from the group removes membership
/// only: it never deletes a definition file or learning data, which is the Host's
/// behaviour and stays so.
/// </para>
/// <para>
/// Editing a method is not activating it. Opening a method's settings does not select
/// that method; activation is a separate, explicit operation the Host guards with its
/// own composition check.
/// </para>
/// </remarks>
public sealed class ImListDraft
{
    private readonly List<string> _enabled;

    public ImListDraft(ImList baseline)
    {
        Baseline = baseline ?? throw new ArgumentNullException(nameof(baseline));
        _enabled = new List<string>(baseline.Enabled);
    }

    /// <summary>
    /// The Host's last confirmed membership. It moves when a fresh read is adopted; the
    /// requested order is held separately, so the two can disagree — and that disagreement is
    /// exactly what <see cref="HasChanges"/> reports.
    /// </summary>
    public ImList Baseline { get; private set; }

    public IReadOnlyList<string> Enabled => _enabled;

    public IReadOnlyList<string> Available => Baseline.Available;

    /// <summary>Items/0, or an empty string when the group is empty.</summary>
    public string Active => _enabled.Count == 0 ? string.Empty : _enabled[0];

    /// <summary>
    /// The order the Host accepted and is holding until the current input finishes. While it is
    /// set the requested order is neither unsaved nor saved: the Host will apply it by itself,
    /// and nothing is on disk yet.
    /// </summary>
    private List<string>? _awaitingOrder;

    public bool HasAwaitingApply => _awaitingOrder is not null;

    /// <summary>Anything the user has meant that the Host has not written yet.</summary>
    public bool HasUnsettledChanges => HasChanges || HasAwaitingApply;

    /// <summary>
    /// A requested order is a change unless the Host is already holding exactly that order: a
    /// submitted list is not an unsaved edit any more, and saying it were would keep the save
    /// button offering to send a request that is already staged.
    /// </summary>
    public bool HasChanges =>
        !_enabled.SequenceEqual(Baseline.Enabled, StringComparer.Ordinal) && _awaitingOrder is null;

    /// <summary>Records that the Host staged this order; it applies it when the input ends.</summary>
    public void MarkAwaiting() => _awaitingOrder = new List<string>(_enabled);

    public bool IsAvailable(string name) =>
        Baseline.Available.Contains(name, StringComparer.Ordinal);

    public bool IsEnabled(string name) => _enabled.Contains(name, StringComparer.Ordinal);

    /// <summary>
    /// Adds an available method to the end of the group. A name the runtime does not
    /// define is refused here rather than dropped by the Host's setGroup.
    /// </summary>
    public bool Add(string name)
    {
        if (!IsAvailable(name) || IsEnabled(name))
        {
            return false;
        }

        _enabled.Add(name);
        return true;
    }

    /// <summary>
    /// Removes membership. Refuses to empty the group: a group with no input method is
    /// not a state the product offers, and the Host would have to invent one.
    /// </summary>
    public bool Remove(string name)
    {
        if (_enabled.Count <= 1)
        {
            return false;
        }

        return _enabled.Remove(name);
    }

    public bool Move(int from, int to)
    {
        if (from < 0 || from >= _enabled.Count || to < 0 || to >= _enabled.Count || from == to)
        {
            return false;
        }

        var item = _enabled[from];
        _enabled.RemoveAt(from);
        _enabled.Insert(to, item);
        return true;
    }

    public void Revert()
    {
        _awaitingOrder = null;
        _enabled.Clear();
        _enabled.AddRange(Baseline.Enabled);
    }

    /// <summary>
    /// Moves the baseline to a fresh read, and adopts the read's enabled list too when there was
    /// nothing pending to preserve.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The reread is authoritative either way, so it always becomes the baseline — including its
    /// availability, which is what the UI shows as unavailable or fallback.
    /// </para>
    /// <para>
    /// The requested order is a different question, and it is decided from the state <b>before</b>
    /// the baseline moves. A list with pending edits is user intent: it stays, and
    /// <see cref="HasChanges"/> then says whether the reread agrees (false: the request is applied
    /// and there is nothing left to save; true: it is still pending). A list with no pending edits
    /// is not user intent at all — it is just what the Host said last time — so adopting the fresh
    /// order is the only correct outcome. Keeping it instead would present a stale cached order as
    /// a local edit, and the next save would submit a reversal nobody asked for.
    /// </para>
    /// <para>
    /// Neither path replays the mutation. This is the reconciliation for an uncertain write and for
    /// a reconnect; the difference between them is only that an uncertain write always arrives
    /// here with a pending request.
    /// </para>
    /// </remarks>
    public void RefreshBaseline(ImList fresh)
    {
        ArgumentNullException.ThrowIfNull(fresh);

        // Ask the question against the old baseline. After the assignment below, the two lists
        // would be compared with each other and the answer would always be "clean".
        // A staged order is reconciled first: if the Host now shows it, it was applied and the
        // request is done; if it does not, the order is not applied and goes back to being
        // unsaved, so the user can submit it again instead of believing it landed.
        if (_awaitingOrder is not null)
        {
            _awaitingOrder = null;
            Baseline = fresh;
            return;
        }

        var hadPendingEdits = HasChanges;
        Baseline = fresh;

        if (hadPendingEdits)
        {
            return;
        }

        _enabled.Clear();
        _enabled.AddRange(fresh.Enabled);
    }

    /// <summary>The ordered list a save submits.</summary>
    public IReadOnlyList<string> BuildEnabledList() => _enabled.ToArray();

    /// <summary>
    /// Names present in the group that the runtime no longer defines. They are kept in
    /// the list (removing them would be a silent configuration change) and reported so
    /// the UI can show them as unavailable.
    /// </summary>
    public IReadOnlyList<string> UndefinedEntries =>
        _enabled.Where(name => !IsAvailable(name)).ToArray();
}
