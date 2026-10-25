using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Fusion.Settings.Model;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// Builds one row per official option, choosing the control from the Host's own type
/// string. This is the only place that knows how a Fcitx type maps to a control, so no
/// option's meaning is duplicated anywhere else.
/// </summary>
public static class OptionRows
{
    /// <summary>
    /// A row for <paramref name="option"/>. <paramref name="current"/> is the value to show
    /// (pending edit or confirmed), and <paramref name="apply"/> returns false when the draft
    /// refused the edit — the control then snaps back instead of showing a lie.
    /// </summary>
    public static FrameworkElement Build(OptionDescription option, SettingsValue current,
                                         Func<SettingsValue, bool> apply,
                                         string? label = null, string? help = null,
                                         Func<KeyChord, string?>? overlapProbe = null,
                                         IReadOnlyDictionary<string, string>? enumLabels = null)
    {
        var title = new TextBlock
        {
            Text = label ?? option.Name,
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            TextWrapping = TextWrapping.Wrap,
        };

        // The stored option name stays available: the label is a translation, not a rename.
        ToolTipService.SetToolTip(title, new ToolTip { Content = option.Name });

        var left = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        left.Children.Add(title);

        // The Host's descriptions are upstream English. Once an option has a Chinese label
        // it also has a Chinese explanation or none; repeating the English line underneath
        // reads as two different names for one setting. Unmapped options keep it as a
        // fallback, because there it is the only description there is.
        var description = help ?? (label is null ? option.Description : null);
        if (!string.IsNullOrWhiteSpace(description) &&
            !string.Equals(description, option.Name, StringComparison.Ordinal))
        {
            left.Children.Add(new TextBlock
            {
                Text = description,
                Opacity = 0.7,
                TextWrapping = TextWrapping.Wrap,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
        }

        Services.Trace.Write("option " + option.Name + " type=" + option.Type);
        var control = CreateControl(option, current, apply, overlapProbe, enumLabels);
        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(left);

        // Wide controls (key lists, multi-line text) go underneath the label so they have room.
        if (control is KeyEditor or TextBox { AcceptsReturn: true })
        {
            var stacked = new StackPanel { Spacing = 8 };
            stacked.Children.Add(row);
            stacked.Children.Add(control);
            return stacked;
        }

        Grid.SetColumn(control, 1);
        control.VerticalAlignment = VerticalAlignment.Center;
        if (control is FrameworkElement element)
        {
            element.MinWidth = 160;
        }

        row.Children.Add(control);
        return row;
    }

    /// <summary>
    /// The row for <c>FontFamily</c>: the installed families, searchable, with the stored value
    /// kept verbatim when the system does not have it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The list is the system's, read through DirectWrite, so a name the user picks is a name
    /// the popup can draw. It is a suggestion, not a constraint: the editor is free text, because
    /// the configuration may name a font that this machine does not have — installed elsewhere,
    /// or simply misspelled — and opening this window must not silently replace what the file
    /// says. When the stored value is not installed the row says so, and says what Windows will
    /// do instead, without changing the value.
    /// </para>
    /// <para>
    /// The value is written through the same <paramref name="apply"/> as every other row, so it
    /// lands in the draft like any edit and is stored by the next save.
    /// </para>
    /// </remarks>
    public static FrameworkElement BuildFontFamily(SettingsValue current,
                                                   Func<SettingsValue, bool> apply,
                                                   string? label = null, string? help = null)
    {
        var title = new TextBlock
        {
            Text = label ?? "FontFamily",
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            TextWrapping = TextWrapping.Wrap,
        };
        ToolTipService.SetToolTip(title, new ToolTip { Content = "FontFamily" });

        var left = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        left.Children.Add(title);
        if (!string.IsNullOrWhiteSpace(help))
        {
            left.Children.Add(new TextBlock
            {
                Text = help,
                Opacity = 0.7,
                TextWrapping = TextWrapping.Wrap,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
        }

        var installed = FontCatalog.InstalledFamilies();
        var stored = current.Scalar;

        var box = new AutoSuggestBox
        {
            Text = stored,
            PlaceholderText = installed.Count == 0 ? "输入字体名称" : "搜索或输入字体名称",
            MinWidth = 220,
            // The suggestion list is the system's; typing filters it, and a name that is not
            // there is still accepted as text.
            ItemsSource = FontChoices(installed, stored),
            TextMemberPath = nameof(ChoiceRow.Name),
            ItemTemplate = ChoiceMark.Template,
        };
        box.TextChanged += (sender, args) =>
        {
            if (args.Reason == AutoSuggestionBoxTextChangeReason.UserInput)
            {
                sender.ItemsSource = FontChoices(FontCatalog.Search(sender.Text), sender.Text);
            }
        };
        // Opening the list shows it from its first row, so with the catalogue in its own order
        // the reader starts at "A" while the box says "Microsoft YaHei UI". The list is put
        // back in front of them instead: the value the box holds — the draft's value, which is
        // what an unsaved choice is — comes first, the rest follows in catalogue order. Only
        // the order changes; opening the list commits nothing and rewrites no configuration.
        box.GotFocus += (_, _) => AnchorSuggestionList(box, stored);

        void Commit(string? text)
        {
            var value = (text ?? string.Empty).Trim();
            if (string.Equals(value, stored, StringComparison.Ordinal))
            {
                return;
            }

            // A name the draft refuses is not shown as if it had been taken: the box goes
            // back to what the configuration still holds.
            if (!apply(SettingsValue.Text(value)))
            {
                box.Text = stored;
                return;
            }

            stored = value;
            Note();
        }

        var note = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = (Brush)Application.Current.Resources["SystemFillColorCautionBrush"],
            Visibility = Visibility.Collapsed,
        };

        void Note()
        {
            var value = stored.Trim();
            if (value.Length == 0 || FontCatalog.Find(value) is not null)
            {
                note.Visibility = Visibility.Collapsed;
                return;
            }

            note.Text = $"系统中没有安装“{value}”。配置里的值会保留，候选窗口将回退到系统默认字体。";
            note.Visibility = Visibility.Visible;
        }

        box.SuggestionChosen += (_, args) =>
        {
            // Clicking a suggestion fills the box; committing happens in QuerySubmitted, so a
            // keyboard user and a mouse user take the same path.
            if (args.SelectedItem is ChoiceRow chosen)
            {
                box.Text = chosen.Name;
            }
        };
        box.QuerySubmitted += (_, args) => Commit(args.QueryText);
        box.LostFocus += (_, _) => Commit(box.Text);

        var control = new StackPanel { Spacing = 4, MinWidth = 220 };
        control.Children.Add(box);
        control.Children.Add(note);
        Note();

        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(left);
        Grid.SetColumn(control, 1);
        control.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(control);
        return row;
    }

    /// <summary>
    /// Puts the value the box holds at the top of the suggestion list, so opening the list shows
    /// the font that is in effect instead of the beginning of the alphabet.
    /// </summary>
    /// <remarks>
    /// The list is the same catalogue; only its order changes, and nothing is written: the box's
    /// text, the draft and the configuration are left exactly as they were. A value that names no
    /// installed family is not invented into the list — the row already says Windows will
    /// substitute, and the list keeps its own order.
    /// </remarks>
    private static void AnchorSuggestionList(AutoSuggestBox box, string value)
    {
        var families = FontCatalog.InstalledFamilies();
        var current = FontCatalog.Find(value);
        if (current is null || families.Count == 0)
        {
            box.ItemsSource = FontChoices(families, value);
            return;
        }

        var ordered = new List<string>(families.Count) { current };
        foreach (var family in families)
        {
            if (!string.Equals(family, current, StringComparison.OrdinalIgnoreCase))
            {
                ordered.Add(family);
            }
        }

        box.ItemsSource = FontChoices(ordered, value);
    }

    /// <summary>
    /// The catalogue as rows, with the family that is in effect carrying the mark.
    /// <paramref name="inEffect"/> is the value the box holds — the draft's value, so an
    /// unsaved choice is the one marked — and a name no installed family matches marks nothing.
    /// </summary>
    private static List<ChoiceRow> FontChoices(IReadOnlyList<string> families, string? inEffect)
    {
        var current = FontCatalog.Find(inEffect);
        var rows = new List<ChoiceRow>(families.Count);
        foreach (var family in families)
        {
            var isCurrent = current is not null &&
                            string.Equals(family, current, StringComparison.OrdinalIgnoreCase);
            rows.Add(new ChoiceRow(family, family, isCurrent));
        }

        return rows;
    }

    private static FrameworkElement CreateControl(OptionDescription option, SettingsValue current,
                                                  Func<SettingsValue, bool> apply,
                                                  Func<KeyChord, string?>? overlapProbe,
                                                  IReadOnlyDictionary<string, string>? enumLabels)
    {
        if (option.IsExternal)
        {
            return ExternalLink(option);
        }

        if (option.IsKeyList)
        {
            var editor = new KeyEditor(
                positional: option.Name == "ChooseCharFromPhrase",
                onChanged: () => { },
                overlapProbe: overlapProbe);
            editor.SetValue(KeyBindingList.FromValue(current, option.Name == "ChooseCharFromPhrase"));

            // Reset writes the Host's own default into the draft; no invented default.
            editor.SetDefaultValue(option.DefaultValue);
            editor.ValueChanged += (_, _) => apply(editor.Value.ToValue());
            return editor;
        }

        if (option.IsSingleKey)
        {
            return new SingleKeyPicker(current.Scalar, value => apply(SettingsValue.Text(value)));
        }

        return option.Type switch
        {
            "Boolean" => BooleanControl(current, apply),
            "Integer" => IntegerControl(option, current, apply),
            "Enum" => EnumControl(option, current, apply, enumLabels),
            "List|String" => StringListControl(current, apply),
            "Color" => new TextBlock { Text = current.Scalar, Opacity = 0.7 },
            _ => TextControl(current, apply),
        };
    }

    private static FrameworkElement BooleanControl(SettingsValue current, Func<SettingsValue, bool> apply)
    {
        var toggle = new ToggleSwitch
        {
            IsOn = string.Equals(current.Scalar, "True", StringComparison.OrdinalIgnoreCase),
            OnContent = null,
            OffContent = null,
            MinWidth = 0,
        };
        toggle.Toggled += (_, _) =>
        {
            if (!apply(SettingsValue.Text(toggle.IsOn ? "True" : "False")))
            {
                toggle.IsOn = !toggle.IsOn;
            }
        };
        return toggle;
    }

    private static FrameworkElement IntegerControl(OptionDescription option, SettingsValue current,
                                                   Func<SettingsValue, bool> apply)
    {
        var box = new NumberBox
        {
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            SmallChange = 1,
            LargeChange = 5,
            Value = double.TryParse(current.Scalar, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out var parsed) ? parsed : double.NaN,
        };

        if (option.IntMinimum is { } minimum)
        {
            box.Minimum = minimum;
        }

        if (option.IntMaximum is { } maximum)
        {
            box.Maximum = maximum;
        }

        box.ValueChanged += (_, args) =>
        {
            if (double.IsNaN(args.NewValue))
            {
                return;
            }

            var text = ((int)Math.Round(args.NewValue)).ToString(CultureInfo.InvariantCulture);
            apply(SettingsValue.Text(text));
        };
        return box;
    }

    /// <summary>
    /// A closed set of values. The list marks the value in effect the same way the font list does,
    /// and the mark moves with the selection — the dropdown's own highlight keeps saying where the
    /// pointer or the keyboard is, which is a different question.
    /// </summary>
    private static FrameworkElement EnumControl(OptionDescription option, SettingsValue current,
                                                Func<SettingsValue, bool> apply,
                                                IReadOnlyDictionary<string, string>? enumLabels)
    {
        var labels = option.LocalizedEnumLabels;
        var stored = option.EnumLabels;
        var rows = new List<ChoiceRow>(labels.Count);
        for (var i = 0; i < labels.Count; i++)
        {
            var value = i < stored.Count ? stored[i] : labels[i];
            // The label is display text; the row's Value stays the stored value the Host round-trips.
            var shown = enumLabels is not null && enumLabels.TryGetValue(value, out var mapped)
                            ? mapped
                            : labels[i];
            rows.Add(new ChoiceRow(shown, value));
        }

        var index = stored.ToList().IndexOf(current.Scalar);
        if (index < 0)
        {
            // A value the schema does not list (an older config, or a Host that changed an
            // enum): show it rather than silently selecting the first entry.
            rows.Add(new ChoiceRow(current.Scalar, current.Scalar));
            index = rows.Count - 1;
        }

        var combo = new ComboBox
        {
            MinWidth = 200,
            ItemsSource = rows,
            ItemTemplate = ChoiceMark.TextTemplate,
            // Selection is decided below, once the mark is in place: setting it here would raise
            // SelectionChanged against a list that has no current row yet.
            SelectedIndex = index,
        };
        ChoiceMark.ApplyTo(combo);
        Mark(rows, index);

        var updating = false;
        var marked = index;
        combo.SelectionChanged += (_, _) =>
        {
            if (updating || combo.SelectedIndex < 0 || combo.SelectedIndex >= rows.Count)
            {
                return;
            }

            var chosen = rows[combo.SelectedIndex];
            if (!apply(SettingsValue.Text(chosen.Value ?? string.Empty)))
            {
                // A value the draft refused is not shown as taken, and not marked as if it were.
                updating = true;
                combo.SelectedIndex = marked;
                updating = false;
                return;
            }

            marked = combo.SelectedIndex;
            Mark(rows, marked);
        };
        return combo;
    }

    /// <summary>Moves the mark to <paramref name="index"/>; every other row loses it.</summary>
    private static void Mark(IReadOnlyList<ChoiceRow> rows, int index)
    {
        for (var i = 0; i < rows.Count; i++)
        {
            rows[i].IsCurrent = i == index;
        }
    }

    private static FrameworkElement StringListControl(SettingsValue current, Func<SettingsValue, bool> apply)
    {
        // One entry per line. The value that leaves this control is always an indexed map —
        // empty text included — because that is the only shape the Host accepts for a list
        // option; see StringListValue for why the read shape differs.
        //
        // AcceptsReturn is set *before* Text: a single-line text box strips line breaks out of
        // the text it is given, so assigning the text first left one pattern where there were
        // three, and the next save wrote the list back truncated.
        var box = new TextBox
        {
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            Height = 90,
            PlaceholderText = "每行一条",
        };
        box.Text = StringListValue.ToText(current);

        AttachCommit(box, box.Text, text => apply(StringListValue.ToValue(StringListValue.SplitLines(text))));
        return box;
    }

    private static FrameworkElement TextControl(SettingsValue current, Func<SettingsValue, bool> apply)
    {
        // A scalar option keeps the scalar path: an empty string here is an empty string, not
        // an empty list.
        var box = new TextBox { Text = current.Scalar, MinWidth = 200 };
        AttachCommit(box, current.Scalar, text => apply(SettingsValue.Text(text)));
        return box;
    }

    /// <summary>
    /// Makes a text field commit on lost focus <b>and</b> on demand, once per actual edit.
    /// </summary>
    /// <remarks>
    /// The callback compares the current text with the text the draft last accepted, so the
    /// window can ask at any time — before a save, before deciding whether closing would lose
    /// something — without submitting the same edit twice. A refusal leaves the accepted text
    /// alone, so a later attempt still carries what the user typed.
    /// </remarks>
    private static void AttachCommit(TextBox box, string initial, Func<string, bool> apply)
    {
        var committed = initial ?? string.Empty;
        bool Commit()
        {
            var text = box.Text ?? string.Empty;
            if (string.Equals(text, committed, StringComparison.Ordinal))
            {
                return true;
            }

            if (!apply(text))
            {
                return false;
            }

            committed = text;
            return true;
        }

        box.LostFocus += (_, _) => Commit();
        PendingEdit.SetCommit(box, Commit);
    }

    private static FrameworkElement ExternalLink(OptionDescription option)
    {
        var reason = PresentationMapBridge.ExternalReason(option.Name);
        var button = new Button
        {
            Content = "打开",
            IsEnabled = false,
        };
        ToolTipService.SetToolTip(button, new ToolTip { Content = reason });

        var stack = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        stack.Children.Add(button);
        stack.Children.Add(new TextBlock
        {
            Text = "不可用",
            Opacity = 0.7,
            VerticalAlignment = VerticalAlignment.Center,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        });
        return stack;
    }
}

/// <summary>A single <c>Key</c> option: one button that records one chord.</summary>
public sealed class SingleKeyPicker : UserControl
{
    private readonly Func<string, bool> _apply;
    private readonly Button _button = new() { Padding = new Thickness(10, 4, 10, 4) };
    private string _value;
    private bool _recording;

    public SingleKeyPicker(string value, Func<string, bool> apply)
    {
        _value = value ?? string.Empty;
        _apply = apply;
        Content = _button;
        _button.Click += (_, _) => BeginRecording();
        AddHandler(KeyDownEvent, new KeyEventHandler(OnKeyDown), handledEventsToo: true);
        UpdateLabel();
    }

    private void BeginRecording()
    {
        _recording = true;
        _button.Content = "请按下按键…";
        Focus(FocusState.Programmatic);
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (!_recording)
        {
            return;
        }

        if (e.Key == VirtualKey.Escape)
        {
            e.Handled = true;
            _recording = false;
            UpdateLabel();
            return;
        }

        var control = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var alt = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var shift = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var windows = InputKeyboardSource.GetKeyStateForCurrentThread((VirtualKey)0x5B)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

        var chord = KeyNames.FromKey(e.Key, control, alt, shift, windows);
        if (chord is null)
        {
            e.Handled = true;
            return;
        }

        e.Handled = true;
        var text = chord.ToString();
        if (_apply(text))
        {
            _value = text;
        }

        _recording = false;
        UpdateLabel();
    }

    private void UpdateLabel()
    {
        var chord = KeyChord.Parse(_value);
        _button.Content = chord is null || chord.IsEmpty ? "未设置" : KeyNames.Label(chord);
        ToolTipService.SetToolTip(_button, new ToolTip { Content = _value.Length == 0 ? "未设置" : _value });
    }
}

/// <summary>Keeps the option factory free of a direct dependency on the presentation table.</summary>
internal static class PresentationMapBridge
{
    public static string ExternalReason(string optionName) =>
        Services.PresentationMap.ExternalToolReason.TryGetValue(optionName, out var reason)
            ? reason
            : "该工具尚未在 Windows 上提供。";
}
