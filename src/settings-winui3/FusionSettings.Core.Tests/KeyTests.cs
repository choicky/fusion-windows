using System;
using System.Linq;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

internal static class KeyTests
{
    public static void Run()
    {
        Chords();
        Lists();
        Editor();
        Preservation();
    }

    private static void Chords()
    {
        Harness.Section("key chord: one chord is one tag");

        var chord = KeyChord.Parse("Control+Return");
        Harness.Check(chord is not null, "a modifier chord parses");
        Harness.CheckEqual(1, chord!.Modifiers.Count, "one modifier");
        Harness.CheckEqual("Return", chord.KeyName, "the key is the last segment");
        Harness.CheckEqual("Control+Return", chord.ToString(), "and it round trips");

        var reordered = KeyChord.Parse("Shift+Control+P");
        Harness.CheckEqual("Control+Shift+P", reordered!.ToString(),
            "modifiers are written in Fcitx's order, not the order they were typed");

        Harness.CheckEqual("Return", KeyChord.Parse("Return")!.ToString(), "a bare key");
        Harness.CheckEqual("KP_Enter", KeyChord.Parse("KP_Enter")!.ToString(), "a keypad key");
        Harness.CheckEqual("bracketleft", KeyChord.Parse("bracketleft")!.ToString(),
            "the wire name for '[' is bracketleft");
        Harness.CheckEqual("Super+space", KeyChord.Parse("Super+space")!.ToString(), "Super is a modifier");
        Harness.CheckEqual("Control+Alt+P", KeyChord.Parse("Control+Alt+P")!.ToString(),
            "two modifiers keep their order");

        Harness.Check(KeyChord.Parse(string.Empty)!.IsEmpty, "an empty string is the empty chord");
        Harness.Check(KeyChord.Parse(null) is null, "null is not a chord");
        Harness.Check(KeyChord.Parse("Foo+Bar") is null, "an unknown modifier chain is refused");
        Harness.Check(KeyChord.Parse("Control+") is null, "a trailing plus is refused");
        Harness.Check(KeyChord.Parse("+P") is null, "a leading plus is refused");

        Harness.Check(KeyChord.Parse("Shift_L")!.IsModifierOnly, "a bare modifier is recognised");
        Harness.Check(!KeyChord.Parse("Shift+Tab")!.IsModifierOnly, "Shift+Tab is not modifier-only");

        Harness.CheckEqual(KeyChord.Parse("Control+space"), KeyChord.Parse("Control+space"),
            "equal chords compare equal");
        Harness.Check(!KeyChord.Parse("Control+space")!.Equals(KeyChord.Parse("Control+Space")),
            "key names stay case-sensitive");
    }

    private static void Lists()
    {
        Harness.Section("key list: the wire shape is an indexed map, not a string");

        var fromHost = TestSchema.KeyList("minus", "Up", "KP_Up", "Page_Up");
        var list = KeyBindingList.FromValue(fromHost);
        Harness.CheckEqual(4, list.Count, "all four bindings are read");
        Harness.CheckEqual("minus", list[0].ToString(), "order is preserved");
        Harness.CheckEqual("Page_Up", list[3].ToString(), "the last binding is the last one");

        var empty = KeyBindingList.FromValue(SettingsValue.Text(string.Empty));
        Harness.CheckEqual(0, empty.Count, "an empty scalar is the empty list");

        // Read and write are deliberately different. The Host returns an empty list as an empty
        // scalar, but its write path rejects a non-map for a List| option ("A list-valued Fcitx
        // option has an invalid value"), so clearing the last binding only works with a map.
        // This assertion used to encode the read shape as the write shape, which is why the
        // defect survived: it asserted the client agreed with itself, not with the Host.
        var emptyWrite = empty.ToValue();
        Harness.Check(emptyWrite.IsMap, "an empty list is written as a map, not a scalar");
        Harness.CheckEqual(0, emptyWrite.Children.Count, "and the map is empty");

        var roundTrip = KeyBindingList.FromValue(list.ToValue());
        Harness.CheckEqual(list.Count, roundTrip.Count, "the list survives a write/read round trip");
        Harness.CheckEqual("KP_Up", roundTrip[2].ToString(), "with the same order");

        Harness.Check(list.ToValue().IsMap, "a non-empty list writes as a map");
        Harness.CheckEqual("minus", list.ToValue().Children["0"].Scalar, "indices start at 0");

        Harness.Section("key list: duplicates are reported, never rewritten");
        var duplicated = KeyBindingList.FromValue(TestSchema.KeyList("Tab", "Tab", "space"));
        Harness.CheckEqual(3, duplicated.Count, "both duplicates are kept");
        var duplicates = duplicated.DuplicateIndices();
        Harness.CheckEqual(1, duplicates.Count, "one duplicate is reported");
        Harness.CheckEqual(1, duplicates[0], "at the second position");
        Harness.CheckEqual(3, duplicated.Count, "reporting did not remove anything");

        Harness.Section("key list: overlap is a warning, not a merge");
        var left = KeyBindingList.FromValue(TestSchema.KeyList("Tab", "space"));
        var right = KeyBindingList.FromValue(TestSchema.KeyList("Tab", "Return"));
        Harness.Check(left.Overlaps(right), "the overlap is detected");
        Harness.CheckEqual(2, left.Count, "the left list is unchanged");
        Harness.CheckEqual(2, right.Count, "the right list is unchanged");

        Harness.Section("key list: positional lists keep position");
        var positional = KeyBindingList.FromValue(
            TestSchema.KeyList("a", "b", "c"), isPositional: true);
        Harness.Check(positional.IsPositional, "ChooseCharFromPhrase is positional");
        Harness.CheckEqual("a", positional[0].ToString(), "position 0 means the first character");
        positional.Move(0, 2);
        Harness.CheckEqual("b", positional[0].ToString(), "a move reorders, it does not sort");
        Harness.CheckEqual("a", positional[2].ToString(), "the moved binding landed at its position");
    }

    private static void Editor()
    {
        Harness.Section("key editor: three tags then 'N more'");
        var editor = new KeyListEditor(KeyBindingList.FromValue(
            TestSchema.KeyList("minus", "Up", "KP_Up", "Page_Up", "Left")));
        Harness.CheckEqual(3, editor.VisibleCount, "three bindings are visible initially");
        Harness.CheckEqual(2, editor.HiddenCount, "the rest are collapsed");
        Harness.CheckEqual("2 more", editor.MoreLabel, "and the label says how many");
        editor.Expand();
        Harness.CheckEqual(5, editor.VisibleCount, "expanding shows every binding");
        Harness.CheckEqual(string.Empty, editor.MoreLabel, "and the affordance disappears");

        Harness.Section("key editor: recording a replacement");
        var editing = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("minus", "Up")));
        editing.BeginRecording(0);
        Harness.Check(editing.IsRecording, "recording starts");
        Harness.CheckEqual(0, editing.RecordingIndex, "the index being replaced is known");
        Harness.Check(editing.ApplyRecorded(KeyChord.Parse("Page_Up")), "a valid recording applies");
        Harness.CheckEqual("Page_Up", editing.Bindings[0].ToString(), "the binding was replaced in place");
        Harness.CheckEqual("Up", editing.Bindings[1].ToString(), "its neighbour is untouched");
        Harness.Check(!editing.IsRecording, "recording ended");

        Harness.Section("key editor: Escape cancels without changing the value");
        var cancelling = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("minus", "Up")));
        cancelling.BeginRecording(0);
        cancelling.CancelRecording();
        Harness.CheckEqual("minus", cancelling.Bindings[0].ToString(), "the binding is unchanged");
        Harness.Check(!cancelling.IsRecording, "recording ended");
        Harness.Check(!cancelling.ApplyRecorded(KeyChord.Parse("Tab")),
            "a chord arriving after the cancel does not apply");

        Harness.Section("key editor: a failed recording preserves the existing value");
        var failing = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("minus")));
        failing.BeginRecording(0);
        Harness.Check(!failing.ApplyRecorded(null), "a failed recording is rejected");
        Harness.CheckEqual("minus", failing.Bindings[0].ToString(), "the existing binding survives");
        failing.BeginRecording(0);
        Harness.Check(!failing.ApplyRecorded(KeyChord.Empty), "an empty chord is rejected");
        Harness.CheckEqual("minus", failing.Bindings[0].ToString(),
            "and the binding is not replaced with Empty");

        Harness.Section("key editor: a new binding is always revealed");
        var adding = new KeyListEditor(KeyBindingList.FromValue(
            TestSchema.KeyList("a", "b", "c")));
        Harness.CheckEqual(0, adding.HiddenCount, "nothing hidden yet");
        Harness.Check(adding.AddBinding(KeyChord.Parse("d")!), "a binding is added");
        Harness.CheckEqual(4, adding.VisibleCount, "the new binding is visible");
        Harness.CheckEqual("d", adding.Bindings[3].ToString(), "and it is the last one");

        Harness.Section("key editor: removal only while editing");
        var removing = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("a", "b")));
        Harness.Check(!removing.RemoveBinding(0), "removal is refused outside editing");
        removing.BeginEditing();
        Harness.Check(removing.RemoveBinding(0), "removal works while editing");
        Harness.CheckEqual(1, removing.Bindings.Count, "the list shrank");

        Harness.Section("key editor: presets edit the same official list");
        var preset = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("minus")));
        preset.ApplyPreset(new[] { KeyChord.Parse("comma")!, KeyChord.Parse("period")! },
                           replaceExisting: true);
        Harness.CheckEqual(2, preset.Bindings.Count, "replace drops the previous binding");
        Harness.CheckEqual("comma", preset.Bindings[0].ToString(), "the first preset binding is first");
        preset.ApplyPreset(new[] { KeyChord.Parse("comma")!, KeyChord.Parse("space")! },
                           replaceExisting: false);
        Harness.CheckEqual(3, preset.Bindings.Count, "extend keeps existing bindings and skips duplicates");

        Harness.Section("key editor: modifier-only bindings are flagged");
        var modifierOnly = new KeyListEditor(KeyBindingList.FromValue(TestSchema.KeyList("Shift_L")));
        Harness.Check(modifierOnly.HasModifierOnlyBinding, "a bare modifier is reported");
    }

    /// <summary>
    /// A binding this client cannot interpret must survive load, edit and save. Replacing
    /// it with an empty chord would silently delete it the next time anything is saved.
    /// </summary>
    private static void Preservation()
    {
        Harness.Section("key list: an unrecognised binding is kept verbatim");
        var list = KeyBindingList.FromValue(TestSchema.KeyList("minus", "Foo+Bar", "Up"));
        Harness.CheckEqual(3, list.Count, "every entry is read, including the odd one");
        Harness.Check(list[1].IsOpaque, "the unparsable entry is marked opaque");
        Harness.CheckEqual("Foo+Bar", list[1].ToString(), "and keeps its original text");
        Harness.Check(!list[1].IsEmpty, "an opaque entry is not an empty binding");
        Harness.Check(KeyChord.Parse("Foo+Bar") is null, "it really is unparsable");

        var roundTripped = KeyBindingList.FromValue(list.ToValue());
        Harness.CheckEqual(3, roundTripped.Count, "a load/save round trip keeps the count");
        Harness.CheckEqual("Foo+Bar", roundTripped[1].ToString(), "and the unknown text");
        Harness.CheckEqual("Up", roundTripped[2].ToString(), "and the order around it");

        Harness.Section("key list: editing a neighbour leaves the unknown entry alone");
        var editor = new KeyListEditor(list);
        editor.BeginRecording(0);
        Harness.Check(editor.ApplyRecorded(KeyChord.Parse("Page_Up")), "the neighbour is replaced");
        Harness.CheckEqual(3, editor.Bindings.Count, "the list length is unchanged");
        Harness.CheckEqual("Page_Up", editor.Bindings[0].ToString(), "the edited binding changed");
        Harness.CheckEqual("Foo+Bar", editor.Bindings[1].ToString(), "the unknown binding is untouched");
        Harness.CheckEqual("Up", editor.Bindings[2].ToString(), "and stays in its position");

        Harness.Section("key list: an empty entry survives a copy and a replacement");
        var withEmpty = KeyBindingList.FromValue(TestSchema.KeyList("minus", string.Empty, "Up"));
        Harness.CheckEqual(3, withEmpty.Count, "the empty entry is read as an entry");
        Harness.Check(withEmpty[1].IsEmpty, "and is recognised as empty");

        var copied = withEmpty.Clone();
        Harness.CheckEqual(3, copied.Count, "a clone keeps every position");
        Harness.Check(copied[1].IsEmpty, "including the empty one");

        var emptyEditor = new KeyListEditor(withEmpty);
        emptyEditor.BeginRecording(2);
        Harness.Check(emptyEditor.ApplyRecorded(KeyChord.Parse("Tab")), "a later binding is replaced");
        Harness.CheckEqual(3, emptyEditor.Bindings.Count, "the replacement keeps the list length");
        Harness.Check(emptyEditor.Bindings[1].IsEmpty, "the empty entry is not silently dropped");
        Harness.CheckEqual("Tab", emptyEditor.Bindings[2].ToString(), "the replaced binding is in place");

        Harness.Section("key list: positional order is preserved through an edit");
        var positional = KeyBindingList.FromValue(
            TestSchema.KeyList("a", "Foo+Bar", "c"), isPositional: true);
        var positionalEditor = new KeyListEditor(positional);
        positionalEditor.BeginRecording(0);
        positionalEditor.ApplyRecorded(KeyChord.Parse("b"));
        Harness.CheckEqual("b", positionalEditor.Bindings[0].ToString(), "position 0 was replaced");
        Harness.CheckEqual("Foo+Bar", positionalEditor.Bindings[1].ToString(),
            "position 1 still selects the second character");
        Harness.CheckEqual("c", positionalEditor.Bindings[2].ToString(), "position 2 is unchanged");

        Harness.Section("key list: a cancelled or failed recording changes nothing");
        var untouched = KeyBindingList.FromValue(TestSchema.KeyList("minus", "Foo+Bar"));
        var cancelling = new KeyListEditor(untouched);
        cancelling.BeginRecording(0);
        cancelling.CancelRecording();
        Harness.CheckEqual(2, cancelling.Bindings.Count, "the list is intact after Escape");
        Harness.CheckEqual("minus", cancelling.Bindings[0].ToString(), "the first binding is unchanged");
        Harness.CheckEqual("Foo+Bar", cancelling.Bindings[1].ToString(), "the unknown one too");

        cancelling.BeginRecording(0);
        Harness.Check(!cancelling.ApplyRecorded(null), "a failed recording is rejected");
        Harness.CheckEqual("minus", cancelling.Bindings[0].ToString(), "the binding survives");
        cancelling.BeginRecording(0);
        Harness.Check(!cancelling.ApplyRecorded(KeyChord.Empty), "an empty chord is rejected");
        Harness.CheckEqual("minus", cancelling.Bindings[0].ToString(), "and is not written over it");
        Harness.CheckEqual("Foo+Bar", cancelling.Bindings[1].ToString(), "the unknown entry survives");

        Harness.Section("key list: a preset still replaces or extends explicitly");
        var preset = new KeyListEditor(KeyBindingList.FromValue(
            TestSchema.KeyList("minus", "Foo+Bar")));
        preset.ApplyPreset(new[] { KeyChord.Parse("comma")! }, replaceExisting: true);
        Harness.CheckEqual(1, preset.Bindings.Count, "replace drops the previous bindings");
        preset.ApplyPreset(new[] { KeyChord.Parse("period")! }, replaceExisting: false);
        Harness.CheckEqual(2, preset.Bindings.Count, "extend keeps them");
        Harness.CheckEqual("comma", preset.Bindings[0].ToString(), "in order");

        Harness.Section("key list: duplicates are still reported, not rewritten");
        var duplicated = KeyBindingList.FromValue(TestSchema.KeyList("Tab", "Foo+Bar", "Tab"));
        Harness.CheckEqual(1, duplicated.DuplicateIndices().Count, "the duplicate is reported");
        Harness.CheckEqual(3, duplicated.Count, "and nothing is removed");
    }
}
