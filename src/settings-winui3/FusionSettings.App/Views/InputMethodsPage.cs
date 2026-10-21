using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Fusion.Settings.App.Views;

/// <summary>
/// Input-method membership and order, with Fcitx's own semantics.
/// </summary>
/// <remarks>
/// The first entry of the group is the method a bound context follows; the fallback
/// (<c>DefaultIM</c>) is a different value and is shown, not edited. Removing a method
/// removes membership only — it never deletes a definition file or learning data.
/// </remarks>
public sealed class InputMethodsPage : SettingsPageBase
{
    protected override UIElement BuildContent()
    {
        var page = PageChrome.Create(
            "输入方式",
            "管理哪些输入方式出现在切换列表中，以及它们的顺序。",
            out var body);

        var methods = Workspace.Methods;
        if (methods is null)
        {
            body.Children.Add(PageChrome.Note("尚未从 Host 读取到输入方式列表。请确认 FusionHost 正在运行。"));
            return page;
        }

        var enabledRows = new List<FrameworkElement>();
        for (var i = 0; i < methods.Enabled.Count; i++)
        {
            enabledRows.Add(EnabledRow(methods, i));
        }

        PageChrome.AddSection(body, "已启用的输入方式",
            "第一项是绑定窗口跟随的当前输入方式；回退方式（" + MethodLabel(methods.Baseline.Fallback) +
            "）是另一个设置，不在这里修改。",
            enabledRows);

        var available = methods.Available.Where(name => !methods.IsEnabled(name)).ToList();
        var availableRows = available.Select(name => AvailableRow(methods, name)).ToList();
        PageChrome.AddSection(body, "可添加的输入方式",
            availableRows.Count == 0 ? "当前安装包提供的输入方式都已经在列表中。" : null,
            availableRows);

        body.Children.Add(BuildEditTarget());
        return page;
    }

    private FrameworkElement EnabledRow(Fusion.Settings.Model.ImListDraft methods, int index)
    {
        var name = methods.Enabled[index];
        var isActive = index == 0;

        var title = new TextBlock
        {
            Text = MethodLabel(name),
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            VerticalAlignment = VerticalAlignment.Center,
        };

        var badges = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        badges.Children.Add(title);

        if (isActive)
        {
            badges.Children.Add(new Border
            {
                Background = (Brush)Application.Current.Resources["AccentFillColorDefaultBrush"],
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(6, 3, 6, 3),
                VerticalAlignment = VerticalAlignment.Center,
                Child = new TextBlock
                {
                    Text = "当前",
                    FontSize = 11,
                    Foreground = (Brush)Application.Current.Resources["TextOnAccentFillColorPrimaryBrush"],
                },
            });
        }

        if (!methods.IsAvailable(name))
        {
            badges.Children.Add(new TextBlock
            {
                Text = "本安装包未提供",
                Opacity = 0.7,
                VerticalAlignment = VerticalAlignment.Center,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
        }

        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };

        var up = new Button { Content = "上移", IsEnabled = index > 0, Padding = new Thickness(10, 4, 10, 4) };
        // Through the workspace, so the save bar and the close guard see the change.
        up.Click += (_, _) =>
        {
            if (Workspace.MoveMethod(index, index - 1))
            {
                Refresh();
            }
        };

        var down = new Button
        {
            Content = "下移",
            IsEnabled = index < methods.Enabled.Count - 1,
            Padding = new Thickness(10, 4, 10, 4),
        };
        down.Click += (_, _) =>
        {
            if (Workspace.MoveMethod(index, index + 1))
            {
                Refresh();
            }
        };

        var remove = new Button
        {
            Content = "移除",
            IsEnabled = methods.Enabled.Count > 1,
            Padding = new Thickness(10, 4, 10, 4),
        };
        remove.Click += (_, _) =>
        {
            if (Workspace.RemoveMethod(name))
            {
                Refresh();
            }
        };

        actions.Children.Add(up);
        actions.Children.Add(down);
        actions.Children.Add(remove);

        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(badges);
        Grid.SetColumn(actions, 1);
        row.Children.Add(actions);
        return row;
    }

    private FrameworkElement AvailableRow(Fusion.Settings.Model.ImListDraft methods, string name)
    {
        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        row.Children.Add(new TextBlock
        {
            Text = MethodLabel(name),
            VerticalAlignment = VerticalAlignment.Center,
        });

        var add = new Button { Content = "添加", Padding = new Thickness(10, 4, 10, 4) };
        add.Click += (_, _) =>
        {
            if (Workspace.AddMethod(name))
            {
                Refresh();
            }
        };

        Grid.SetColumn(add, 1);
        row.Children.Add(add);
        return row;
    }

    private FrameworkElement BuildEditTarget()
    {
        var section = new StackPanel { Spacing = 8 };
        section.Children.Add(new TextBlock
        {
            Text = "编辑哪个输入方式的设置",
            Style = (Style)Application.Current.Resources["SubtitleTextBlockStyle"],
        });

        var card = new Border
        {
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(16),
        };

        var content = new StackPanel { Spacing = 10 };
        content.Children.Add(PageChrome.MethodSelector(Workspace, Refresh));
        content.Children.Add(PageChrome.Note(
            "拼音与双拼共用同一份底层配置，所以它们的大部分设置是同一批值；两者各自可见的选项不完全相同，" +
            "在这里切换只决定编辑哪一份视图。打开设置不会把该方式设为当前输入法。"));
        card.Child = content;
        section.Children.Add(card);
        return section;
    }
}
