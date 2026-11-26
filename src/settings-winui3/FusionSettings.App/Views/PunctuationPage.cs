using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Controls;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App.Views;

/// <summary>
/// The zh_CN punctuation map editor. It edits the Fcitx addon's own Entries list through the
/// shared draft and the ordinary save bar; no copy of the map, its defaults or its storage path
/// lives in this application.
/// </summary>
/// <remarks>
/// <para>
/// Every row is kept as a row. A key may appear more than once, the row order is candidate
/// order, and AltMapping is the second half of a paired entry, so a dictionary or a sort would
/// silently destroy the configuration. New rows are appended; up/down move a row by swapping
/// the same ordered list.
/// </para>
/// <para>
/// The table lives in the workspace, not in this page. That is what preserves an incomplete
/// row across navigation and refresh, and what makes the visible table — rather than an older
/// valid draft — the thing Save validates.
/// </para>
/// </remarks>
public sealed class PunctuationPage : SettingsPageBase
{
    /// <summary>
    /// One row's identity for as long as the controls built from it exist.
    /// </summary>
    /// <remarks>
    /// A record is a value: editing a row replaces it, so a control that captured the old value
    /// can no longer recognise its own row and a second column of the same row would be dropped.
    /// The slot keeps one identity per row while its value changes, and a rebuild discards the
    /// slots — which is what makes a late LostFocus from a removed control a no-op instead of a
    /// write into whichever row took its position.
    /// </remarks>
    private sealed class RowSlot
    {
        public RowSlot(PunctuationRow value) => Value = value;

        public PunctuationRow Value { get; set; }
    }

    private StackPanel _rows = null!;
    private TextBlock _notice = null!;
    private List<RowSlot> _slots = new();

    protected override UIElement BuildContent()
    {
        var scroll = PageChrome.Create(
            "标点映射",
            "编辑拼音/双拼共用的 Fcitx zh_CN 标点表。每一行都是官方配置中的一条映射。",
            out var body);

        var snapshot = Workspace.SnapshotFor(SettingsTarget.Punctuation);
        var draft = Workspace.DraftFor(SettingsTarget.Punctuation);
        if (snapshot is null || draft is null)
        {
            PageChrome.AddSection(body, "标点映射", null,
                new[] { Note("Host 没有提供 zh_CN 标点配置，请重新连接后重试。") });
            return scroll;
        }

        _rows = new StackPanel { Spacing = 8 };
        _notice = Note(string.Empty);
        _notice.Visibility = Visibility.Collapsed;

        var header = new Grid { ColumnSpacing = 8 };
        for (var column = 0; column < 4; column++)
        {
            header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        }
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.Children.Add(Label("键", 0));
        header.Children.Add(Label("映射", 1));
        header.Children.Add(Label("备选 / 成对", 2));
        header.Children.Add(Label("操作", 4));

        var add = new Button { Content = "添加一行" };
        add.Click += (_, _) =>
        {
            var candidate = Workspace.PunctuationRows
                .Append(new PunctuationRow(string.Empty, string.Empty, string.Empty))
                .ToList();
            if (Workspace.SetPunctuationRows(candidate))
            {
                Rebuild();
            }
        };

        var restore = new Button { Content = "恢复默认映射" };
        restore.Click += (_, _) =>
        {
            var defaults = PunctuationMap.Defaults(snapshot);
            if (defaults.Count == 0)
            {
                Say("Host 没有给出默认表；没有改动任何内容。");
                return;
            }
            if (!Workspace.SetPunctuationRows(defaults))
            {
                Say("默认映射无法写入草稿；没有改动任何内容。");
                return;
            }
            Rebuild();
            Say("已恢复 Fcitx 自带的 zh_CN 映射；点击“保存”写入配置。");
        };

        var actions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 12,
        };
        actions.Children.Add(add);
        actions.Children.Add(restore);

        PageChrome.AddSection(body, "zh_CN 映射", null, new FrameworkElement[]
        {
            Note("同一键可以有多行；顺序就是候选顺序。带成对符号的一行使用“备选 / 成对”列。"),
            header,
            _rows,
            actions,
            _notice,
        });
        Rebuild();
        return scroll;
    }

    private static TextBlock Note(string text) => new()
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
    };

    private static TextBlock Label(string text, int column)
    {
        var label = new TextBlock
        {
            Text = text,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(label, column);
        return label;
    }

    private void Rebuild()
    {
        _rows.Children.Clear();
        _slots = Workspace.PunctuationRows.Select(row => new RowSlot(row)).ToList();
        for (var index = 0; index < _slots.Count; index++)
        {
            _rows.Children.Add(Row(index, _slots[index]));
        }
        UpdateNotice();
    }

    private FrameworkElement Row(int index, RowSlot slot)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        grid.Children.Add(Field("键", slot, "key", 0));
        grid.Children.Add(Field("映射", slot, "mapping", 1));
        grid.Children.Add(Field("备选", slot, "alt", 2));
        grid.Children.Add(Move(index, -1, "↑", 3));
        grid.Children.Add(Move(index, 1, "↓", 4));
        grid.Children.Add(Delete(index, 5));
        return grid;
    }

    private TextBox Field(string name, RowSlot slot, string field, int column)
    {
        var row = slot.Value;
        var box = new TextBox
        {
            Text = field switch
            {
                "key" => row.Key,
                "mapping" => row.Mapping,
                _ => row.AltMapping,
            },
            PlaceholderText = name,
            MinWidth = 80,
            MaxWidth = 180,
        };
        Grid.SetColumn(box, column);
        box.LostFocus += (_, _) => Change(slot, field, box.Text);
        // Save commits the focused field before it reads the table. Returning true here means
        // the control handed over its text; whether the table is valid is decided at save time.
        PendingEdit.SetCommit(box, () => Change(slot, field, box.Text));
        return box;
    }

    private Button Move(int index, int delta, string glyph, int column)
    {
        var button = new Button
        {
            Content = glyph,
            IsEnabled = index + delta >= 0 && index + delta < Workspace.PunctuationRows.Count,
        };
        Grid.SetColumn(button, column);
        button.Click += (_, _) =>
        {
            var candidate = Workspace.PunctuationRows.ToList();
            var other = index + delta;
            if (other < 0 || other >= candidate.Count)
            {
                return;
            }
            (candidate[index], candidate[other]) = (candidate[other], candidate[index]);
            if (Workspace.SetPunctuationRows(candidate))
            {
                Rebuild();
            }
        };
        return button;
    }

    private Button Delete(int index, int column)
    {
        var button = new Button { Content = "删除" };
        Grid.SetColumn(button, column);
        button.Click += (_, _) =>
        {
            var candidate = Workspace.PunctuationRows.ToList();
            if (index < 0 || index >= candidate.Count)
            {
                return;
            }
            candidate.RemoveAt(index);
            if (Workspace.SetPunctuationRows(candidate))
            {
                Rebuild();
            }
        };
        return button;
    }

    private bool Change(RowSlot slot, string field, string text)
    {
        var index = -1;
        for (var i = 0; i < _slots.Count; i++)
        {
            if (ReferenceEquals(_slots[i], slot))
            {
                index = i;
                break;
            }
        }
        // A rebuild replaces the slots. A late LostFocus from a removed control must not apply
        // its old text to whichever row now occupies that position.
        if (index < 0)
        {
            return true;
        }

        var candidate = _slots.Select(entry => entry.Value).ToList();
        var row = candidate[index];
        candidate[index] = field switch
        {
            "key" => row with { Key = text },
            "mapping" => row with { Mapping = text },
            _ => row with { AltMapping = text },
        };
        if (!Workspace.SetPunctuationRows(candidate))
        {
            return false;
        }

        // The slot keeps its identity while its value moves on, so the other columns of this
        // same row still find it.
        slot.Value = candidate[index];
        UpdateNotice();
        return true;
    }

    private void UpdateNotice()
    {
        var reason = Workspace.PunctuationInvalidReason;
        if (reason is null)
        {
            _notice.Visibility = Visibility.Collapsed;
            _notice.Text = string.Empty;
            return;
        }
        Say(reason + " 修正后才会写入草稿。");
    }

    private void Say(string text)
    {
        _notice.Text = text;
        _notice.Visibility = Visibility.Visible;
    }
}
