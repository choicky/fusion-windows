using System;
using System.Collections.Generic;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// Snapshots shaped like the ones the Host actually returns, so the checks exercise the
/// real structure rather than a convenient fiction. The shapes here were read from a
/// running Host on 2026-10-21 (see the task's schema dump).
/// </summary>
internal static class TestSchema
{
    public static OptionDescription Option(string name, string type, string defaultValue,
                                           params (string Key, string Value)[] properties)
    {
        var propertyMap = SettingsValue.EmptyMap();
        foreach (var (key, value) in properties)
        {
            propertyMap = propertyMap.WithPath(new[] { key }, SettingsValue.Text(value));
        }

        return new OptionDescription(name, type, name, SettingsValue.Text(defaultValue), propertyMap);
    }

    /// <summary>The Pinyin view: 41 options, and it does not describe ShuangpinProfile.</summary>
    public static SettingsSnapshot PinyinView()
    {
        var options = new List<OptionDescription>
        {
            Option("PageSize", "Integer", "7", ("IntMin", "3"), ("IntMax", "10")),
            Option("Learning", "Boolean", "True"),
            Option("PreeditMode", "Enum", "Composing pinyin"),
            Option("PrevPage", "List|Key", string.Empty),
            Option("ChooseCharFromPhrase", "List|Key", string.Empty),
            Option("ChooseLastCharFromPhrase", "Key", "bracketright"),
            Option("Number of sentence", "Integer", "2", ("IntMin", "1"), ("IntMax", "3")),
            Option("DictManager", "External", string.Empty,
                ("External", "fcitx://config/addon/pinyin/dictmanager")),
            Option("Punctuation", "External", string.Empty,
                ("External", "fcitx://config/addon/punctuation/punctuationmap/zh_CN"),
                ("LaunchSubConfig", "True")),
            Option("Fuzzy", "Fuzzy$FuzzyConfig", string.Empty),
        };

        var fuzzy = new List<OptionDescription>
        {
            Option("VE_UE", "Boolean", "True"),
            Option("Inner", "Boolean", "True"),
            Option("Correction", "Enum", "None"),
        };

        var values = SettingsValue.Map(
            ("PageSize", SettingsValue.Text("7")),
            ("Learning", SettingsValue.Text("True")),
            ("PreeditMode", SettingsValue.Text("Composing pinyin")),
            ("PrevPage", KeyList("minus", "Up", "KP_Up", "Page_Up")),
            ("ChooseCharFromPhrase", KeyList("bracketleft")),
            ("ChooseLastCharFromPhrase", SettingsValue.Text("bracketright")),
            ("Number of sentence", SettingsValue.Text("2")),
            ("Fuzzy", SettingsValue.Map(
                ("VE_UE", SettingsValue.Text("True")),
                ("Inner", SettingsValue.Text("True")),
                ("Correction", SettingsValue.Text("None")))),
            // Present in the value tree, absent from this view's schema.
            ("ShuangpinProfile", SettingsValue.Text("ziranma")),
            ("ShowShuangpinMode", SettingsValue.Text("False")),
            ("FirstRun", SettingsValue.Text("False")));

        return new SettingsSnapshot(values,
            new[]
            {
                new ConfigType("PinyinEngineConfig", options),
                new ConfigType("Fuzzy$FuzzyConfig", fuzzy),
            });
    }

    /// <summary>The Shuangpin view: the same values, two extra schema options.</summary>
    public static SettingsSnapshot ShuangpinView()
    {
        var pinyin = PinyinView();
        var options = new List<OptionDescription>(pinyin.Types[0].Options)
        {
            Option("ShuangpinProfile", "Enum", "ziranma"),
            Option("ShowShuangpinMode", "Boolean", "False"),
        };

        return new SettingsSnapshot(pinyin.Values,
            new[]
            {
                new ConfigType("PinyinEngineConfig", options),
                pinyin.Types[1],
            });
    }

    /// <summary>A <c>List|Key</c> value in the shape the Host produces: indexed keys.</summary>
    public static SettingsValue KeyList(params string[] keys)
    {
        var entries = new (string, SettingsValue)[keys.Length];
        for (var i = 0; i < keys.Length; i++)
        {
            entries[i] = (i.ToString(System.Globalization.CultureInfo.InvariantCulture),
                          SettingsValue.Text(keys[i]));
        }

        return SettingsValue.Map(entries);
    }

    public static SettingsSnapshot GlobalView()
    {
        var hotkey = new List<OptionDescription>
        {
            Option("TriggerKeys", "List|Key", string.Empty),
            Option("ConsumeRedundantActivateKeys", "Boolean", "False"),
        };

        var behavior = new List<OptionDescription>
        {
            Option("ActiveByDefault", "Boolean", "False"),
            Option("DefaultPageSize", "Integer", "5", ("IntMin", "1"), ("IntMax", "10")),
            Option("resetStateWhenFocusIn", "Enum", "No"),
            Option("AutoSavePeriod", "Integer", "30", ("IntMin", "0"), ("IntMax", "1440")),
        };

        var values = SettingsValue.Map(
            ("Hotkey", SettingsValue.Map(
                ("TriggerKeys", KeyList("Control+space")),
                ("ConsumeRedundantActivateKeys", SettingsValue.Text("False")))),
            ("Behavior", SettingsValue.Map(
                ("ActiveByDefault", SettingsValue.Text("False")),
                ("DefaultPageSize", SettingsValue.Text("5")),
                ("resetStateWhenFocusIn", SettingsValue.Text("No")),
                ("AutoSavePeriod", SettingsValue.Text("30")))));

        return new SettingsSnapshot(values,
            new[]
            {
                new ConfigType("GlobalConfig", new List<OptionDescription>
                {
                    Option("Hotkey", "Hotkey$HotkeyConfig", string.Empty),
                    Option("Behavior", "Behavior$BehaviorConfig", string.Empty),
                }),
                new ConfigType("Hotkey$HotkeyConfig", hotkey),
                new ConfigType("Behavior$BehaviorConfig", behavior),
            });
    }
}
