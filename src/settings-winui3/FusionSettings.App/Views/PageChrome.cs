using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Services;
using Fusion.Settings.Workspace;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Fusion.Settings.App.Views;

/// <summary>Shared chrome for the four pages: one readable column, grouped sections.</summary>
public static class PageChrome
{
    /// <summary>A page with a title, a one-line purpose, and a scrollable body.</summary>
    public static ScrollViewer Create(string title, string subtitle, out StackPanel body)
    {
        var column = new StackPanel
        {
            Spacing = 16,
            MaxWidth = 880,
            HorizontalAlignment = HorizontalAlignment.Left,
        };

        column.Children.Add(new TextBlock
        {
            Text = title,
            Style = (Style)Application.Current.Resources["TitleTextBlockStyle"],
        });

        column.Children.Add(new TextBlock
        {
            Text = subtitle,
            Opacity = 0.75,
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
        });

        body = new StackPanel { Spacing = 24 };
        column.Children.Add(body);

        return new ScrollViewer
        {
            Content = column,
            Padding = new Thickness(32, 24, 32, 32),
            HorizontalScrollMode = ScrollMode.Disabled,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
    }

    /// <summary>One titled group of rows. One card per group, not per row.</summary>
    public static void AddSection(StackPanel body, string title, string? note,
                                  IEnumerable<FrameworkElement> rows)
    {
        var rowList = rows.ToList();
        if (rowList.Count == 0)
        {
            return;
        }

        var header = new StackPanel { Spacing = 2 };
        header.Children.Add(new TextBlock
        {
            Text = title,
            Style = (Style)Application.Current.Resources["SubtitleTextBlockStyle"],
        });

        if (!string.IsNullOrEmpty(note))
        {
            header.Children.Add(new TextBlock
            {
                Text = note,
                Opacity = 0.7,
                TextWrapping = TextWrapping.Wrap,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
        }

        var content = new StackPanel { Spacing = 14 };
        for (var i = 0; i < rowList.Count; i++)
        {
            if (i > 0)
            {
                content.Children.Add(new Border
                {
                    Height = 1,
                    Background = (Brush)Application.Current.Resources["DividerStrokeColorDefaultBrush"],
                    Margin = new Thickness(0, 0, 0, 0),
                });
            }

            content.Children.Add(rowList[i]);
        }

        var card = new Border
        {
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(16),
            Child = content,
        };

        var section = new StackPanel { Spacing = 8 };
        section.Children.Add(header);
        section.Children.Add(card);
        body.Children.Add(section);
    }

    /// <summary>A short inline note, used for limitations that must stay visible.</summary>
    public static TextBlock Note(string text) => new()
    {
        Text = text,
        Opacity = 0.75,
        TextWrapping = TextWrapping.Wrap,
        Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
    };

    /// <summary>
    /// The 拼音/双拼 selector. It changes which schema is edited and never activates a
    /// method — the Host's Items/0 is untouched by this control.
    /// </summary>
    public static FrameworkElement MethodSelector(SettingsWorkspace workspace, Action onChanged)
    {
        // The same row style as every other selection list: the method being edited carries the
        // mark, and it moves with the selection without rebuilding the items.
        var rows = new List<Controls.ChoiceRow>
        {
            new("拼音", "pinyin", workspace.EditingMethod != "shuangpin"),
            new("双拼", "shuangpin", workspace.EditingMethod == "shuangpin"),
        };
        var combo = new ComboBox
        {
            MinWidth = 180,
            ItemsSource = rows,
            ItemTemplate = Controls.ChoiceMark.TextTemplate,
            SelectedIndex = workspace.EditingMethod == "shuangpin" ? 1 : 0,
        };
        Controls.ChoiceMark.ApplyTo(combo);
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex < 0 || combo.SelectedIndex >= rows.Count)
            {
                return;
            }

            for (var i = 0; i < rows.Count; i++)
            {
                rows[i].IsCurrent = i == combo.SelectedIndex;
            }

            if (rows[combo.SelectedIndex].Value is { } method)
            {
                workspace.EditMethod(method);
                onChanged();
            }
        };

        var stack = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        stack.Children.Add(new TextBlock
        {
            Text = "正在编辑",
            VerticalAlignment = VerticalAlignment.Center,
        });
        stack.Children.Add(combo);
        stack.Children.Add(Note("切换这里只改变编辑哪一份设置，不会切换当前使用的输入法。"));
        return stack;
    }
}
