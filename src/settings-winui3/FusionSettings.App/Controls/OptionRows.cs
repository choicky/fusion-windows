using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Fusion.Settings.Model;
using Microsoft.UI;
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
                                         string? label = null, string? help = null)
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
        var control = CreateControl(option, current, apply);
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

    private static FrameworkElement CreateControl(OptionDescription option, SettingsValue current,
                                                  Func<SettingsValue, bool> apply)
    {
        if (option.IsExternal)
        {
            return ExternalLink(option);
        }

        if (option.IsKeyList)
        {
            var editor = new KeyEditor(positional: option.Name == "ChooseCharFromPhrase", onChanged: () => { });
            editor.SetValue(KeyBindingList.FromValue(current, option.Name == "ChooseCharFromPhrase"));
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
            "Enum" => EnumControl(option, current, apply),
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

    private static FrameworkElement EnumControl(OptionDescription option, SettingsValue current,
                                                Func<SettingsValue, bool> apply)
    {
        var labels = option.LocalizedEnumLabels;
        var stored = option.EnumLabels;
        var combo = new ComboBox { MinWidth = 200 };

        for (var i = 0; i < labels.Count; i++)
        {
            combo.Items.Add(new ComboBoxItem { Content = labels[i], Tag = i < stored.Count ? stored[i] : labels[i] });
        }

        var index = stored.ToList().IndexOf(current.Scalar);
        if (index < 0)
        {
            // A value the schema does not list (an older config, or a Host that changed an
            // enum): show it rather than silently selecting the first entry.
            combo.Items.Add(new ComboBoxItem { Content = current.Scalar, Tag = current.Scalar });
            index = combo.Items.Count - 1;
        }

        combo.SelectedIndex = index;
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedItem is ComboBoxItem item && item.Tag is string value)
            {
                apply(SettingsValue.Text(value));
            }
        };
        return combo;
    }

    private static FrameworkElement StringListControl(SettingsValue current, Func<SettingsValue, bool> apply)
    {
        var lines = current.IsMap
            ? current.Children.OrderBy(entry => int.TryParse(entry.Key, out var index) ? index : int.MaxValue)
                .Select(entry => entry.Value.Scalar)
            : current.Scalar.Length == 0
                ? Array.Empty<string>()
                : new[] { current.Scalar };

        var box = new TextBox
        {
            Text = string.Join("\n", lines),
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            Height = 90,
            PlaceholderText = "每行一条",
        };

        box.LostFocus += (_, _) => apply(ToIndexedMap(box.Text.Split('\n')));
        return box;
    }

    private static FrameworkElement TextControl(SettingsValue current, Func<SettingsValue, bool> apply)
    {
        var box = new TextBox { Text = current.Scalar, MinWidth = 200 };
        box.LostFocus += (_, _) => apply(SettingsValue.Text(box.Text));
        return box;
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

    /// <summary>Turns lines into the indexed map a Fcitx string list is stored as.</summary>
    public static SettingsValue ToIndexedMap(IEnumerable<string> lines)
    {
        var kept = lines.Select(line => line.TrimEnd('\r')).ToList();
        if (kept.Count == 0 || (kept.Count == 1 && kept[0].Length == 0))
        {
            return SettingsValue.Text(string.Empty);
        }

        var entries = new (string, SettingsValue)[kept.Count];
        for (var i = 0; i < kept.Count; i++)
        {
            entries[i] = (i.ToString(CultureInfo.InvariantCulture), SettingsValue.Text(kept[i]));
        }

        return SettingsValue.Map(entries);
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
