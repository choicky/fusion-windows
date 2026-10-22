using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// What the save bar says when one page is stored and another is not.
/// </summary>
/// <remarks>
/// <para>
/// <see cref="SettingsWorkspace.SaveAsync"/> submits each changed configuration separately and
/// reports each result, so "partly saved" is a real outcome the user has to be able to read:
/// the pages that were written say so, the page that was refused keeps its edit, and nothing
/// claims more than happened. This is checked on the real write path rather than by inspecting
/// strings: the candidate-presentation file is held open, so Fcitx's rename cannot replace it,
/// while the global configuration in the same save succeeds.
/// </para>
/// <para>
/// It needs <c>FUSION_SETTINGS_GLOBAL_PROFILE</c>, the profile root the Host was pointed at, to
/// find the presentation file.
/// </para>
/// </remarks>
internal static class PartialSaveTests
{
    private const string PresentationOption = "CandidateLayout";
    private const string PresentationFileName = "candidate-presentation.conf";
    private const string BehaviorType = "Behavior$BehaviorConfig";
    private const string GlobalOption = "DefaultPageSize";

    public static async Task RunAsync(string endpoint, string profileRoot,
                                      CancellationToken cancellationToken)
    {
        Harness.Section("save: a page that fails and a page that succeeds are reported separately");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the workspace loads");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var globalSnapshot = workspace.SnapshotFor(SettingsTarget.Global);
        var presentationSnapshot = workspace.SnapshotFor(SettingsTarget.Presentation);
        if (globalSnapshot is null || presentationSnapshot is null)
        {
            Harness.Check(false, "both configurations are described by the Host");
            return;
        }

        var pageSizeOption = globalSnapshot.Types
            .FirstOrDefault(type => string.Equals(type.Name, BehaviorType, StringComparison.Ordinal))
            ?.Options.FirstOrDefault(candidate =>
                string.Equals(candidate.Name, GlobalOption, StringComparison.Ordinal));
        Harness.Check(pageSizeOption is not null, "the global page-size option is in the schema");
        if (pageSizeOption is null)
        {
            return;
        }

        // Two edits in one save: a global value that can be written, and a presentation value
        // that cannot while the file is held.
        var currentPageSize = workspace.GlobalDraft!
            .EffectiveNested(globalSnapshot, BehaviorType, GlobalOption).Scalar;
        var nextPageSize = int.TryParse(currentPageSize, out var parsed) ? parsed + 1 : 6;
        if (pageSizeOption.IntMaximum is { } maximum && nextPageSize > maximum)
        {
            nextPageSize = pageSizeOption.IntMinimum ?? 1;
        }

        Harness.Check(workspace.TrySetNested(SettingsTarget.Global, BehaviorType, GlobalOption,
                SettingsValue.Text(nextPageSize.ToString(System.Globalization.CultureInfo.InvariantCulture))),
            "the page size is changed");
        var layoutBefore = workspace.PresentationDraft!.Effective(PresentationOption).Scalar;
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, PresentationOption,
                SettingsValue.Text(layoutBefore == "Horizontal" ? "Vertical" : "Horizontal")),
            "and the candidate layout is changed in the same save");

        var presentationFile = FindFile(profileRoot, PresentationFileName);
        Harness.Check(presentationFile is not null, "the presentation file is known");
        if (presentationFile is null)
        {
            return;
        }

        using (new FileStream(presentationFile, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        }

        Console.WriteLine($"   the save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(workspace.SaveSummary.Contains("全局配置：已保存", StringComparison.Ordinal),
            "the page that was written reports a save");
        Harness.Check(workspace.SaveSummary.Contains("候选窗口：", StringComparison.Ordinal) &&
                      workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the page that was refused reports it was not saved");
        Harness.Check(!workspace.GlobalDraft!.HasChanges,
            "the written page has nothing pending, because the Host confirmed what it stored");
        Harness.Check(workspace.PresentationDraft!.HasChanges,
            "and the refused page keeps its edit, so the user can retry it");

        // Once the file can be replaced, the same save finishes the second page and leaves
        // nothing pending — the retry is a real write, not a no-op reported as success.
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Console.WriteLine($"   the retried save said: {workspace.SaveSummary.Replace("\n", " / ")}");
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the retried save reports no refusal");
        Harness.Check(!workspace.PresentationDraft!.HasChanges, "and nothing is pending afterwards");

        // Put the two values back, so the pages after this section start where they did.
        Harness.Check(workspace.TrySet(SettingsTarget.Presentation, PresentationOption,
                SettingsValue.Text(layoutBefore)),
            "the layout is put back");
        Harness.Check(workspace.TrySetNested(SettingsTarget.Global, BehaviorType, GlobalOption,
                SettingsValue.Text(currentPageSize)),
            "the page size is put back");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.HasChanges, "and the workspace is back to an unmodified state");
    }

    private static string? FindFile(string root, string name) =>
        Directory.Exists(root)
            ? Directory.EnumerateFiles(root, name, SearchOption.AllDirectories).FirstOrDefault()
            : null;
}
