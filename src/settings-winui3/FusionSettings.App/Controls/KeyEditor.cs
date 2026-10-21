using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.Model;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.System;
using Windows.UI;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// Edits one <c>List|Key</c> option as a row of tags.
/// </summary>
/// <remarks>
/// <para>
/// Three tags are shown first with an "N more" affordance; expanding wraps the rest. Removal
/// controls appear only in editing mode, and a newly added binding is always revealed.
/// </para>
/// <para>
/// A recording session is all-or-nothing: Escape cancels, an unrecognised key changes
/// nothing, and the previous binding survives both. A binding this client cannot interpret
/// is shown as it is stored rather than being replaced, so editing a neighbour cannot delete
/// it.
/// </para>
/// </remarks>
public sealed class KeyEditor : UserControl
{
    private const int InitialVisible = 3;

    private readonly bool _positional;
    private readonly Action _onChanged;
    private readonly StackPanel _tagRows = new() { Spacing = 6 };
    private readonly Button _moreButton = new() { Padding = new Thickness(10, 4, 10, 4) };
    private readonly Button _addButton = new() { Content = "添加按键", Padding = new Thickness(10, 4, 10, 4) };
    private readonly Button _editButton = new() { Content = "编辑", Padding = new Thickness(10, 4, 10, 4) };
    private readonly TextBlock _hint = new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        TextWrapping = TextWrapping.Wrap,
    };

    private KeyBindingList _list = KeyBindingList.Empty();
    private bool _layingOut;
    private double _laidOutWidth = -1;
    private bool _expanded;
    private bool _editing;
    private bool _recording;
    private int _recordingIndex = -1;

    public KeyEditor(bool positional, Action onChanged)
    {
        _positional = positional;
        _onChanged = onChanged;

        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Margin = new Thickness(0, 2, 0, 0),
        };
        buttons.Children.Add(_moreButton);
        buttons.Children.Add(_addButton);
        buttons.Children.Add(_editButton);
        buttons.Children.Add(_hint);

        var root = new StackPanel { Spacing = 6 };
        root.Children.Add(_tagRows);
        root.Children.Add(buttons);
        Content = root;

        _moreButton.Click += (_, _) =>
        {
            _expanded = true;
            Render();
        };
        _addButton.Click += (_, _) => BeginRecording(-1);
        _editButton.Click += (_, _) =>
        {
            _editing = !_editing;
            if (!_editing)
            {
                CancelRecording();
            }

            Render();
        };

        // Capture the key wherever focus is inside the editor while recording.
        AddHandler(KeyDownEvent, new KeyEventHandler(OnRecordingKeyDown), handledEventsToo: true);
        SizeChanged += (_, _) => LayoutTags();
        Render();
    }

    /// <summary>The current list. Mutated only through the editor's own operations.</summary>
    public KeyBindingList Value => _list;

    /// <summary>Raised after any accepted change, so the page can push it into the draft.</summary>
    public event EventHandler? ValueChanged;

    public void SetValue(KeyBindingList list)
    {
        _list = list ?? throw new ArgumentNullException(nameof(list));
        _expanded = false;
        CancelRecording();
        Render();
    }

    private void Render()
    {
        _tagRows.Children.Clear();

        if (_list.Count == 0)
        {
            var empty = new TextBlock
            {
                Text = "未设置",
                Opacity = 0.7,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            };
            _tagRows.Children.Add(empty);
        }
        else
        {
            var visible = _expanded ? _list.Count : Math.Min(InitialVisible, _list.Count);
            for (var i = 0; i < visible; i++)
            {
                AddChip(i);
            }

            LayoutTags();
        }

        var hidden = _list.Count - (_expanded ? _list.Count : Math.Min(InitialVisible, _list.Count));
        _moreButton.Content = hidden > 0 ? $"{hidden} 个更多" : string.Empty;
        _moreButton.Visibility = hidden > 0 ? Visibility.Visible : Visibility.Collapsed;

        _addButton.Visibility = _recording ? Visibility.Collapsed : Visibility.Visible;
        _editButton.Content = _editing ? "完成" : "编辑";

        _hint.Text = _recording
            ? "请按下要使用的按键，Esc 取消。"
            : _positional
                ? "第 N 个绑定选择当前候选的第 N 个字，顺序有意义。"
                : string.Empty;
        _hint.Visibility = _hint.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void AddChip(int index)
    {
        var chord = _list[index];
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };

        var label = new TextBlock
        {
            Text = (index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture) + ". " + KeyNames.Label(chord),
            VerticalAlignment = VerticalAlignment.Center,
            FontFamily = new FontFamily("Consolas, Segoe UI"),
        };

        var tip = new ToolTip { Content = KeyNames.Stored(chord) };
        ToolTipService.SetToolTip(label, tip);

        if (chord.IsOpaque)
        {
            // Shown, not hidden: the user should see that something is stored that this
            // client does not understand, and it is preserved untouched.
            label.Opacity = 0.85;
            label.FontStyle = Windows.UI.Text.FontStyle.Italic;
        }

        content.Children.Add(label);

        if (_editing)
        {
            var remove = new Button
            {
                Content = new FontIcon { Glyph = "\uE711", FontSize = 10 },
                Padding = new Thickness(4, 0, 4, 0),
                MinWidth = 20,
                Height = 20,
                Background = new SolidColorBrush(Colors.Transparent),
                BorderThickness = new Thickness(0),
            };
            ToolTipService.SetToolTip(remove, new ToolTip { Content = "移除这个绑定" });
            remove.Click += (_, _) =>
            {
                if (_list.RemoveAt(index))
                {
                    RaiseChanged();
                }
            };
            content.Children.Add(remove);
        }

        var chip = new Border
        {
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(8, 3, 6, 3),
            Background = (Brush)Application.Current.Resources["SubtleFillColorSecondaryBrush"],
            Child = content,
        };

        _tagRows.Children.Add(chip);
    }

    /// <summary>
    /// Reflows the chips into rows that fit the available width. A wrap panel would be
    /// shorter, but laying the rows out here keeps the chips ordinary controls with their
    /// own focus and tooltips.
    /// </summary>
    private void LayoutTags()
    {
        if (_list.Count == 0 || _layingOut)
        {
            return;
        }

        var chips = _tagRows.Children.OfType<Border>().ToList();
        if (chips.Count == 0)
        {
            return;
        }

        var available = Math.Max(240, ActualWidth - 4);
        if (Math.Abs(available - _laidOutWidth) < 8 && _laidOutWidth > 0)
        {
            return;
        }

        // Rebuilding the rows from inside a SizeChanged handler re-enters layout; the guard
        // above and this flag keep it to one pass per real width change.
        _layingOut = true;
        _laidOutWidth = available;
        _tagRows.Children.Clear();

        StackPanel? row = null;
        double used = 0;
        foreach (var chip in chips)
        {
            chip.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var width = chip.DesiredSize.Width + 6;
            if (row is null || (used + width > available && row.Children.Count > 0))
            {
                row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                _tagRows.Children.Add(row);
                used = 0;
            }

            row.Children.Add(chip);
            used += width;
        }

        _layingOut = false;
    }

    private void BeginRecording(int index)
    {
        _recording = true;
        _recordingIndex = index;
        _editing = true;
        _addButton.Visibility = Visibility.Collapsed;
        Render();
        Focus(FocusState.Programmatic);
    }

    private void CancelRecording()
    {
        _recording = false;
        _recordingIndex = -1;
        _hint.Text = string.Empty;
        _hint.Visibility = Visibility.Collapsed;
    }

    private void OnRecordingKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (!_recording)
        {
            return;
        }

        if (e.Key == VirtualKey.Escape)
        {
            // Cancel is not a deletion: the list is untouched.
            e.Handled = true;
            CancelRecording();
            Render();
            return;
        }

        var control = IsDown(VirtualKey.Control);
        var alt = IsDown(VirtualKey.Menu);
        var shift = IsDown(VirtualKey.Shift);
        var windows = IsDown((VirtualKey)0x5B) || IsDown((VirtualKey)0x5C);

        var chord = KeyNames.FromKey(e.Key, control, alt, shift, windows);
        if (chord is null)
        {
            // A modifier on its own, or a key this client cannot name: keep recording and
            // change nothing, rather than writing an empty binding.
            e.Handled = true;
            _hint.Text = "这个按键无法作为绑定，请再按一次，Esc 取消。";
            _hint.Visibility = Visibility.Visible;
            return;
        }

        e.Handled = true;
        Apply(chord);
    }

    private static bool IsDown(VirtualKey key) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(key)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    private void Apply(KeyChord chord)
    {
        var updated = _list.Clone();
        if (_recordingIndex >= 0 && _recordingIndex < updated.Count)
        {
            if (!updated.RemoveAt(_recordingIndex))
            {
                CancelRecording();
                Render();
                return;
            }

            updated.Add(chord);
            updated.Move(updated.Count - 1, _recordingIndex);
        }
        else
        {
            updated.Add(chord);
            _expanded = true;
        }

        _list = updated;
        CancelRecording();
        RaiseChanged();
    }

    private void RaiseChanged()
    {
        Render();
        ValueChanged?.Invoke(this, EventArgs.Empty);
        _onChanged();
    }

    /// <summary>
    /// Applies a preset to this same official list. Replacing is explicit: the caller says
    /// whether existing bindings are dropped, and the UI states that before applying.
    /// </summary>
    public void ApplyPreset(IEnumerable<KeyChord> chords, bool replaceExisting)
    {
        var updated = replaceExisting ? KeyBindingList.Empty(_positional) : _list.Clone();
        foreach (var chord in chords)
        {
            if (chord.IsEmpty)
            {
                continue;
            }

            if (replaceExisting || !updated.Contains(chord))
            {
                updated.Add(chord);
            }
        }

        _list = updated;
        _expanded = true;
        RaiseChanged();
    }
}
