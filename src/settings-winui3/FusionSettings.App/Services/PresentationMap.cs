using System;
using System.Collections.Generic;

namespace Fusion.Settings.App.Services;

/// <summary>Where one option appears, and how it is described.</summary>
/// <param name="EnumLabels">
/// Chinese display text for an enum's stored values, keyed by the stored value. The stored
/// value itself is never translated: it is what the configuration file contains and what
/// the Host reads back, so a saved profile never depends on the display language. A value
/// with no entry keeps whatever the Host supplied.
/// </param>
public sealed record Placement(string Page, string Section, int Order, string Label, string? Help = null,
                               IReadOnlyDictionary<string, string>? EnumLabels = null);

/// <summary>
/// The one table that decides where each official option lives in the accepted
/// navigation. It is presentation only: types, defaults and constraints come from the
/// Host schema, so nothing here can invent an option or a default.
/// </summary>
/// <remarks>
/// An option with no entry here is still reachable — the pages render it under
/// "更多设置" of its own configuration — so a Host that adds a field never becomes
/// invisible. The table only decides placement and wording.
/// </remarks>
public static class PresentationMap
{
    public const string PageMethods = "methods";
    public const string PageKeys = "keys";
    public const string PageCandidates = "candidates";
    public const string PageAbout = "about";

    /// <summary>Pinyin/Shuangpin options, keyed by the name the Host uses.</summary>
    public static readonly IReadOnlyDictionary<string, Placement> Pinyin =
        new Dictionary<string, Placement>(StringComparer.Ordinal)
        {
            // Candidates
            ["PageSize"] = new(PageCandidates, "候选", 10, "每页候选数", "拼音引擎自己的每页数量，与系统默认值不同。"),
            ["Prediction"] = new(PageCandidates, "候选", 20, "联想"),
            ["PredictionSize"] = new(PageCandidates, "候选", 21, "联想候选数量"),
            ["KeepCurrentContext"] = new(PageCandidates, "候选", 22, "联想时保留已输入内容"),
            ["BackspaceBehaviorOnPrediction"] = new(PageCandidates, "候选", 23, "联想时的退格行为"),
            ["SpellEnabled"] = new(PageCandidates, "候选", 30, "显示英文候选"),
            ["SymbolsEnabled"] = new(PageCandidates, "候选", 31, "显示符号候选"),
            ["ChaiziEnabled"] = new(PageCandidates, "候选", 32, "显示拆字候选"),
            ["StrokeCandidateEnabled"] = new(PageCandidates, "候选", 33, "输入 h 时显示笔画候选"),
            ["ExtBEnabled"] = new(PageCandidates, "候选", 34, "启用扩展 B 区及之后的汉字"),
            ["UseKeypadAsSelection"] = new(PageCandidates, "候选", 40, "使用小键盘选择候选"),

            // Preedit
            ["PreeditMode"] = new(PageCandidates, "预编辑", 10, "预编辑模式"),
            ["PinyinInPreedit"] = new(PageCandidates, "预编辑", 20, "在预编辑中显示完整拼音"),
            ["PreeditCursorPositionAtBeginning"] = new(PageCandidates, "预编辑", 30, "光标固定在预编辑开头"),

            // Learning
            ["Learning"] = new(PageCandidates, "学习", 10, "启用学习"),
            ["ForgetWord"] = new(PageKeys, "学习", 10, "忘记词语",
                "把光标放在词语上按下此键，可以把它从学习记录中移除。"),

            // Paging
            ["PrevPage"] = new(PageKeys, "翻页", 10, "上一页"),
            ["NextPage"] = new(PageKeys, "翻页", 20, "下一页"),

            // Candidate navigation
            ["PrevCandidate"] = new(PageKeys, "候选导航", 10, "上一个候选"),
            ["NextCandidate"] = new(PageKeys, "候选导航", 20, "下一个候选"),
            ["CurrentCandidate"] = new(PageKeys, "候选导航", 30, "选择当前候选"),

            // Candidate selection
            ["SecondCandidate"] = new(PageKeys, "候选选择", 10, "选择第二个候选"),
            ["ThirdCandidate"] = new(PageKeys, "候选选择", 20, "选择第三个候选"),

            // Commit
            ["CommitRawInput"] = new(PageKeys, "上屏", 10, "直接上屏原始输入",
                "在此路径上，带 Ctrl/Alt/Shift 的组合键与 KP_Enter 不会被送达引擎，只有未修饰的按键生效。"),

            // Phrase
            ["ChooseCharFromPhrase"] = new(PageKeys, "短语选字", 10, "从候选中逐字选择",
                "第 N 个绑定选择当前候选的第 N 个字，顺序有意义，不会被排序。"),
            ["ChooseLastCharFromPhrase"] = new(PageKeys, "短语选字", 20, "选择候选的最后一个字"),
            ["BackSpaceToUnselect"] = new(PageKeys, "短语选字", 30, "用退格取消已选的字"),

            // Quick phrase
            ["QuickPhraseKey"] = new(PageKeys, "快捷短语", 10, "触发键"),
            ["VAsQuickphrase"] = new(PageKeys, "快捷短语", 20, "用 v 触发快捷短语"),

            // More
            ["FilterByStroke"] = new(PageKeys, "更多设置", 10, "按笔画筛选"),
            ["QuickPhraseTriggerRegex"] = new(PageKeys, "更多设置", 20, "触发快捷短语的规则",
                "每行一条正则表达式。"),
            ["SwitchInputMethodBehavior"] = new(PageCandidates, "更多设置", 10, "切换输入法时的处理"),
            ["Number of sentence"] = new(PageCandidates, "更多设置", 20, "联想句子数量"),
            ["WordCandidateLimit"] = new(PageCandidates, "更多设置", 30, "词组候选数量"),
            ["LongWordLengthLimit"] = new(PageCandidates, "更多设置", 40, "长词提示长度"),

            // Shuangpin-only
            ["ShuangpinProfile"] = new(PageCandidates, "双拼", 10, "双拼方案"),
            ["ShowShuangpinMode"] = new(PageCandidates, "双拼", 20, "显示双拼提示"),
        };

    /// <summary>Global options, keyed by the value-tree path inside the global configuration.</summary>
    public static readonly IReadOnlyDictionary<string, Placement> Global =
        new Dictionary<string, Placement>(StringComparer.Ordinal)
        {
            ["Hotkey/TriggerKeys"] = new(PageKeys, "系统快捷键", 10, "切换输入法"),
            ["Hotkey/AltTriggerKeys"] = new(PageKeys, "系统快捷键", 20, "临时切换输入法"),
            ["Hotkey/ActivateKeys"] = new(PageKeys, "系统快捷键", 30, "启用输入法"),
            ["Hotkey/DeactivateKeys"] = new(PageKeys, "系统快捷键", 40, "停用输入法"),
            ["Hotkey/ConsumeRedundantActivateKeys"] = new(PageKeys, "系统快捷键", 50, "忽略重复的启用/停用键"),
            ["Hotkey/EnumerateWithTriggerKeys"] = new(PageKeys, "系统快捷键", 60, "按住切换键时枚举"),
            ["Hotkey/EnumerateForwardKeys"] = new(PageKeys, "系统快捷键", 70, "向前枚举输入法"),
            ["Hotkey/EnumerateBackwardKeys"] = new(PageKeys, "系统快捷键", 80, "向后枚举输入法"),
            ["Hotkey/EnumerateSkipFirst"] = new(PageKeys, "系统快捷键", 90, "枚举时跳过第一个"),
            ["Hotkey/EnumerateGroupForwardKeys"] = new(PageKeys, "系统快捷键", 100, "向前切换输入法组"),
            ["Hotkey/EnumerateGroupBackwardKeys"] = new(PageKeys, "系统快捷键", 110, "向后切换输入法组"),
            ["Hotkey/TogglePreedit"] = new(PageKeys, "系统快捷键", 120, "切换内嵌预编辑"),
            ["Hotkey/ModifierOnlyKeyTimeout"] = new(PageKeys, "系统快捷键", 130, "修饰键触发时限（毫秒）"),

            // The global fallback paging keys are a different option from the Pinyin ones:
            // the engine's own keys page the Pinyin candidate list, while these are consumed
            // by the core's fallback path for temporary modes that have no engine paging.
            ["Hotkey/PrevPage"] = new(PageKeys, "系统回退翻页", 10, "上一页（回退）",
                "这一组与拼音引擎的翻页键是两个不同的选项：它由核心的回退路径使用，不是拼音候选列表的翻页键。"),
            ["Hotkey/NextPage"] = new(PageKeys, "系统回退翻页", 20, "下一页（回退）",
                "这一组与拼音引擎的翻页键是两个不同的选项：它由核心的回退路径使用，不是拼音候选列表的翻页键。"),
            ["Hotkey/PrevCandidate"] = new(PageKeys, "系统回退翻页", 30, "上一个候选（回退）"),
            ["Hotkey/NextCandidate"] = new(PageKeys, "系统回退翻页", 40, "下一个候选（回退）"),

            ["Behavior/ActiveByDefault"] = new(PageCandidates, "系统输入行为", 10, "默认启用输入法"),
            ["Behavior/resetStateWhenFocusIn"] = new(PageCandidates, "系统输入行为", 20, "获得焦点时重置状态"),
            ["Behavior/ShareInputState"] = new(PageCandidates, "系统输入行为", 30, "共享输入状态"),
            ["Behavior/PreeditEnabledByDefault"] = new(PageCandidates, "系统输入行为", 40, "在应用中显示预编辑"),
            ["Behavior/DefaultPageSize"] = new(PageCandidates, "系统输入行为", 50, "系统默认每页候选数",
                "这是系统的默认值，拼音页有自己的每页候选数，两者不同。"),
            ["Behavior/AutoSavePeriod"] = new(PageCandidates, "系统输入行为", 60, "自动保存间隔（分钟）"),
            ["Behavior/AllowInputMethodForPassword"] = new(PageCandidates, "系统输入行为", 70, "允许在密码框中使用输入法"),
            ["Behavior/ShowPreeditForPassword"] = new(PageCandidates, "系统输入行为", 80, "在密码框中显示预编辑"),
            ["Behavior/ShowInputMethodInformation"] = new(PageCandidates, "系统输入行为", 90, "切换时显示输入法提示",
                "提示由 notifications 组件提供，本安装包未包含该组件，因此此项不会产生可见效果。"),
            ["Behavior/showInputMethodInformationWhenFocusIn"] = new(PageCandidates, "系统输入行为", 100, "获得焦点时显示提示",
                "提示由 notifications 组件提供，本安装包未包含该组件。"),
            ["Behavior/CompactInputMethodInformation"] = new(PageCandidates, "系统输入行为", 110, "显示紧凑提示",
                "提示由 notifications 组件提供，本安装包未包含该组件。"),
            ["Behavior/ShowFirstInputMethodInformation"] = new(PageCandidates, "系统输入行为", 120, "显示第一个输入法的提示",
                "提示由 notifications 组件提供，本安装包未包含该组件。"),
        };

    /// <summary>
    /// Windows candidate-presentation options, keyed by the name the Host uses. This is the
    /// one configuration that is not an official Fcitx addon or global schema: it is the
    /// Windows presentation configuration the owner authorized on 2026-10-22, and the Host
    /// owns the stored value.
    /// </summary>
    public static readonly IReadOnlyDictionary<string, Placement> Presentation =
        new Dictionary<string, Placement>(StringComparer.Ordinal)
        {
            ["CandidateLayout"] = new(PageCandidates, "候选窗口", 10, "候选排列方向",
                "竖排每个候选占一行；横排把这一页的候选排在同一行；自动卷轴先显示一行，"
                + "按过第一个“下一页”后展开成多行，每行候选数取输入法自己的每页候选数（最多五行，"
                + "屏幕放不下时少显示几行），“下一页”先下移一行并保持所在列，到底再进入下一屏。"
                + "若输入法为某个候选列表指定了排列方向"
                + "（例如快捷短语、Unicode 等模式），输入法的指定优先于这里的设置。",
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["Vertical"] = "竖排",
                    ["Horizontal"] = "横排",
                    ["AutoScroll"] = "自动卷轴",
                }),
            ["FontFamily"] = new(PageCandidates, "候选窗口", 20, "候选字体",
                "候选窗口中候选文字使用的字体。字体不存在时由 Windows 自动替换为相近字体，"
                + "缺少的字由系统字体链接补齐。"),
            ["FontSize"] = new(PageCandidates, "候选窗口", 30, "字号（磅）",
                "候选文字的字号，以磅为单位，所以在不同缩放比例的屏幕上保持相同的物理大小。"
                + "不是按屏幕缩小字号来迁就越界。"),
        };

    /// <summary>Section order within a page; anything unknown sorts last.</summary>
    public static readonly IReadOnlyList<string> KeysSections = new[]
    {
        "翻页", "候选导航", "候选选择", "上屏", "短语选字", "快捷短语", "学习",
        "系统快捷键", "系统回退翻页", "更多设置",
    };

    public static readonly IReadOnlyList<string> CandidateSections = new[]
    {
        "候选窗口", "候选", "预编辑", "学习", "双拼", "系统输入行为", "更多设置",
    };

    public static Placement? FindPinyin(string optionName) =>
        Pinyin.TryGetValue(optionName, out var placement) ? placement : null;

    public static Placement? FindGlobal(string container, string optionName) =>
        Global.TryGetValue(container + "/" + optionName, out var placement) ? placement : null;

    public static Placement? FindPresentation(string optionName) =>
        Presentation.TryGetValue(optionName, out var placement) ? placement : null;

    /// <summary>Options that are links to other tools rather than values.</summary>
    public static readonly IReadOnlyDictionary<string, string> ExternalToolReason =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["DictManager"] = "词典管理工具尚未在 Windows 上提供。",
            ["CustomPhrase"] = "自定义短语编辑器尚未在 Windows 上提供。",
            ["Punctuation"] = "标点映射编辑器尚未在 Windows 上提供。",
            ["Chttrans"] = "简繁转换组件（chttrans）未包含在本安装包中。",
            ["QuickPhrase"] = "快捷短语编辑器尚未在 Windows 上提供。",
        };
}
