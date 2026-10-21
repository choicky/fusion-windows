using System;
using System.Collections.Generic;

namespace Fusion.Settings.Model;

/// <summary>
/// The key-editor state machine: how many tags are visible, when removal controls
/// appear, and what a recording session does to the value.
/// </summary>
/// <remarks>
/// The editor never invents key semantics. It shows the official list, records one
/// complete chord per tag, and treats a cancelled or unrecognised recording as "no
/// change" rather than as a reason to write an empty binding.
/// </remarks>
public sealed class KeyListEditor
{
    /// <summary>Tags shown before "N more" is offered.</summary>
    public const int InitialVisible = 3;

    public KeyListEditor(KeyBindingList bindings)
    {
        Bindings = bindings ?? throw new ArgumentNullException(nameof(bindings));
    }

    public KeyBindingList Bindings { get; }

    /// <summary>True once the user asked to see every binding.</summary>
    public bool IsExpanded { get; private set; }

    /// <summary>Removal controls are revealed only while editing.</summary>
    public bool IsEditing { get; private set; }

    /// <summary>True while a chord is being recorded.</summary>
    public bool IsRecording { get; private set; }

    /// <summary>Index being replaced by the current recording, or -1 for a new binding.</summary>
    public int RecordingIndex { get; private set; } = -1;

    public int VisibleCount => IsExpanded ? Bindings.Count : Math.Min(InitialVisible, Bindings.Count);

    public int HiddenCount => Bindings.Count - VisibleCount;

    /// <summary>The label the UI shows next to the expand affordance.</summary>
    public string MoreLabel =>
        HiddenCount <= 0 ? string.Empty : $"{HiddenCount} more";

    public void Expand() => IsExpanded = true;

    public void Collapse() => IsExpanded = false;

    public void BeginEditing() => IsEditing = true;

    public void EndEditing()
    {
        IsEditing = false;
        CancelRecording();
    }

    /// <summary>Starts recording a replacement for <paramref name="index"/>, or a new binding.</summary>
    public void BeginRecording(int index = -1)
    {
        if (index < -1 || index >= Bindings.Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }

        IsEditing = true;
        IsRecording = true;
        RecordingIndex = index;
    }

    /// <summary>
    /// Escape during recording. The value is untouched: cancelling is not a deletion.
    /// </summary>
    public void CancelRecording()
    {
        IsRecording = false;
        RecordingIndex = -1;
    }

    /// <summary>
    /// Applies a recorded chord. Returns false and changes nothing when the recording
    /// failed, produced no key, or produced text that is not a chord — the existing
    /// binding is preserved in every one of those cases.
    /// </summary>
    public bool ApplyRecorded(KeyChord? chord)
    {
        if (!IsRecording)
        {
            return false;
        }

        if (chord is null || chord.IsEmpty)
        {
            CancelRecording();
            return false;
        }

        if (RecordingIndex >= 0 && RecordingIndex < Bindings.Count)
        {
            var updated = Bindings.Clone();
            if (!updated.RemoveAt(RecordingIndex))
            {
                CancelRecording();
                return false;
            }

            updated.Add(chord);
            if (!updated.Move(updated.Count - 1, RecordingIndex))
            {
                CancelRecording();
                return false;
            }

            Replace(updated);
        }
        else
        {
            Bindings.Add(chord);
            // A newly added binding is always revealed.
            IsExpanded = true;
        }

        CancelRecording();
        return true;
    }

    /// <summary>Appends a binding and reveals it.</summary>
    public bool AddBinding(KeyChord chord)
    {
        if (chord is null || chord.IsEmpty)
        {
            return false;
        }

        Bindings.Add(chord);
        IsExpanded = true;
        return true;
    }

    public bool RemoveBinding(int index)
    {
        if (!IsEditing)
        {
            return false;
        }

        return Bindings.RemoveAt(index);
    }

    /// <summary>
    /// Applies a preset. A preset edits the same official list; it is not a separate
    /// boolean, and the caller must tell the user whether it replaces or extends the
    /// current bindings before applying it.
    /// </summary>
    public void ApplyPreset(IEnumerable<KeyChord> chords, bool replaceExisting)
    {
        ArgumentNullException.ThrowIfNull(chords);
        if (replaceExisting)
        {
            var replacement = KeyBindingList.Empty(Bindings.IsPositional);
            foreach (var chord in chords)
            {
                replacement.Add(chord);
            }

            Replace(replacement);
        }
        else
        {
            foreach (var chord in chords)
            {
                if (!chord.IsEmpty && !Bindings.Contains(chord))
                {
                    Bindings.Add(chord);
                }
            }
        }

        IsExpanded = true;
    }

    public IReadOnlyList<int> Duplicates => Bindings.DuplicateIndices();

    /// <summary>True when a binding is a bare modifier, which Windows does not deliver.</summary>
    public bool HasModifierOnlyBinding
    {
        get
        {
            foreach (var chord in Bindings.Items)
            {
                if (chord.IsModifierOnly)
                {
                    return true;
                }
            }

            return false;
        }
    }

    private void Replace(KeyBindingList replacement)
    {
        Bindings.Clear();
        foreach (var chord in replacement.Items)
        {
            Bindings.Add(chord);
        }
    }
}
