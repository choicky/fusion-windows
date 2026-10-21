using System;
using System.Linq;
using Fusion.Settings.Client;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

internal static class ImListAndEndpointTests
{
    public static void Run()
    {
        Endpoints();
        ImList();
    }

    private static void Endpoints()
    {
        Harness.Section("endpoint: production name and normalisation");

        var production = PipeEndpoint.Production();
        Harness.Check(production is not null, "the production endpoint is computable");
        Harness.Check(production!.StartsWith("FusionHost-v1-S-1-", StringComparison.Ordinal),
            "it is the per-user per-session name");
        Harness.Check(char.IsDigit(production[^1]), "and it ends with the session id");

        Harness.CheckEqual("fusion-test", PipeEndpoint.Normalize("fusion-test"), "a bare name passes through");
        Harness.CheckEqual("fusion-test", PipeEndpoint.Normalize(@"\\.\pipe\fusion-test"),
            "the device path is stripped");
        Harness.CheckEqual("fusion-test", PipeEndpoint.Normalize(@"\\.\PIPE\fusion-test"),
            "the device path is matched case-insensitively");
        Harness.CheckEqual("  fusion-test  ", PipeEndpoint.Normalize("  fusion-test  "),
            "a name is not trimmed into a different name");
        Harness.Check(PipeEndpoint.Normalize("   ") is null, "whitespace only is refused");
        Harness.Check(PipeEndpoint.Normalize(string.Empty) is null, "an empty name is refused");
        Harness.Check(PipeEndpoint.Normalize(null) is null, "null is refused");
        Harness.Check(PipeEndpoint.Normalize(@"\\.\pipe\a\b") is null, "a second component is refused");
        Harness.Check(PipeEndpoint.Normalize("bad*name") is null, "an invalid character is refused");
        Harness.Check(PipeEndpoint.Normalize("bad\u0001name") is null, "a control character is refused");

        Harness.Check(PipeEndpoint.IsProduction(production), "the production name is recognised");
        Harness.Check(!PipeEndpoint.IsProduction("fusion-settings-probe-1"),
            "a test endpoint is not the production one");
        Harness.CheckEqual(@"\\.\pipe\x", PipeEndpoint.DevicePath("x"), "the device path is added once");

        Harness.Throws<ArgumentException>(() => new SettingsClient("   "),
            "a client refuses an unusable endpoint instead of falling back");
        Harness.Check(!new SettingsClient("fusion-test").IsProductionEndpoint,
            "a test client knows it is not on the production endpoint");
    }

    private static void ImList()
    {
        Harness.Section("input-method list: membership and order");

        var baseline = new ImList("pinyin", "pinyin", new[] { "pinyin", "shuangpin" },
                                  new[] { "pinyin", "shuangpin" });
        var draft = new ImListDraft(baseline);
        Harness.Check(!draft.HasChanges, "a fresh draft has no changes");
        Harness.CheckEqual("pinyin", draft.Active, "Items/0 is the active method");
        Harness.CheckEqual("pinyin", baseline.Fallback, "the fallback is separate and is not written");

        Harness.Check(!draft.Add("pinyin"), "an already-enabled method is not added twice");
        Harness.Check(!draft.Add("mozc"), "a method the runtime does not define is refused");

        Harness.Check(draft.Move(1, 0), "reordering is allowed");
        Harness.CheckEqual("shuangpin", draft.Active, "the new first entry becomes active");
        Harness.Check(draft.HasChanges, "reordering is a change");
        draft.Revert();
        Harness.CheckEqual("pinyin", draft.Active, "revert restores the order");
        Harness.Check(!draft.HasChanges, "and clears the change flag");

        Harness.Check(draft.Remove("shuangpin"), "membership can be removed");
        Harness.CheckEqual(1, draft.Enabled.Count, "the group shrank");
        Harness.Check(!draft.Remove("pinyin"), "the last member cannot be removed");
        Harness.CheckEqual(1, draft.Enabled.Count, "the group still has one member");

        var single = new ImListDraft(new ImList("pinyin", "pinyin", new[] { "pinyin" },
                                                new[] { "pinyin", "shuangpin" }));
        Harness.Check(!single.Remove("pinyin"), "an empty group is not a state the product offers");

        Harness.Section("input-method list: entries the runtime no longer defines");
        var stale = new ImListDraft(new ImList("pinyin", "pinyin",
                                               new[] { "pinyin", "retired" }, new[] { "pinyin" }));
        Harness.CheckEqual(1, stale.UndefinedEntries.Count, "the retired entry is reported");
        Harness.CheckEqual("retired", stale.UndefinedEntries[0], "and it is the one that is gone");
        Harness.CheckEqual(2, stale.Enabled.Count,
            "it is kept in the list rather than silently dropped");

        Harness.Section("input-method list: the submitted list is the ordered membership");
        var submit = new ImListDraft(new ImList("pinyin", "pinyin",
                                                new[] { "pinyin", "shuangpin" },
                                                new[] { "pinyin", "shuangpin" }));
        submit.Move(0, 1);
        Harness.CheckEqual("shuangpin,pinyin", string.Join(",", submit.BuildEnabledList()),
            "the order is what gets submitted");
    }
}
