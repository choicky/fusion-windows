using System;
using System.Collections.Generic;
using System.Linq;
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
/// The editor refuses to write a row Fcitx would drop (an empty or multi-character key, an
/// empty mapping, or whitespace inside a column). The visible row stays so it can be fixed,
/// but it is not put into the draft until it is valid; the save bar therefore cannot send a
/// map the addon would silently rewrite.
/// </para>
/// </remarks>
public sealed class PunctuationPage : SettingsPageBase
{
    private StackPanel _rows = null!;
    private TextBlock _notice = null!;
    private List<PunctuationRow> _editing = new();

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

        _editing = PunctuationMap.RowsFromOption(draft.Effective(PunctuationMap.Entries)).ToList();
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
            _editing.Add(new PunctuationRow(string.Empty, string.Empty, string.Empty));
            Rebuild();
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
            if (!Workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                    PunctuationMap.Value(defaults)))
            {
                Say("默认映射无法写入草稿；没有改动任何内容。");
                return;
            }
            _editing = defaults.ToList();
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
        for (var index = 0; index < _editing.Count; index++)
        {
            _rows.Children.Add(Row(index, _editing[index]));
        }
        UpdateNotice();
    }

    private FrameworkElement Row(int index, PunctuationRow row)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        grid.Children.Add(Field("键", row.Key, index, "key", 0));
        grid.Children.Add(Field("映射", row.Mapping, index, "mapping", 1));
        grid.Children.Add(Field("备选", row.AltMapping, index, "alt", 2));
        grid.Children.Add(Move(index, -1, "↑", 3));
        grid.Children.Add(Move(index, 1, "↓", 4));
        grid.Children.Add(Delete(index, 5));
        return grid;
    }

    private TextBox Field(string name, string value, int index, string field, int column)
    {
        var box = new TextBox
        {
            Text = value,
            PlaceholderText = name,
            MinWidth = 80,
            MaxWidth = 180,
        };
        Grid.SetColumn(box, column);
        box.LostFocus += (_, _) => Change(index, field, box.Text);
        return box;
    }

    private Button Move(int index, int delta, string glyph, int column)
    {
        var button = new Button
        {
            Content = glyph,
            IsEnabled = index + delta >= 0 && index + delta < _editing.Count,
        };
        Grid.SetColumn(button, column);
        button.Click += (_, _) =>
        {
            var other = index + delta;
            if (other < 0 || other >= _editing.Count)
            {
                return;
            }
            var candidate = _editing.ToList();
            (candidate[index], candidate[other]) = (candidate[other], candidate[index]);
            Commit(candidate);
            Rebuild();
        };
        return button;
    }

    private Button Delete(int index, int column)
    {
        var button = new Button { Content = "删除" };
        Grid.SetColumn(button, column);
        button.Click += (_, _) =>
        {
            var candidate = _editing.ToList();
            candidate.RemoveAt(index);
            Commit(candidate);
            Rebuild();
        };
        return button;
    }

    private void Change(int index, string field, string text)
    {
        if (index >= _editing.Count)
        {
            return;
        }
        var row = _editing[index];
        var candidate = _editing.ToList();
        candidate[index] = field switch
        {
            "key" => row with { Key = text },
            "mapping" => row with { Mapping = text },
            _ => row with { AltMapping = text },
        };
        Commit(candidate);
        UpdateNotice();
    }

    // Only a completely valid list enters the draft. A partially typed row remains visible so
    // the user can finish it, and the notice says why it has not been stored yet.
    private void Commit(List<PunctuationRow> candidate)
    {
        _editing = candidate;
        var reason = PunctuationMap.InvalidReason(candidate);
        if (reason is not null)
        {
            Say(reason + " 修正后才会写入草稿。");
            return;
        }
        if (!Workspace.TrySet(SettingsTarget.Punctuation, PunctuationMap.Entries,
                PunctuationMap.Value(candidate)))
        {
            Say("标点映射当前不可编辑；修改没有写入草稿。");
            return;
        }
        UpdateNotice();
    }

    private void UpdateNotice()
    {
        var reason = PunctuationMap.InvalidReason(_editing);
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
