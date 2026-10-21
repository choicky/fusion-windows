using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// Lets a control hand over an edit the draft has not seen yet.
/// </summary>
/// <remarks>
/// <para>
/// A <c>TextBox</c> normally commits on <c>LostFocus</c>. That is fine for tabbing, but the
/// window has two decisions — "is there anything to save?" when closing, and "would a reload
/// discard anything?" — that must not depend on whether focus happened to move. Moving focus is
/// also not guaranteed: the move can fail, and catching that failure says nothing about whether
/// the text was committed.
/// </para>
/// <para>
/// The callback lives in a side table rather than in a dependency property. Two reasons: this is
/// an internal behaviour hook that nothing binds to or styles, and a dependency property whose
/// type is a delegate makes the XAML type system resolve that type, which fails at runtime with
/// "No installed components were detected". A table keyed by the control avoids the XAML type
/// system completely and cannot leak, because the key is held weakly.
/// </para>
/// </remarks>
public static class PendingEdit
{
    private static readonly ConditionalWeakTable<DependencyObject, Entry> Table = new();

    private sealed class Entry
    {
        public Entry(Func<bool> commit) => Commit = commit;

        public Func<bool> Commit { get; }
    }

    /// <summary>Attaches a commit callback. True means nothing is left uncommitted.</summary>
    public static void SetCommit(DependencyObject element, Func<bool> commit)
    {
        ArgumentNullException.ThrowIfNull(element);
        ArgumentNullException.ThrowIfNull(commit);
        Table.AddOrUpdate(element, new Entry(commit));
    }

    public static Func<bool>? GetCommit(DependencyObject element) =>
        element is not null && Table.TryGetValue(element, out var entry) ? entry.Commit : null;

    /// <summary>
    /// Commits every attached control under <paramref name="root"/>. True when all of them
    /// accepted their edit — a refusal (a read-only draft, an operation in flight) is reported
    /// rather than hidden.
    /// </summary>
    public static bool CommitAll(DependencyObject root)
    {
        ArgumentNullException.ThrowIfNull(root);
        var committed = true;
        foreach (var commit in Sinks(root))
        {
            if (!commit())
            {
                committed = false;
            }
        }

        return committed;
    }

    private static IEnumerable<Func<bool>> Sinks(DependencyObject root)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (GetCommit(child) is { } commit)
            {
                yield return commit;
            }

            foreach (var nested in Sinks(child))
            {
                yield return nested;
            }
        }
    }
}
