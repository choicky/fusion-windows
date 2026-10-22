using System;
using System.Globalization;
using System.Linq;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

internal static class ModelTests
{
    public static void Run()
    {
        Identity();
        Draft();
        DraftLifecycle();
        SharedViews();
        SchemaHelpers();
        StringLists();
        FontFamilies();
    }

    /// <summary>
    /// The installed-font list the candidate-window page offers: it is the system's own
    /// collection, ordered and de-duplicated, and a name it does not contain is reported as
    /// missing rather than replaced.
    /// </summary>
    private static void FontFamilies()
    {
        Harness.Section("font list: the installed families, and a name that is not installed");

        var families = FontCatalog.InstalledFamilies();
        Harness.Check(families.Count > 0, "the system reports installed font families");
        Harness.Check(families.All(name => name.Trim().Length > 0), "no entry is empty");
        Harness.CheckEqual(families.Count,
            families.Distinct(StringComparer.OrdinalIgnoreCase).Count(),
            "no family is listed twice under one spelling");
        Harness.Check(
            families.SequenceEqual(families.OrderBy(name => name, StringComparer.OrdinalIgnoreCase)),
            "the list is sorted the way the picker shows it");

        if (families.Count > 0)
        {
            var first = families[0];
            Harness.CheckEqual(first, FontCatalog.Find(first), "a listed family is found");
            Harness.CheckEqual(first, FontCatalog.Find("  " + first.ToUpperInvariant() + " "),
                "and is found through case and surrounding space");
            Console.WriteLine($"   {families.Count} families; first is \"{first}\"");

            // A Chinese Windows names the CJK families in Chinese (微软雅黑 rather than only
            // "Microsoft YaHei"), which is what makes the list usable here; an English UI is
            // not expected to have them, so the check is guarded by the UI language.
            var localized = families.Count(name => name.Any(ch => ch >= '\u4e00' && ch <= '\u9fff'));
            Console.WriteLine($"   {localized} families are named in the UI language");
            if (CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "zh")
            {
                Harness.Check(localized > 0,
                    "a Chinese UI gets Chinese family names, not only their English spelling");
            }
        }

        Harness.Check(FontCatalog.Find("Fusion No Such Font 12345") is null,
            "a name the system does not have is reported as missing");
        Harness.CheckEqual(0, FontCatalog.Search("Fusion No Such Font 12345").Count,
            "and the search finds nothing for it");
        Harness.CheckEqual(families.Count, FontCatalog.Search(null).Count,
            "an empty search shows the whole list");
        Harness.CheckEqual(1, FontCatalog.Search(families.Count > 0 ? families[0] : string.Empty)
                .Count(family => string.Equals(family, families[0], StringComparison.Ordinal)),
            "a search by exact name narrows to that family");
    }

    private static void Identity()
    {
        Harness.Section("identity: Pinyin and Shuangpin share one underlying configuration");

        var pinyin = ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin");
        var shuangpin = ConfigIdentity.ForUri("fcitx://config/inputmethod/shuangpin");
        Harness.CheckEqual("addon:pinyin", pinyin.Id, "pinyin identity is the addon");
        Harness.CheckEqual("addon:pinyin", shuangpin.Id, "shuangpin identity is the same addon");
        Harness.Check(pinyin.SharesStorageWith(shuangpin), "the two views share storage");
        Harness.CheckEqual(pinyin, shuangpin, "equal identities");
        Harness.Check(pinyin.PublicUri != shuangpin.PublicUri, "public URIs stay distinct");

        var global = ConfigIdentity.ForUri("fcitx://config/global");
        Harness.CheckEqual("global", global.Id, "global has its own identity");
        Harness.Check(!global.SharesStorageWith(pinyin), "global does not share with pinyin");

        var unknown = ConfigIdentity.ForUri("fcitx://config/addon/cloudpinyin");
        Harness.CheckEqual("uri:fcitx://config/addon/cloudpinyin", unknown.Id,
            "an unlisted URI keeps its own identity rather than being generalised");
        Harness.Throws<ArgumentException>(() => ConfigIdentity.ForUri(string.Empty),
            "an empty URI is refused");
    }

    private static void Draft()
    {
        Harness.Section("draft: one draft, changed options only");

        var view = TestSchema.PinyinView();
        var draft = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), view);
        Harness.Check(!draft.HasChanges, "a fresh draft has no changes");
        Harness.CheckEqual(0, draft.BuildChangeSet().Children.Count,
            "a no-change save has an empty change set (nothing to send)");

        Harness.Check(draft.TrySet(view, "PageSize", SettingsValue.Text("9")),
            "a schema option is editable");
        Harness.Check(draft.HasChanges, "the edit is pending");
        var changeSet = draft.BuildChangeSet();
        Harness.CheckEqual(1, changeSet.Children.Count, "only the changed option is in the change set");
        Harness.CheckEqual("9", changeSet.Children["PageSize"].Scalar, "the change set carries the new value");
        Harness.Check(!changeSet.Children.ContainsKey("Learning"),
            "an untouched option is not submitted");

        Harness.Check(!draft.TrySet(view, "ShuangpinProfile", SettingsValue.Text("abc")),
            "an option this view does not describe cannot be edited here");
        Harness.Check(!draft.TrySet(view, "FirstRun", SettingsValue.Text("True")),
            "a value with no schema in this view cannot be edited here");

        draft.TrySet(view, "PageSize", SettingsValue.Text("7"));
        Harness.Check(!draft.HasChanges, "setting a value back to the confirmed one is not a change");
        Harness.CheckEqual(0, draft.BuildChangeSet().Children.Count, "and it empties the change set");

        Harness.Section("draft: nested configuration");
        Harness.Check(draft.TrySetNested(view, "Fuzzy$FuzzyConfig", "VE_UE", SettingsValue.Text("False")),
            "a nested option is editable through its type");
        var nested = draft.BuildChangeSet();
        Harness.Check(nested.Children.ContainsKey("Fuzzy"), "the change set keeps the nesting");
        Harness.CheckEqual("False", nested.Children["Fuzzy"].Children["VE_UE"].Scalar,
            "the nested value is carried");
        Harness.Check(!nested.Children["Fuzzy"].Children.ContainsKey("Inner"),
            "an untouched nested sibling is not submitted");
        Harness.Check(!draft.TrySetNested(view, "Fuzzy$FuzzyConfig", "Nope", SettingsValue.Text("1")),
            "an unknown nested option is refused");
        Harness.Check(!draft.TrySetNested(view, "Hotkey$HotkeyConfig", "TriggerKeys",
                SettingsValue.Text("Control+space")),
            "a nested type this view does not contain is refused");
    }

    private static void DraftLifecycle()
    {
        Harness.Section("draft: save, adopt, reconnect, stale replies");

        var view = TestSchema.PinyinView();
        var draft = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), view);
        draft.TrySet(view, "PageSize", SettingsValue.Text("9"));
        draft.TrySet(view, "Learning", SettingsValue.Text("False"));
        var revisionSent = draft.Revision;

        var confirmedValues = view.Values
            .WithPath(new[] { "PageSize" }, SettingsValue.Text("9"))
            .WithPath(new[] { "Learning" }, SettingsValue.Text("False"));
        draft.AdoptConfirmed(confirmedValues);
        Harness.Check(!draft.HasChanges, "confirmed edits leave nothing pending");
        Harness.CheckEqual("9", draft.Effective("PageSize").Scalar, "the confirmed value is now the baseline");
        Harness.Check(!draft.IsCurrent(revisionSent), "the reply's revision is no longer current");

        Harness.Section("draft: partial success keeps the rest");
        var partial = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), view);
        partial.TrySet(view, "PageSize", SettingsValue.Text("9"));
        partial.TrySet(view, "Learning", SettingsValue.Text("False"));
        partial.AdoptConfirmed(view.Values.WithPath(new[] { "PageSize" }, SettingsValue.Text("9")));
        Harness.CheckEqual(1, partial.PendingPaths.Count, "the unconfirmed edit is still pending");
        Harness.Check(partial.PendingPaths.Contains("Learning"), "and it is the one not applied");

        Harness.Section("draft: reconnect refreshes the baseline without touching drafts");
        var reconnecting = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), view);
        reconnecting.TrySet(view, "PageSize", SettingsValue.Text("9"));
        var fresh = view.Values
            .WithPath(new[] { "PageSize" }, SettingsValue.Text("8"))
            .WithPath(new[] { "Learning" }, SettingsValue.Text("False"));
        reconnecting.RefreshBaseline(fresh);
        Harness.CheckEqual("9", reconnecting.Effective("PageSize").Scalar,
            "the unsaved edit survives the reconnect");
        Harness.CheckEqual("False", reconnecting.Effective("Learning").Scalar,
            "an untouched field follows the new baseline");
        Harness.Check(reconnecting.HasChanges, "the edit is still pending after the refresh");

        Harness.Section("draft: revert");
        reconnecting.Revert("PageSize");
        Harness.Check(!reconnecting.HasChanges, "reverting the only edit clears the change set");
        reconnecting.TrySet(view, "PageSize", SettingsValue.Text("6"));
        reconnecting.RevertAll();
        Harness.Check(!reconnecting.HasChanges, "revert all discards every pending edit");
    }

    /// <summary>
    /// The two Pinyin views share one draft and one value baseline, and keep their own
    /// schemas. These are the cases that a draft storing a schema would fail.
    /// </summary>
    private static void SharedViews()
    {
        Harness.Section("shared draft: one draft, two views, two schemas");

        var pinyin = TestSchema.PinyinView();
        var shuangpin = TestSchema.ShuangpinView();
        var draft = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), pinyin);

        Harness.CheckEqual(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin").Id,
            ConfigIdentity.ForUri("fcitx://config/inputmethod/shuangpin").Id,
            "both views address the same draft identity");
        Harness.Check(draft.BaselineValues.Equals(pinyin.Values),
            "the baseline holds values only, so it is the same baseline for both views");

        Harness.Section("shared draft: a shared edit is visible through the other view");
        Harness.Check(draft.TrySet(pinyin, "PageSize", SettingsValue.Text("9")),
            "the pinyin view edits a shared option");
        Harness.CheckEqual("9", draft.Effective("PageSize").Scalar,
            "the shared draft reports the edit");
        Harness.CheckEqual("9", draft.BuildChangeSet().Children["PageSize"].Scalar,
            "and it is what a save from either view would submit");

        Harness.Section("shared draft: the Shuangpin view edits its own field");
        Harness.Check(draft.TrySet(shuangpin, "ShuangpinProfile", SettingsValue.Text("abc")),
            "the shuangpin view can edit ShuangpinProfile");
        Harness.CheckEqual("abc", draft.Effective("ShuangpinProfile").Scalar,
            "the value lands in the one shared draft");
        Harness.CheckEqual("abc", draft.BuildChangeSet().Children["ShuangpinProfile"].Scalar,
            "and is submitted from the same change set");

        Harness.Section("shared draft: the Pinyin view cannot touch a Shuangpin-only field");
        Harness.Check(!draft.TrySet(pinyin, "ShuangpinProfile", SettingsValue.Text("zzz")),
            "the pinyin view refuses the field its schema does not describe");
        Harness.CheckEqual("abc", draft.Effective("ShuangpinProfile").Scalar,
            "and the existing value is untouched");

        Harness.Section("shared draft: adopting a reply from one view keeps the other's schema");
        // The Host replies through the shuangpin URI: same values, shuangpin schema.
        var shuangpinReply = TestSchema.ShuangpinView().Values
            .WithPath(new[] { "PageSize" }, SettingsValue.Text("9"));
        draft.AdoptConfirmed(new SettingsSnapshot(shuangpinReply, shuangpin.Types));
        Harness.CheckEqual("9", draft.Effective("PageSize").Scalar, "the shared value was adopted");
        Harness.Check(!draft.TrySet(pinyin, "ShuangpinProfile", SettingsValue.Text("zzz")),
            "the pinyin view still refuses the field after a shuangpin reply");
        Harness.Check(draft.TrySet(shuangpin, "ShuangpinProfile", SettingsValue.Text("ms")),
            "the shuangpin view still accepts it");

        Harness.Section("shared draft: refreshing from one view keeps the other's schema");
        draft.RefreshBaseline(TestSchema.ShuangpinView());
        Harness.Check(!draft.TrySet(pinyin, "ShuangpinProfile", SettingsValue.Text("zzz")),
            "a refresh through the shuangpin view does not widen the pinyin view");
        Harness.Check(draft.TrySet(pinyin, "Learning", SettingsValue.Text("False")),
            "the pinyin view still edits the options it does describe");

        Harness.Section("shared draft: nested edits follow the asking view's schema");
        Harness.Check(draft.TrySetNested(pinyin, "Fuzzy$FuzzyConfig", "Inner", SettingsValue.Text("False")),
            "the pinyin view edits a nested option");
        Harness.Check(draft.TrySetNested(shuangpin, "Fuzzy$FuzzyConfig", "VE_UE", SettingsValue.Text("False")),
            "the shuangpin view edits the same nested container");
        var changeSet = draft.BuildChangeSet();
        Harness.CheckEqual("False", changeSet.Children["Fuzzy"].Children["Inner"].Scalar,
            "both nested edits live in one change set");
        Harness.CheckEqual("False", changeSet.Children["Fuzzy"].Children["VE_UE"].Scalar,
            "with one entry per option");
        Harness.Check(!SettingsDraft.IsKnownNestedOption(pinyin, "Fuzzy$FuzzyConfig", "NotAnOption"),
            "an unknown nested option is not described by either view");

        Harness.Section("shared draft: partial success across the two views");
        var shared = SettingsDraft.FromSnapshot(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"), pinyin);
        shared.TrySet(pinyin, "PageSize", SettingsValue.Text("9"));
        shared.TrySet(shuangpin, "ShuangpinProfile", SettingsValue.Text("abc"));
        // The Host applied only PageSize.
        shared.AdoptConfirmed(pinyin.Values.WithPath(new[] { "PageSize" }, SettingsValue.Text("9")));
        Harness.CheckEqual(1, shared.PendingPaths.Count, "the unapplied edit stays pending");
        Harness.Check(shared.PendingPaths.Contains("ShuangpinProfile"),
            "and it is the shuangpin-specific one");
    }

    private static void SchemaHelpers()
    {
        Harness.Section("schema: description helpers read the Host, not a second schema");

        var view = TestSchema.PinyinView();
        var pageSize = view.FindOption("PageSize");
        Harness.Check(pageSize is not null, "the option is found");
        Harness.CheckEqual(3, pageSize!.IntMinimum, "IntMin is read");
        Harness.CheckEqual(10, pageSize.IntMaximum, "IntMax is read");

        var dictManager = view.FindOption("DictManager");
        Harness.CheckEqual("fcitx://config/addon/pinyin/dictmanager", dictManager!.ExternalUri,
            "an External option carries its URI");
        Harness.Check(!dictManager.LaunchesSubConfig, "and does not claim to launch a page");

        var punctuation = view.FindOption("Punctuation");
        Harness.Check(punctuation!.LaunchesSubConfig, "LaunchSubConfig is read");

        var fuzzy = view.FindOption("Fuzzy");
        Harness.Check(fuzzy!.IsNested, "a nested configuration is recognised");
        Harness.CheckEqual("Fuzzy$FuzzyConfig", fuzzy.Type, "the nested type name is kept");

        Harness.CheckEqual(13, view.SchemaOptionNames.Count,
            "every option of every type in the view is addressable (10 + 3 nested)");

        var shuangpin = TestSchema.ShuangpinView();
        Harness.Check(!shuangpin.SchemaOptionNames.SetEquals(view.SchemaOptionNames),
            "the two views do not expose the same options");
        Harness.Check(shuangpin.SchemaOptionNames.Contains("ShuangpinProfile"),
            "the shuangpin view exposes its own field");
        Harness.Check(!view.SchemaOptionNames.Contains("ShuangpinProfile"),
            "the pinyin view does not");
        Harness.CheckEqual(1, view.Types.Count(t => t.Name == "Fuzzy$FuzzyConfig"),
            "the nested type appears once");

        Harness.Section("schema: which view may carry which field");
        Harness.Check(view.Describes("PageSize"), "a root option is described");
        Harness.Check(view.Describes("Fuzzy/VE_UE"), "a nested option is described through its container");
        Harness.Check(!view.Describes("ShowShuangpinMode"),
            "a Shuangpin-only field is not described by the Pinyin view");
        Harness.Check(view.Describes("Fuzzy/Nope") == false, "an unknown nested option is not described");
        Harness.Check(!view.Describes("Nope"), "an unknown root option is not described");
        Harness.Check(!view.Describes("Nope/VE_UE"), "nor through a container that is not there");
        Harness.Check(shuangpin.Describes("ShowShuangpinMode"),
            "the Shuangpin view does describe it");
    }

    /// <summary>
    /// The write shape of a <c>List|String</c> option, which is not its read shape.
    /// </summary>
    /// <remarks>
    /// The Host writes a vector as an indexed map and reads an empty one back as the empty
    /// scalar, but on write it requires a map for every <c>List|</c> option and answers
    /// "A list-valued Fcitx option has an invalid value" to a scalar. These checks pin the three
    /// values that must stay distinct.
    /// </remarks>
    private static void StringLists()
    {
        Harness.Section("string list: an empty list is written as an empty map");

        var empty = StringListValue.ToValue(Array.Empty<string>());
        Harness.Check(empty.IsMap, "no lines is a map, not a scalar");
        Harness.CheckEqual(0, empty.Children.Count, "and it has no entries");

        var blank = StringListValue.ToValue(new[] { string.Empty });
        Harness.Check(blank.IsMap, "empty text is a map too");
        Harness.CheckEqual(0, blank.Children.Count, "with no entries, which is what an empty box means");

        var fromControl = StringListValue.ToValue(string.Empty.Split('\n'));
        Harness.Check(fromControl.IsMap, "the control's own empty text produces a map");
        Harness.CheckEqual(0, fromControl.Children.Count, "and no entries");

        Harness.Section("string list: entries keep their positions");

        var lines = StringListValue.ToValue(new[] { "a", string.Empty, "c" });
        Harness.Check(lines.IsMap, "a list of lines is a map");
        Harness.CheckEqual(3, lines.Children.Count, "every line is an entry");
        Harness.CheckEqual("a", lines.Children["0"].Scalar, "the first entry");
        Harness.CheckEqual(string.Empty, lines.Children["1"].Scalar,
            "an empty positional entry keeps its own index instead of being dropped");
        Harness.CheckEqual("c", lines.Children["2"].Scalar, "the third entry did not move up");

        var crlf = StringListValue.ToValue(new[] { "a\r", "b\r" });
        Harness.CheckEqual("a", crlf.Children["0"].Scalar, "a trailing CR is not part of the entry");
        Harness.CheckEqual("b", crlf.Children["1"].Scalar, "for every line");

        Harness.Section("string list: reading still accepts both shapes");

        Harness.CheckEqual(0, StringListValue.Lines(SettingsValue.Text(string.Empty)).Count,
            "the empty scalar the Host reads back is an empty list");
        Harness.CheckEqual(0, StringListValue.Lines(SettingsValue.EmptyMap()).Count,
            "so is an empty map");
        Harness.CheckEqual("a,,c", string.Join(",", StringListValue.Lines(lines)),
            "a map reads back in index order, with the empty entry still in its own place");
        Harness.CheckEqual("a,b",
            string.Join(",", StringListValue.Lines(SettingsValue.Text("a b"))),
            "a scalar written by an older client is split rather than shown as empty");

        Harness.Section("string list: the text a control shows round-trips exactly");

        var shown = StringListValue.ToText(lines);
        Harness.Check(shown.Contains("\r\n", StringComparison.Ordinal),
            "lines are separated by CRLF, which is what a WinUI text box keeps");
        Harness.Check(!shown.Replace("\r\n", string.Empty).Contains('\n'),
            "and never by a bare LF, which the control would collapse");
        var roundTrip = StringListValue.ToValue(StringListValue.SplitLines(shown));
        Harness.CheckEqual(string.Join("|", StringListValue.Lines(lines)),
            string.Join("|", StringListValue.Lines(roundTrip)),
            "so the text a control shows converts back to the same list");
        Harness.CheckEqual(3, StringListValue.Lines(roundTrip).Count,
            "with every entry still present");

        Harness.Section("string list: every line break a control can give back");

        Harness.CheckEqual(2, StringListValue.SplitLines("a\r\nb").Length, "CRLF splits");
        Harness.CheckEqual(2, StringListValue.SplitLines("a\rb").Length,
            "CR alone splits — a text box reads text set with CRLF back this way");
        Harness.CheckEqual(2, StringListValue.SplitLines("a\nb").Length, "LF alone splits");
        Harness.CheckEqual(0, StringListValue.SplitLines(string.Empty).Length, "empty text has no lines");
        Harness.CheckEqual(4, StringListValue.SplitLines("a\r\nb\rc\nd").Length,
            "a mixture is split at every break");
        Harness.CheckEqual(4,
            StringListValue.Lines(StringListValue.ToValue(
                StringListValue.SplitLines(".(/|@)$\r^(www|bbs|forum|mail)\\.\r^(http|https|ftp|telnet|mailto):\r^https?://x/"))).Count,
            "four patterns stored with CR separators are four entries, not one");

        Harness.Section("string list: a scalar option is not reinterpreted");

        var scalar = SettingsValue.Text(string.Empty);
        Harness.Check(!scalar.IsMap, "an empty string stays an empty scalar");
        Harness.Check(!scalar.Equals(empty),
            "and it is not the same value as an empty list, so a string option is untouched");
    }
}
