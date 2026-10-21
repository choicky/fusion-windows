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
        SharedViews();
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
    }
}
