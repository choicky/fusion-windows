using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.Model;

namespace Fusion.Settings.Tests;

/// <summary>
/// The row shape that has to survive the wire, the draft and the editor: duplicates, order,
/// paired halves and Fcitx's silent row filters.
/// </summary>
internal static class PunctuationMapTests
{
    public static void Run()
    {
        Harness.Section("punctuation rows: order, duplicates, pairs and validation");

        var rows = new List<PunctuationRow>
        {
            new("#", "#", string.Empty),
            new("#", "＃", string.Empty),
            new("\"", "“", "”"),
            new("[", "【", string.Empty),
            new("[", "「", string.Empty),
        };
        for (var index = 0; index < 12; index++)
        {
            rows.Add(new PunctuationRow("x", "值" + index, string.Empty));
        }

        var value = PunctuationMap.Value(rows);
        Harness.Check(PunctuationMap.RowsFromOption(value).SequenceEqual(rows),
            "an indexed map round-trips in numeric order, including rows past index 9");

        var duplicate = PunctuationMap.RowsFromOption(value).Count(row => row.Key == "#");
        Harness.CheckEqual(2, duplicate, "two rows for one key stay two rows");
        Harness.Check(PunctuationMap.RowsFromOption(value)[2].AltMapping == "”",
            "the paired half stays on its row rather than becoming another row");

        Harness.Check(PunctuationMap.InvalidReason(rows) is null,
            "the shipped shape is accepted");
        Harness.Check(PunctuationMap.InvalidReason(new[] { new PunctuationRow("ab", "×", "") }) ==
                      "键必须是一个字符。",
            "a multi-character key is rejected before it can be silently dropped");
        Harness.Check(PunctuationMap.InvalidReason(new[] { new PunctuationRow("#", "", "") }) ==
                      "映射不能为空。",
            "an empty mapping is rejected");
        Harness.Check(PunctuationMap.InvalidReason(new[] { new PunctuationRow(" ", "#", "") }) is not null,
            "a whitespace key is rejected");
        Harness.Check(PunctuationMap.InvalidReason(new[] { new PunctuationRow("#", "a b", "") }) is not null,
            "a mapping Fcitx would split into two columns is rejected");

        var snapshot = TestSchema.PunctuationSnapshot();
        var defaults = PunctuationMap.Defaults(snapshot);
        Harness.Check(defaults.Count > 0, "defaults are read from the Host's schema");
        Harness.Check(defaults.Count(row => row.Key == "#") == 2,
            "including both shipped # candidates");
    }
}
