using System;
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
        SchemaHelpers();
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

        var draft = new SettingsDraft(ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"),
                                      TestSchema.PinyinView());
        Harness.Check(!draft.HasChanges, "a fresh draft has no changes");
        Harness.CheckEqual(0, draft.BuildChangeSet().Children.Count,
            "a no-change save has an empty change set (nothing to send)");

        Harness.Check(draft.TrySet("PageSize", SettingsValue.Text("9")), "a schema option is editable");
        Harness.Check(draft.HasChanges, "the edit is pending");
        var changeSet = draft.BuildChangeSet();
        Harness.CheckEqual(1, changeSet.Children.Count, "only the changed option is in the change set");
        Harness.CheckEqual("9", changeSet.Children["PageSize"].Scalar, "the change set carries the new value");
        Harness.Check(!changeSet.Children.ContainsKey("Learning"),
            "an untouched option is not submitted");

        Harness.Check(!draft.TrySet("ShuangpinProfile", SettingsValue.Text("abc")),
            "an option this view does not describe cannot be edited here");
        Harness.Check(!draft.TrySet("FirstRun", SettingsValue.Text("True")),
            "a value with no schema in this view cannot be edited here");

        draft.TrySet("PageSize", SettingsValue.Text("7"));
        Harness.Check(!draft.HasChanges, "setting a value back to the confirmed one is not a change");
        Harness.CheckEqual(0, draft.BuildChangeSet().Children.Count, "and it empties the change set");

        Harness.Section("draft: nested configuration");
        Harness.Check(draft.TrySetPath("Fuzzy$FuzzyConfig", "VE_UE", SettingsValue.Text("False")),
            "a nested option is editable through its type");
        var nested = draft.BuildChangeSet();
        Harness.Check(nested.Children.ContainsKey("Fuzzy"), "the change set keeps the nesting");
        Harness.CheckEqual("False", nested.Children["Fuzzy"].Children["VE_UE"].Scalar,
            "the nested value is carried");
        Harness.Check(!nested.Children["Fuzzy"].Children.ContainsKey("Inner"),
            "an untouched nested sibling is not submitted");
        Harness.Check(!draft.TrySetPath("Fuzzy$FuzzyConfig", "Nope", SettingsValue.Text("1")),
            "an unknown nested option is refused");

        Harness.Section("draft: two views of one configuration cannot diverge");
        var shuangpinView = new SettingsDraft(
            ConfigIdentity.ForUri("fcitx://config/inputmethod/shuangpin"), TestSchema.ShuangpinView());
        Harness.CheckEqual(draft.Identity.Id, shuangpinView.Identity.Id,
            "both views resolve to the same draft identity");
        Harness.Check(shuangpinView.TrySet("ShuangpinProfile", SettingsValue.Text("abc")),
            "the shuangpin view can edit its own field");
        Harness.Check(!draft.TrySet("ShuangpinProfile", SettingsValue.Text("zzz")),
            "the pinyin view cannot, so it cannot revert it either");
    }

    private static void DraftLifecycle()
    {
        Harness.Section("draft: save, adopt, reconnect, stale replies");

        var draft = new SettingsDraft(ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"),
                                      TestSchema.PinyinView());
        draft.TrySet("PageSize", SettingsValue.Text("9"));
        draft.TrySet("Learning", SettingsValue.Text("False"));
        var revisionSent = draft.Revision;

        // The Host confirmed the write and returns the resulting state.
        var confirmed = TestSchema.PinyinView();
        confirmed = new SettingsSnapshot(
            confirmed.Values
                .WithPath(new[] { "PageSize" }, SettingsValue.Text("9"))
                .WithPath(new[] { "Learning" }, SettingsValue.Text("False")),
            confirmed.Types);
        draft.AdoptConfirmed(confirmed);
        Harness.Check(!draft.HasChanges, "confirmed edits leave nothing pending");
        Harness.CheckEqual("9", draft.Effective("PageSize").Scalar, "the confirmed value is now the baseline");
        Harness.Check(!draft.IsCurrent(revisionSent), "the reply's revision is no longer current");

        Harness.Section("draft: partial success keeps the rest");
        var partial = new SettingsDraft(ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"),
                                        TestSchema.PinyinView());
        partial.TrySet("PageSize", SettingsValue.Text("9"));
        partial.TrySet("Learning", SettingsValue.Text("False"));
        var onlyOne = new SettingsSnapshot(
            TestSchema.PinyinView().Values.WithPath(new[] { "PageSize" }, SettingsValue.Text("9")),
            TestSchema.PinyinView().Types);
        partial.AdoptConfirmed(onlyOne);
        Harness.CheckEqual(1, partial.PendingPaths.Count, "the unconfirmed edit is still pending");
        Harness.Check(partial.PendingPaths.Contains("Learning"), "and it is the one not applied");

        Harness.Section("draft: reconnect refreshes the baseline without touching drafts");
        var reconnecting = new SettingsDraft(ConfigIdentity.ForUri("fcitx://config/inputmethod/pinyin"),
                                             TestSchema.PinyinView());
        reconnecting.TrySet("PageSize", SettingsValue.Text("9"));
        var fresh = new SettingsSnapshot(
            TestSchema.PinyinView().Values
                .WithPath(new[] { "PageSize" }, SettingsValue.Text("8"))
                .WithPath(new[] { "Learning" }, SettingsValue.Text("False")),
            TestSchema.PinyinView().Types);
        reconnecting.RefreshBaseline(fresh);
        Harness.CheckEqual("9", reconnecting.Effective("PageSize").Scalar,
            "the unsaved edit survives the reconnect");
        Harness.CheckEqual("False", reconnecting.Effective("Learning").Scalar,
            "an untouched field follows the new baseline");
        Harness.Check(reconnecting.HasChanges, "the edit is still pending after the refresh");

        Harness.Section("draft: revert");
        reconnecting.Revert("PageSize");
        Harness.Check(!reconnecting.HasChanges, "reverting the only edit clears the change set");
        reconnecting.TrySet("PageSize", SettingsValue.Text("6"));
        reconnecting.RevertAll();
        Harness.Check(!reconnecting.HasChanges, "revert all discards every pending edit");
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
    }
}
