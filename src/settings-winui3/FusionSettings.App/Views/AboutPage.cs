using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Fusion.Settings.App.Views;

/// <summary>
/// What this build is, how it is connected, and what it does not do.
/// </summary>
public sealed class AboutPage : SettingsPageBase
{
    protected override UIElement BuildContent()
    {
        var page = PageChrome.Create(
            "关于与诊断",
            "版本、连接状态，以及尚未提供的功能。",
            out var body);

        AddConnection(body);
        AddLimitations(body);
        AddDiagnostics(body);
        return page;
    }

    private void AddConnection(StackPanel body)
    {
        var rows = new List<FrameworkElement>
        {
            Field("程序", "Fusion 设置（WinUI 3）"),
            Field("版本", typeof(AboutPage).Assembly.GetName().Version?.ToString() ?? "未知"),
            Field("Host 端点", Workspace.Endpoint),
            Field("端点类型", Workspace.IsProductionEndpoint ? "当前用户的正式端点" : "隔离端点（--pipe-name）"),
            Field("状态", Workspace.StatusText),
        };

        PageChrome.AddSection(body, "连接", null, rows);
    }

    private void AddLimitations(StackPanel body)
    {
        var rows = new List<FrameworkElement>
        {
            Paragraph("带 Ctrl/Alt/Win 的组合键、纯修饰键以及小键盘回车在此输入路径上不会被送达引擎。这些绑定可以保存，但不会触发；" +
                      "这是当前 TSF 路径的限制，不是设置界面可以改变的。"),
            Paragraph("词库管理在本窗口的「词库管理」页提供；简繁转换、自定义短语、标点映射与快捷短语编辑器是 Fcitx " +
                      "上游的独立工具，本安装包没有提供。"),
            Paragraph("搜狗 .scel 词库暂不支持导入：它需要 addon 的 scel2org5 转换工具，当前 Windows 运行时没有构建该工具。"),
            Paragraph("云拼音组件未包含在本安装包中，因此没有对应选项。"),
            Paragraph("设置窗口只读写配置，不会自动重启 Host，也不会修改输入法的注册状态。"),
        };

        PageChrome.AddSection(body, "已知限制", null, rows);
    }

    private void AddDiagnostics(StackPanel body)
    {
        var log = Workspace.Diagnostics;
        var text = log.Count == 0
            ? "本次运行还没有诊断记录。"
            : string.Join(Environment.NewLine, log);

        var box = new TextBox
        {
            Text = text,
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Consolas"),
            FontSize = 12,
            Height = 180,
        };

        PageChrome.AddSection(body, "诊断",
            "这些是本次运行的技术记录：状态码与错误文本，不包含输入内容。",
            new List<FrameworkElement> { box });
    }

    private static FrameworkElement Field(string label, string value)
    {
        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(180) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        row.Children.Add(new TextBlock
        {
            Text = label,
            Opacity = 0.75,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
        });

        var valueBlock = new TextBlock
        {
            Text = value,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
        };
        Grid.SetColumn(valueBlock, 1);
        row.Children.Add(valueBlock);
        return row;
    }

    private static FrameworkElement Paragraph(string text) => new TextBlock
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
    };
}
