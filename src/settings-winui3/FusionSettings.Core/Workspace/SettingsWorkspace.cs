using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;

namespace Fusion.Settings.Workspace;

/// <summary>One configuration the window can edit.</summary>
public enum SettingsTarget
{
    Pinyin,
    Shuangpin,
    Global,

    /// <summary>
    /// The Windows candidate-presentation configuration: a Fusion-owned Fcitx
    /// configuration that holds presentation only (today the layout direction).
    /// </summary>
    Presentation,
}

/// <summary>
/// The window's state and its edit/save/recovery path: one connection to the Host, one draft
/// per underlying configuration, and the result of the last operation.
/// </summary>
/// <remarks>
/// <para>
/// This lives in the core rather than the app because it has no UI dependency, and because
/// the path it implements — routing an edit, splitting a save across the schemas that permit
/// it, reconciling an uncertain write — is the part that has to be tested against a real Host.
/// </para>
/// <para>
/// Two rules from the core drive the shape here. Pinyin and Shuangpin are one configuration
/// with two schemas, so there is exactly one draft for both and the schema used for an edit is
/// the one belonging to the view the user is looking at. Global configuration is a different
/// object and gets its own draft.
/// </para>
/// <para>
/// Nothing here activates an input method. Choosing which method to edit only changes the
/// schema the editor renders; selecting the active method is a separate, explicit operation.
/// </para>
/// </remarks>
public sealed class SettingsWorkspace : INotifyPropertyChanged
{
    public const string PinyinUri = "fcitx://config/inputmethod/pinyin";
    public const string ShuangpinUri = "fcitx://config/inputmethod/shuangpin";
    public const string GlobalUri = "fcitx://config/global";

    /// <summary>
    /// The Windows candidate-presentation configuration. Owner authorization 2026-10-22: a
    /// Windows-specific presentation setting may have its own configuration, while official
    /// input semantics must not acquire duplicate stores. The Host owns and reads/writes the
    /// value; this window only edits it through the Host.
    /// </summary>
    public const string PresentationUri = "fcitx://config/candidate-presentation";

    /// <summary>
    /// What the window says while a save is waiting for the Host to be free. It says what is
    /// true — nothing is on disk — and it does not name a cause, because the Host's
    /// <c>Busy</c> covers both a live composition and another request in flight.
    /// </summary>
    public const string WaitingForHostText = "尚未保存，等待当前输入结束或 Host 空闲后自动重试。";

    /// <summary>How long one wait lasts before the next attempt.</summary>
    private static readonly TimeSpan HostRetryInterval = TimeSpan.FromMilliseconds(400);

    /// <summary>
    /// A ceiling on the whole wait, so a Host that never becomes free ends in a stated
    /// "still unsaved" rather than in a window that looks frozen. 150 attempts is a minute of
    /// uninterrupted composition.
    /// </summary>
    private const int MaxHostWaitAttempts = 150;

    private readonly SettingsClient _client;
    private readonly List<string> _log = new();
    private readonly object _operationGate = new();

    private SettingsStatus _status = SettingsStatus.Unreachable;
    private string _statusText = "尚未连接 Host。";
    private bool _isBusy;
    private CancellationTokenSource? _hostWait;
    private bool _waitingForHost;
    private ulong _saveIncarnation;
    private bool _hostChangedDuringSave;
    private TaskCompletionSource _idle = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private bool _isLoaded;
    private bool _isConnected;
    private string _editingMethod = "pinyin";
    private string _saveSummary = string.Empty;

    public SettingsWorkspace(string endpoint)
    {
        Endpoint = endpoint;
        _client = new SettingsClient(endpoint, TimeSpan.FromSeconds(6));
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Endpoint { get; }

    public bool IsProductionEndpoint => _client.IsProductionEndpoint;

    public SettingsStatus Status
    {
        get => _status;
        private set => Set(ref _status, value);
    }

    public string StatusText
    {
        get => _statusText;
        private set => Set(ref _statusText, value);
    }

    /// <summary>True while an operation owns the Host; editing is disabled meanwhile.</summary>
    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (Set(ref _isBusy, value))
            {
                Raise(nameof(CanEdit));
            }
        }
    }

    /// <summary>Editing is allowed only when nothing is in flight.</summary>
    public bool CanEdit => !_isBusy;

    /// <summary>
    /// True while a save is waiting for the Host to be free. Nothing has been written; the
    /// bar is not allowed to say otherwise, and the wait can be stopped.
    /// </summary>
    public bool IsWaitingForHost => _waitingForHost;

    /// <summary>
    /// Stops the automatic retries. The draft stays unsaved, and a request that has already
    /// been sent is not abandoned: the save in flight finishes and reports its own outcome,
    /// because claiming to have cancelled a write the Host may be performing would be a lie.
    /// </summary>
    public void StopWaiting() => _hostWait?.Cancel();

    /// <summary>
    /// Completes when no operation is running. Used before a reload so the wait is stopped
    /// first and the new state is not raced by a late reply.
    /// </summary>
    public Task WhenIdleAsync()
    {
        lock (_operationGate)
        {
            // Nothing is running, so there is nothing to wait for. Returning the next gate would
            // wait for an operation that may never start.
            return IsBusy ? _idle.Task : Task.CompletedTask;
        }
    }

    /// <summary>True once a load has produced state this window can show.</summary>
    /// <remarks>
    /// This is deliberately not "the last operation succeeded". A failed reconnect keeps the
    /// loaded state on screen — the drafts are still there and still worth saving — so clearing
    /// this on failure would hide a page that is still valid.
    /// </remarks>
    public bool IsLoaded
    {
        get => _isLoaded;
        private set => Set(ref _isLoaded, value);
    }

    /// <summary>
    /// True while the Host is answering. It goes false when a load or a save cannot reach the
    /// Host, which is a different fact from "nothing was ever loaded": a connection lost after a
    /// successful load still has drafts to preserve.
    /// </summary>
    public bool IsConnected
    {
        get => _isConnected;
        private set
        {
            if (Set(ref _isConnected, value))
            {
                Raise(nameof(NeedsRetry));
            }
        }
    }

    /// <summary>The method whose schema the editor is showing. Not the active method.</summary>
    public string EditingMethod
    {
        get => _editingMethod;
        private set
        {
            if (Set(ref _editingMethod, value))
            {
                Raise(nameof(EditingMethodLabel));
            }
        }
    }

    public string EditingMethodLabel => EditingMethod == "shuangpin" ? "双拼" : "拼音";

    public string SaveSummary
    {
        get => _saveSummary;
        private set => Set(ref _saveSummary, value);
    }

    public SettingsSnapshot? PinyinSnapshot { get; private set; }

    public SettingsSnapshot? ShuangpinSnapshot { get; private set; }

    public SettingsSnapshot? GlobalSnapshot { get; private set; }

    public SettingsSnapshot? PresentationSnapshot { get; private set; }

    public SettingsDraft? PinyinDraft { get; private set; }

    public SettingsDraft? GlobalDraft { get; private set; }

    public SettingsDraft? PresentationDraft { get; private set; }

    public ImListDraft? Methods { get; private set; }

    public SettingsTarget CurrentTarget =>
        EditingMethod == "shuangpin" ? SettingsTarget.Shuangpin : SettingsTarget.Pinyin;

    /// <summary>The schema the editor should use for a target.</summary>
    /// <remarks>
    /// Every target is named explicitly. An unknown target resolves to nothing rather than
    /// to some other configuration, because silently falling through would let a control
    /// write to a draft it does not belong to — the defect this routing exists to prevent.
    /// </remarks>
    public SettingsSnapshot? SnapshotFor(SettingsTarget target) => target switch
    {
        SettingsTarget.Pinyin => PinyinSnapshot,
        SettingsTarget.Shuangpin => ShuangpinSnapshot,
        SettingsTarget.Global => GlobalSnapshot,
        SettingsTarget.Presentation => PresentationSnapshot,
        _ => null,
    };

    public SettingsDraft? DraftFor(SettingsTarget target) => target switch
    {
        SettingsTarget.Global => GlobalDraft,
        SettingsTarget.Presentation => PresentationDraft,
        SettingsTarget.Pinyin => PinyinDraft,
        SettingsTarget.Shuangpin => PinyinDraft,
        _ => null,
    };

    public bool HasChanges =>
        (PinyinDraft?.HasChanges ?? false) || (GlobalDraft?.HasChanges ?? false) ||
        (PresentationDraft?.HasChanges ?? false) || (Methods?.HasChanges ?? false);

    public IReadOnlyList<string> Diagnostics => _log;

    /// <summary>True when a retry is meaningful: nothing loaded yet, or the Host went away.</summary>
    public bool NeedsRetry => !IsConnected;

    /// <summary>True once drafts exist, which is when a reconnect has something to preserve.</summary>
    public bool HasDrafts => PinyinDraft is not null;

    /// <summary>
    /// The first load. Creates the drafts, so it is only used when there is nothing to lose;
    /// a reconnect uses <see cref="ReloadAsync"/> instead.
    /// </summary>
    public async Task<bool> InitializeAsync(CancellationToken cancellationToken = default)
    {
        if (!TryBeginOperation())
        {
            return false;
        }

        try
        {
            StatusText = "正在读取 Host 配置…";
            return await LoadAllAsync(recreateDrafts: true, cancellationToken).ConfigureAwait(true);
        }
        finally
        {
            EndOperation();
        }
    }

    /// <summary>
    /// The visible retry action: reconnect without losing work.
    /// </summary>
    /// <remarks>
    /// Before the first successful load there are no drafts, so the retry has to create them.
    /// After one there are, and the retry must not: a connection lost with edits pending is
    /// exactly the case where recreating the drafts would throw the user's work away. The
    /// choice is made from the state, not from the button that was pressed.
    /// </remarks>
    public Task<bool> RetryAsync(CancellationToken cancellationToken = default) =>
        ReloadAsync(preserveEdits: HasDrafts, cancellationToken);

    /// <summary>
    /// Re-reads the Host without losing work.
    /// </summary>
    /// <param name="preserveEdits">
    /// True for a reconnect or a retry: pending edits stay and only the baseline moves. False
    /// for an explicit reload, where the caller has already asked the user about discarding.
    /// </param>
    public async Task<bool> ReloadAsync(bool preserveEdits, CancellationToken cancellationToken = default)
    {
        // A waiting save is stopped before the state is replaced: a late retry must not write a
        // draft the reload is discarding. The wait ends at the next attempt boundary, and a
        // request already sent is awaited rather than abandoned.
        StopWaiting();
        await WhenIdleAsync().ConfigureAwait(true);
        if (!TryBeginOperation())
        {
            return false;
        }

        try
        {
            StatusText = preserveEdits ? "正在重新读取 Host 配置…" : "正在重新加载…";
            return await LoadAllAsync(recreateDrafts: !preserveEdits, cancellationToken)
                .ConfigureAwait(true);
        }
        finally
        {
            EndOperation();
        }
    }

    private async Task<bool> LoadAllAsync(bool recreateDrafts, CancellationToken cancellationToken)
    {
        try
        {
            // The Host allows one settings request at a time and answers Busy to a second, so
            // these are sequential and stop at the first failure.
            var pinyin = await _client.GetConfigAsync(PinyinUri, cancellationToken).ConfigureAwait(true);
            if (!pinyin.IsOk)
            {
                return Fail(pinyin.Status, pinyin.Error);
            }

            var shuangpin = await _client.GetConfigAsync(ShuangpinUri, cancellationToken).ConfigureAwait(true);
            if (!shuangpin.IsOk)
            {
                return Fail(shuangpin.Status, shuangpin.Error);
            }

            var global = await _client.GetConfigAsync(GlobalUri, cancellationToken).ConfigureAwait(true);
            if (!global.IsOk)
            {
                return Fail(global.Status, global.Error);
            }

            var presentation = await _client.GetConfigAsync(PresentationUri, cancellationToken)
                .ConfigureAwait(true);
            if (!presentation.IsOk)
            {
                return Fail(presentation.Status, presentation.Error);
            }

            var methods = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
            if (!methods.IsOk)
            {
                return Fail(methods.Status, methods.Error);
            }

            // One load must come from one Host. If the incarnation moved between replies, the
            // snapshots describe different processes and must not be adopted as one state.
            var incarnation = pinyin.HostIncarnation;
            if (shuangpin.HostIncarnation != incarnation || global.HostIncarnation != incarnation ||
                presentation.HostIncarnation != incarnation ||
                methods.HostIncarnation != incarnation)
            {
                Log("load rejected: replies came from different Host incarnations");
                Status = SettingsStatus.Unreachable;
                StatusText = "Host 在读取过程中被替换，已放弃这次读取。请重试。";
                IsConnected = false;
                IsLoaded = HasDrafts;
                return false;
            }

            PinyinSnapshot = pinyin.Value;
            ShuangpinSnapshot = shuangpin.Value;
            GlobalSnapshot = global.Value;
            PresentationSnapshot = presentation.Value;

            if (recreateDrafts)
            {
                PinyinDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(PinyinUri), pinyin.Value!);
                GlobalDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(GlobalUri), global.Value!);
                PresentationDraft = SettingsDraft.FromSnapshot(
                    ConfigIdentity.ForUri(PresentationUri), presentation.Value!);
                Methods = new ImListDraft(methods.Value!);
            }
            else
            {
                // A refresh moves the baseline. Configuration drafts keep their pending edits; the
                // method list decides per draft, because a list nobody edited must follow the Host
                // rather than be reported as a local edit.
                PinyinDraft?.RefreshBaseline(pinyin.Value!);
                GlobalDraft?.RefreshBaseline(global.Value!);
                PresentationDraft?.RefreshBaseline(presentation.Value!);
                MergeMethods(methods.Value!);
            }

            if (!ConfigIdentity.ForUri(PinyinUri).SharesStorageWith(ConfigIdentity.ForUri(ShuangpinUri)))
            {
                Log("warning: pinyin and shuangpin identities diverged");
            }

            Status = SettingsStatus.Ok;
            StatusText = $"已连接 Host（incarnation {incarnation}）。";
            IsConnected = true;
            IsLoaded = true;
            Log($"loaded: pinyin={pinyin.Value!.Types.Count} types, global={global.Value!.Types.Count} types");
            return true;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception)
        {
            return Fail(SettingsStatus.Unreachable, exception.Message);
        }
        finally
        {
            Raise(nameof(HasChanges));
            Raise(nameof(HasDrafts));
        }
    }

    private bool Fail(SettingsStatus status, string error)
    {
        Status = status;
        StatusText = Describe(status, error);
        Log($"load failed: status={status} error={error}");
        IsConnected = false;

        // Whatever was loaded before is still on screen and still editable, so a failed
        // reconnect does not pretend the window is empty.
        IsLoaded = HasDrafts;
        return false;
    }

    /// <summary>Switches which method's schema is edited. Does not activate the method.</summary>
    public void EditMethod(string method)
    {
        if (method is not ("pinyin" or "shuangpin"))
        {
            return;
        }

        EditingMethod = method;
    }

    /// <summary>
    /// Edits a top-level option of <paramref name="target"/>. The schema used is that target's,
    /// so a Global control can never be applied to the Pinyin draft.
    /// </summary>
    public bool TrySet(SettingsTarget target, string optionName, SettingsValue value)
    {
        var snapshot = SnapshotFor(target);
        var draft = DraftFor(target);
        if (snapshot is null || draft is null || IsBusy)
        {
            return false;
        }

        var accepted = draft.TrySet(snapshot, optionName, value);
        if (accepted)
        {
            Raise(nameof(HasChanges));
        }

        return accepted;
    }

    /// <summary>Edits a leaf inside a nested configuration of <paramref name="target"/>.</summary>
    public bool TrySetNested(SettingsTarget target, string nestedTypeName, string optionName,
                             SettingsValue value)
    {
        var snapshot = SnapshotFor(target);
        var draft = DraftFor(target);
        if (snapshot is null || draft is null || IsBusy)
        {
            return false;
        }

        var accepted = draft.TrySetNested(snapshot, nestedTypeName, optionName, value);
        if (accepted)
        {
            Raise(nameof(HasChanges));
        }

        return accepted;
    }

    // --- input-method list ---------------------------------------------------------
    // These go through the workspace so the save bar and the close guard see them.

    public bool AddMethod(string name)
    {
        if (IsBusy || Methods is null || !Methods.Add(name))
        {
            return false;
        }

        Raise(nameof(HasChanges));
        return true;
    }

    public bool RemoveMethod(string name)
    {
        if (IsBusy || Methods is null || !Methods.Remove(name))
        {
            return false;
        }

        Raise(nameof(HasChanges));
        return true;
    }

    public bool MoveMethod(int from, int to)
    {
        if (IsBusy || Methods is null || !Methods.Move(from, to))
        {
            return false;
        }

        Raise(nameof(HasChanges));
        return true;
    }

    public void Discard()
    {
        // An operation owns the drafts while it runs; clearing them underneath it would change
        // the answer a request already in flight is being reconciled against.
        if (IsBusy)
        {
            Report("正在与 Host 通信，暂时不能放弃修改。");
            return;
        }

        PinyinDraft?.RevertAll();
        GlobalDraft?.RevertAll();
        PresentationDraft?.RevertAll();
        Methods?.Revert();
        SaveSummary = "已放弃未保存的修改。";
        Raise(nameof(HasChanges));
    }

    /// <summary>
    /// Puts a short message in the save bar without touching a draft. Used for outcomes that are
    /// not a save result, such as refusing to close while a request is in flight.
    /// </summary>
    public void Report(string message) => SaveSummary = message;

    /// <summary>
    /// Saves every configuration that changed and reports each result separately, in one user
    /// action.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The shared Pinyin/Shuangpin draft is split by which view's schema describes each field,
    /// because the Host validates a submitted field against the URI's own schema: sending
    /// <c>ShuangpinProfile</c> through the Pinyin URI is refused. An uncertain outcome is never
    /// replayed — the state is reread and the edits the reread confirms are dropped.
    /// </para>
    /// <para>
    /// <c>Busy</c> is the one answer this Host gives without having written anything: the two
    /// pages whose only official setter publishes by applying — the Pinyin/Shuangpin
    /// configuration and the input-method list — are refused while a buffer is live, and the
    /// request lands nowhere. So instead of making the user come back and click again, the same
    /// save waits a short interval and resubmits what is still unsaved, until the Host is free.
    /// Nothing else is retried: a timeout, a dropped connection, a cancellation, an
    /// <c>Indeterminate</c> outcome or an <c>Invalid</c> rejection is handled exactly as it was.
    /// </para>
    /// <para>
    /// Each attempt submits only the drafts that are still dirty, so a target that already
    /// succeeded is never sent twice, and an edit made while waiting is what the next attempt
    /// sends. The wait is off the UI thread and cancellable; a request already sent is awaited
    /// to its own end, because cancelling the read would turn a real write into an unknown one.
    /// </para>
    /// </remarks>
    public async Task SaveAsync(CancellationToken cancellationToken = default)
    {
        if (!TryBeginOperation())
        {
            SaveSummary = "上一次操作还在进行，请稍候。";
            return;
        }

        using var wait = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _hostWait = wait;
        _saveIncarnation = 0;
        _hostChangedDuringSave = false;
        var reports = new List<SaveReport>();
        var busy = new HashSet<string>(StringComparer.Ordinal);
        var stopped = string.Empty;
        try
        {
            // The first pass is the user's save: every changed page is submitted once. Later
            // passes exist for one reason only — a page that answered `Busy` — so they are
            // given that set and touch nothing else.
            await SavePassAsync(reports, busy, only: null, cancellationToken).ConfigureAwait(true);
            var attempts = 0;
            while (busy.Count > 0)
            {
                if (wait.IsCancellationRequested)
                {
                    stopped = "已停止自动重试，仍有未保存的修改。";
                    break;
                }

                if (attempts >= MaxHostWaitAttempts)
                {
                    stopped = "等待 Host 空闲超时，仍有未保存的修改。";
                    break;
                }

                // Waiting is reported as waiting: nothing has been written, so the bar must not
                // say the change is saved, and the drafts stay dirty for the next attempt.
                var composed = Compose(reports);
                SaveSummary = composed.Contains(WaitingForHostText, StringComparison.Ordinal)
                    ? composed
                    : composed + "\n" + WaitingForHostText;
                SetWaitingForHost(true);
                bool waited;
                try
                {
                    waited = await WaitForHostAsync(wait.Token).ConfigureAwait(true);
                }
                finally
                {
                    SetWaitingForHost(false);
                }

                if (!waited)
                {
                    stopped = "已停止自动重试，仍有未保存的修改。";
                    break;
                }

                ++attempts;
                // A snapshot, because this pass rewrites `busy` as it goes: a page that answers
                // something other than Busy leaves the set, and one that is still busy stays.
                var retry = new HashSet<string>(busy, StringComparer.Ordinal);
                await SavePassAsync(reports, busy, retry, cancellationToken).ConfigureAwait(true);
                if (_hostChangedDuringSave)
                {
                    // A different Host answered. Writing to it would apply an intent formed
                    // against another instance's state, so the wait ends here.
                    stopped = "Host 已重新启动，已停止自动重试；修改已保留。";
                    break;
                }
            }

            SaveSummary = reports.Count == 0 && stopped.Length == 0
                ? "没有需要保存的修改。"
                : string.Join("\n", new[] { Compose(reports), stopped }
                    .Where(line => line.Length > 0));
        }
        finally
        {
            _hostWait = null;
            SetWaitingForHost(false);
            EndOperation();
        }
    }

    /// <summary>
    /// One pass over the targets, submitting only the drafts that are still dirty.
    /// </summary>
    /// <remarks>
    /// <paramref name="only"/> is null on the first pass — the user's save submits every changed
    /// page — and on a retry it is exactly the pages that answered <c>Busy</c> in the previous
    /// pass. Nothing else is submitted again: a page that failed with <c>Error</c>, was rejected
    /// as <c>Invalid</c>, came back <c>Indeterminate</c> or was cancelled keeps its draft, and it
    /// is not re-sent merely because some other page is still waiting for the Host. A retried
    /// page's draft was never adopted, so it is still dirty here and its change set is rebuilt
    /// from the current values rather than replayed from the first attempt. The two Pinyin views
    /// are one draft submitted as two requests, so they are two pages here: one group answering
    /// <c>Busy</c> may not drag the other group's failed fields back to the Host.
    /// </remarks>
    private async Task SavePassAsync(List<SaveReport> reports, HashSet<string> busy,
                                     IReadOnlySet<string>? only,
                                     CancellationToken cancellationToken)
    {
        await SavePinyinFamilyAsync(reports, busy, only, cancellationToken).ConfigureAwait(true);

        if (Wants(only, SavePage.Global) && GlobalDraft is { HasChanges: true } globalDraft)
        {
            Record(reports, busy, SavePage.Global, await SaveConfigurationAsync(
                GlobalUri, SettingsTarget.Global, globalDraft.BuildChangeSet(),
                globalDraft.PendingPaths.ToHashSet(StringComparer.Ordinal), "全局配置",
                cancellationToken).ConfigureAwait(true));
        }

        if (Wants(only, SavePage.Presentation) &&
            PresentationDraft is { HasChanges: true } presentationDraft)
        {
            Record(reports, busy, SavePage.Presentation, await SaveConfigurationAsync(
                PresentationUri, SettingsTarget.Presentation, presentationDraft.BuildChangeSet(),
                presentationDraft.PendingPaths.ToHashSet(StringComparer.Ordinal), "候选窗口",
                cancellationToken).ConfigureAwait(true));
        }

        if (Wants(only, SavePage.Methods) && Methods is { HasChanges: true } methods)
        {
            Record(reports, busy, SavePage.Methods,
                await SaveMethodsAsync(methods, cancellationToken).ConfigureAwait(true));
        }
    }

    /// <summary>
    /// Keeps one line per target: a retried target replaces the sentence of its earlier attempt
    /// instead of adding a second one.
    /// </summary>
    private static void Record(List<SaveReport> reports, HashSet<string> busy, string page,
                               SaveOutcome outcome)
    {
        reports.RemoveAll(report => report.Page == page);
        reports.Add(new SaveReport(page, outcome.Text));
        if (outcome.RetryWhenFree)
        {
            busy.Add(page);
        }
        else
        {
            busy.Remove(page);
        }
    }

    /// <summary>
    /// Records which Host instance answered <c>Busy</c>, and reports whether a later answer came
    /// from a different one — in which case the wait must not continue against it.
    /// </summary>
    private bool NoteBusyIncarnation(ulong incarnation)
    {
        if (_saveIncarnation == 0)
        {
            _saveIncarnation = incarnation;
            return false;
        }

        if (_saveIncarnation != incarnation)
        {
            _hostChangedDuringSave = true;
            return true;
        }

        return false;
    }

    private void SetWaitingForHost(bool waiting)
    {
        if (_waitingForHost == waiting)
        {
            return;
        }

        _waitingForHost = waiting;
        Raise(nameof(IsWaitingForHost));
    }

    private static string Compose(List<SaveReport> reports) =>
        string.Join("\n", reports.Select(report => report.Text));

    private static async Task<bool> WaitForHostAsync(CancellationToken token)
    {
        try
        {
            await Task.Delay(HostRetryInterval, token).ConfigureAwait(true);
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
    }


    /// <summary>
    /// Splits the one shared draft across the two URIs whose schemas describe its fields, and
    /// submits each part separately. A field neither view describes cannot be pending, because
    /// the draft refuses it; if one appears anyway it is reported rather than sent.
    /// </summary>
    private async Task SavePinyinFamilyAsync(List<SaveReport> reports, HashSet<string> busy,
                                             IReadOnlySet<string>? only,
                                             CancellationToken cancellationToken)
    {
        if (PinyinDraft is not { HasChanges: true } draft)
        {
            return;
        }

        var pending = draft.PendingPaths.ToList();
        var viaPinyin = pending
            .Where(path => PinyinSnapshot?.Describes(path) == true)
            .ToHashSet(StringComparer.Ordinal);
        var viaShuangpin = pending
            .Where(path => !viaPinyin.Contains(path) && ShuangpinSnapshot?.Describes(path) == true)
            .ToHashSet(StringComparer.Ordinal);
        var orphaned = pending
            .Where(path => !viaPinyin.Contains(path) && !viaShuangpin.Contains(path))
            .ToList();

        // Only the first pass reports a routing problem: it is not something a retry can fix.
        if (orphaned.Count > 0 && only is null)
        {
            // Should not happen: the draft refuses an option the asking view does not
            // describe. Report it instead of guessing a URI.
            Log("save: refused to send fields no view describes: " + string.Join(",", orphaned));
            // Not a refusal to retry: nothing can ever route these.
            Record(reports, busy, SavePage.PinyinOrphan,
                new SaveOutcome($"拼音/双拼：有 {orphaned.Count} 项无法确定归属，未提交。", false));
        }

        if (viaPinyin.Count > 0 && Wants(only, SavePage.Pinyin))
        {
            Record(reports, busy, SavePage.Pinyin, await SaveConfigurationAsync(
                PinyinUri, SettingsTarget.Pinyin, draft.BuildChangeSet(viaPinyin), viaPinyin,
                "拼音/双拼", cancellationToken).ConfigureAwait(true));
        }

        if (viaShuangpin.Count > 0 && Wants(only, SavePage.Shuangpin))
        {
            Record(reports, busy, SavePage.Shuangpin, await SaveConfigurationAsync(
                ShuangpinUri, SettingsTarget.Shuangpin, draft.BuildChangeSet(viaShuangpin),
                viaShuangpin, "双拼", cancellationToken).ConfigureAwait(true));
        }
    }

    private async Task<SaveOutcome> SaveConfigurationAsync(string uri, SettingsTarget target,
                                                           SettingsValue changeSet,
                                                           IReadOnlySet<string> sentPaths,
                                                           string name,
                                                           CancellationToken cancellationToken)
    {
        var draft = DraftFor(target)!;
        var result = await _client.SetConfigAsync(uri, changeSet, cancellationToken).ConfigureAwait(true);

        switch (result.Status)
        {
            case SettingsStatus.Ok:
                // A successful apply is authoritative for exactly what was sent.
                draft.AdoptConfirmed(result.Value!, sentPaths);
                SetSnapshot(target, result.Value!);
                MarkConnected();
                Log($"save {uri}: ok");
                return new SaveOutcome($"{name}：已保存。", false);

            case SettingsStatus.Indeterminate:
                // The Host may have applied it. Reread and reconcile; never replay.
                var reread = await _client.GetConfigAsync(uri, cancellationToken).ConfigureAwait(true);
                if (!reread.IsOk)
                {
                    Log($"save {uri}: indeterminate, reread failed {reread.Status}");
                    MarkDisconnected(reread.Status, reread.Error);
                    return new SaveOutcome(
                        $"{name}：写入结果不确定，可能已生效；重新读取也失败了，修改已保留，请手动重新加载核对。",
                        false);
                }

                // Adopting the reread drops exactly the edits it confirms and keeps the rest.
                draft.AdoptConfirmed(reread.Value!);
                SetSnapshot(target, reread.Value!);
                MarkConnected();
                Log($"save {uri}: indeterminate, reconciled from a reread");
                return new SaveOutcome(
                    $"{name}：写入结果不确定，可能已生效。已重新读取当前配置并据此核对。", false);

            case SettingsStatus.Busy:
                // The Host wrote nothing, so this is a wait, not a failure: the caller retries
                // this same draft when the Host is free again.
                Log($"save {uri}: busy ({result.HostIncarnation})");
                if (NoteBusyIncarnation(result.HostIncarnation))
                {
                    return new SaveOutcome($"{name}：{WaitingForHostText}", false);
                }

                return new SaveOutcome($"{name}：{WaitingForHostText}", true);

            default:
                Log($"save {uri}: {result.Status} {result.Error}");
                NoteIfConnectionLost(result);
                return new SaveOutcome(
                    $"{name}：未保存（{Describe(result.Status, result.Error)}）。修改已保留。",
                    false);
        }
    }

    private void SetSnapshot(SettingsTarget target, SettingsSnapshot snapshot)
    {
        switch (target)
        {
            case SettingsTarget.Pinyin:
                PinyinSnapshot = snapshot;
                break;
            case SettingsTarget.Shuangpin:
                ShuangpinSnapshot = snapshot;
                break;
            case SettingsTarget.Presentation:
                PresentationSnapshot = snapshot;
                break;
            case SettingsTarget.Global:
                GlobalSnapshot = snapshot;
                break;
        }
    }

    private async Task<SaveOutcome> SaveMethodsAsync(ImListDraft methods,
                                                      CancellationToken cancellationToken)
    {
        var submitted = methods.BuildEnabledList();
        var result = await _client.SetImListAsync(submitted, cancellationToken).ConfigureAwait(true);
        switch (result.Status)
        {
            case SettingsStatus.Ok:
                Methods = new ImListDraft(result.Value!);
                Log("save im-list: ok");
                return new SaveOutcome("输入方式列表：已保存。", false);

            case SettingsStatus.Indeterminate:
                // The Host may have applied the list. The reread is authoritative for what it
                // now holds, but it is not evidence that the request was dropped: the order the
                // user asked for stays pending unless the reread shows it. Nothing is replayed.
                var fresh = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
                if (!fresh.IsOk)
                {
                    Log("save im-list: indeterminate, reread failed");
                    MarkDisconnected(fresh.Status, fresh.Error);
                    return new SaveOutcome(
                        "输入方式列表：写入结果不确定，可能已生效；重新读取也失败了，修改仍保留，请重试连接后核对。",
                        false);
                }

                MarkConnected();
                methods.RefreshBaseline(fresh.Value!);
                if (methods.HasChanges)
                {
                    Log("save im-list: indeterminate, reread shows the request was not applied");
                    return new SaveOutcome(
                        "输入方式列表：写入结果不确定；重新读取显示这次修改尚未生效，修改仍保留，可再次保存。",
                        false);
                }

                Log("save im-list: indeterminate, reread shows the request was applied");
                return new SaveOutcome("输入方式列表：写入结果不确定，但重新读取显示已生效。", false);

            case SettingsStatus.Busy:
                // Nothing was written: the switch is stored by `setGroup`, which ends the
                // composition, so the Host refuses it while a buffer is live. Retried below.
                Log($"save im-list: busy ({result.HostIncarnation})");
                if (NoteBusyIncarnation(result.HostIncarnation))
                {
                    return new SaveOutcome($"输入方式列表：{WaitingForHostText}", false);
                }

                return new SaveOutcome($"输入方式列表：{WaitingForHostText}", true);

            default:
                Log($"save im-list: {result.Status} {result.Error}");
                NoteIfConnectionLost(result);
                return new SaveOutcome(
                    $"输入方式列表：未保存（{Describe(result.Status, result.Error)}）。修改已保留。",
                    false);
        }
    }

    /// <summary>
    /// Keeps a refresh from discarding list edits without inventing one. The baseline moves to the
    /// fresh read; a pending order stays pending, and a list with nothing pending adopts the Host's
    /// order instead of turning the cached one into a local edit.
    /// </summary>
    private void MergeMethods(ImList fresh)
    {
        if (Methods is null)
        {
            Methods = new ImListDraft(fresh);
            return;
        }

        Methods.RefreshBaseline(fresh);
    }

    /// <summary>A short, user-facing sentence for a status.</summary>
    public static string Describe(SettingsStatus status, string error) => status switch
    {
        SettingsStatus.Ok => "正常。",
        SettingsStatus.Busy => "Host 暂时没有接受这次请求：正在组合中，或另一次请求正在进行。",
        SettingsStatus.Unsupported => string.IsNullOrEmpty(error) ? "Host 不支持该操作。" : error,
        SettingsStatus.Invalid => string.IsNullOrEmpty(error) ? "请求无效。" : error,
        SettingsStatus.Timeout => "Host 没有在限定时间内响应。",
        SettingsStatus.Indeterminate => "写入结果不确定，可能已生效。",
        SettingsStatus.VersionMismatch => string.IsNullOrEmpty(error)
            ? "Host 的协议版本与设置程序不匹配。"
            : error,
        SettingsStatus.Unreachable => string.IsNullOrEmpty(error)
            ? "无法连接 FusionHost，请确认它正在运行。"
            : $"无法连接 FusionHost：{error}",
        _ => string.IsNullOrEmpty(error) ? "未知错误。" : error,
    };

    private void MarkConnected()
    {
        if (!IsConnected)
        {
            IsConnected = true;
            Status = SettingsStatus.Ok;
            StatusText = "已重新连接 Host。";
        }
    }

    /// <summary>
    /// Records that the Host could not be reached. Only the statuses that actually mean "no
    /// usable connection" do this: a refusal (<c>Busy</c>, <c>Invalid</c>, a schema rejection)
    /// proves the Host answered, so it must not offer a reconnect.
    /// </summary>
    private void MarkDisconnected(SettingsStatus status, string error)
    {
        if (status is not (SettingsStatus.Unreachable or SettingsStatus.Timeout))
        {
            return;
        }

        IsConnected = false;
        Status = status;
        StatusText = Describe(status, error);
        Log($"connection lost: status={status} error={error}");
    }

    private void NoteIfConnectionLost<T>(SettingsResult<T> result) =>
        MarkDisconnected(result.Status, result.Error);

    private bool TryBeginOperation()
    {
        lock (_operationGate)
        {
            if (IsBusy)
            {
                return false;
            }

            IsBusy = true;
            return true;
        }
    }

    private void EndOperation()
    {
        IsBusy = false;
        var idle = _idle;
        _idle = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        idle.TrySetResult();
        Raise(nameof(HasChanges));
        Raise(nameof(NeedsRetry));
    }

    private void Log(string line)
    {
        _log.Add($"{DateTime.Now:HH:mm:ss} {line}");
        if (_log.Count > 200)
        {
            _log.RemoveAt(0);
        }
    }

    /// <summary>
    /// The pages a save can submit. They are also the keys of the retry set, so a page is
    /// retried only when the key it records its outcome under is the key that was refused.
    /// </summary>
    private static class SavePage
    {
        public const string Pinyin = "拼音/双拼";
        public const string Shuangpin = "双拼";
        public const string Global = "全局配置";
        public const string Presentation = "候选窗口";
        public const string Methods = "输入方式列表";

        /// <summary>Fields no view's schema describes. Reported once, never submitted.</summary>
        public const string PinyinOrphan = "拼音/双拼，一部分字段";
    }

    /// <summary>
    /// Whether this pass may submit <paramref name="page"/>. The first pass may submit
    /// everything; a retry may submit only what the previous pass reported as <c>Busy</c>.
    /// </summary>
    private static bool Wants(IReadOnlySet<string>? only, string page) =>
        only is null || only.Contains(page);

    /// <summary>One page's sentence in the save bar; one per page, replaced on a retry.</summary>
    private sealed record SaveReport(string Page, string Text);

    /// <summary>
    /// The outcome of saving one page, and whether it is worth another attempt. Only a
    /// <c>Busy</c> answer sets <c>RetryWhenFree</c>: it is the one status that guarantees the
    /// Host wrote nothing.
    /// </summary>
    private sealed record SaveOutcome(string Text, bool RetryWhenFree);

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        Raise(name);
        return true;
    }

    private void Raise(string? name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
