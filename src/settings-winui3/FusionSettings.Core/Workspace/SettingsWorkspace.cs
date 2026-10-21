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

    private readonly SettingsClient _client;
    private readonly List<string> _log = new();
    private readonly object _operationGate = new();

    private SettingsStatus _status = SettingsStatus.Unreachable;
    private string _statusText = "尚未连接 Host。";
    private bool _isBusy;
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

    public SettingsDraft? PinyinDraft { get; private set; }

    public SettingsDraft? GlobalDraft { get; private set; }

    public ImListDraft? Methods { get; private set; }

    public SettingsTarget CurrentTarget =>
        EditingMethod == "shuangpin" ? SettingsTarget.Shuangpin : SettingsTarget.Pinyin;

    /// <summary>The schema the editor should use for a target.</summary>
    public SettingsSnapshot? SnapshotFor(SettingsTarget target) => target switch
    {
        SettingsTarget.Pinyin => PinyinSnapshot,
        SettingsTarget.Shuangpin => ShuangpinSnapshot,
        _ => GlobalSnapshot,
    };

    public SettingsDraft? DraftFor(SettingsTarget target) => target switch
    {
        SettingsTarget.Global => GlobalDraft,
        _ => PinyinDraft,
    };

    public bool HasChanges =>
        (PinyinDraft?.HasChanges ?? false) || (GlobalDraft?.HasChanges ?? false) ||
        (Methods?.HasChanges ?? false);

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

            var methods = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
            if (!methods.IsOk)
            {
                return Fail(methods.Status, methods.Error);
            }

            // One load must come from one Host. If the incarnation moved between replies, the
            // four snapshots describe different processes and must not be adopted as one state.
            var incarnation = pinyin.HostIncarnation;
            if (shuangpin.HostIncarnation != incarnation || global.HostIncarnation != incarnation ||
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

            if (recreateDrafts)
            {
                PinyinDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(PinyinUri), pinyin.Value!);
                GlobalDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(GlobalUri), global.Value!);
                Methods = new ImListDraft(methods.Value!);
            }
            else
            {
                // A refresh moves the baseline. Configuration drafts keep their pending edits; the
                // method list decides per draft, because a list nobody edited must follow the Host
                // rather than be reported as a local edit.
                PinyinDraft?.RefreshBaseline(pinyin.Value!);
                GlobalDraft?.RefreshBaseline(global.Value!);
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
    /// Saves every configuration that changed and reports each result separately.
    /// </summary>
    /// <remarks>
    /// The shared Pinyin/Shuangpin draft is split by which view's schema describes each field,
    /// because the Host validates a submitted field against the URI's own schema: sending
    /// <c>ShuangpinProfile</c> through the Pinyin URI is refused. An uncertain outcome is never
    /// replayed — the state is reread and the edits the reread confirms are dropped.
    /// </remarks>
    public async Task SaveAsync(CancellationToken cancellationToken = default)
    {
        if (!TryBeginOperation())
        {
            SaveSummary = "上一次操作还在进行，请稍候。";
            return;
        }

        var reports = new List<string>();
        try
        {
            reports.AddRange(await SavePinyinFamilyAsync(cancellationToken).ConfigureAwait(true));

            if (GlobalDraft is { HasChanges: true } globalDraft)
            {
                reports.Add(await SaveConfigurationAsync(
                    GlobalUri, SettingsTarget.Global, globalDraft.BuildChangeSet(),
                    globalDraft.PendingPaths.ToHashSet(StringComparer.Ordinal), "全局配置",
                    cancellationToken).ConfigureAwait(true));
            }

            if (Methods is { HasChanges: true } methods)
            {
                reports.Add(await SaveMethodsAsync(methods, cancellationToken).ConfigureAwait(true));
            }

            SaveSummary = reports.Count == 0
                ? "没有需要保存的修改。"
                : string.Join("\n", reports);
        }
        finally
        {
            EndOperation();
        }
    }

    /// <summary>
    /// Splits the one shared draft across the two URIs whose schemas describe its fields, and
    /// submits each part separately. A field neither view describes cannot be pending, because
    /// the draft refuses it; if one appears anyway it is reported rather than sent.
    /// </summary>
    private async Task<List<string>> SavePinyinFamilyAsync(CancellationToken cancellationToken)
    {
        var reports = new List<string>();
        if (PinyinDraft is not { HasChanges: true } draft)
        {
            return reports;
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

        if (orphaned.Count > 0)
        {
            // Should not happen: the draft refuses an option the asking view does not
            // describe. Report it instead of guessing a URI.
            Log("save: refused to send fields no view describes: " + string.Join(",", orphaned));
            reports.Add($"拼音/双拼：有 {orphaned.Count} 项无法确定归属，未提交。");
        }

        if (viaPinyin.Count > 0)
        {
            reports.Add(await SaveConfigurationAsync(
                PinyinUri, SettingsTarget.Pinyin, draft.BuildChangeSet(viaPinyin), viaPinyin,
                "拼音/双拼", cancellationToken).ConfigureAwait(true));
        }

        if (viaShuangpin.Count > 0)
        {
            reports.Add(await SaveConfigurationAsync(
                ShuangpinUri, SettingsTarget.Shuangpin, draft.BuildChangeSet(viaShuangpin),
                viaShuangpin, "双拼", cancellationToken).ConfigureAwait(true));
        }

        return reports;
    }

    private async Task<string> SaveConfigurationAsync(string uri, SettingsTarget target,
                                                      SettingsValue changeSet,
                                                      IReadOnlySet<string> sentPaths, string name,
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
                return $"{name}：已保存。";

            case SettingsStatus.Indeterminate:
                // The Host may have applied it. Reread and reconcile; never replay.
                var reread = await _client.GetConfigAsync(uri, cancellationToken).ConfigureAwait(true);
                if (!reread.IsOk)
                {
                    Log($"save {uri}: indeterminate, reread failed {reread.Status}");
                    MarkDisconnected(reread.Status, reread.Error);
                    return $"{name}：写入结果不确定，可能已生效；重新读取也失败了，修改已保留，请手动重新加载核对。";
                }

                // Adopting the reread drops exactly the edits it confirms and keeps the rest.
                draft.AdoptConfirmed(reread.Value!);
                SetSnapshot(target, reread.Value!);
                MarkConnected();
                Log($"save {uri}: indeterminate, reconciled from a reread");
                return $"{name}：写入结果不确定，可能已生效。已重新读取当前配置并据此核对。";

            case SettingsStatus.Busy:
                Log($"save {uri}: busy");
                return $"{name}：当前正在输入或组合中，未保存。请完成输入后重试。";

            default:
                Log($"save {uri}: {result.Status} {result.Error}");
                NoteIfConnectionLost(result);
                return $"{name}：未保存（{Describe(result.Status, result.Error)}）。修改已保留。";
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
            default:
                GlobalSnapshot = snapshot;
                break;
        }
    }

    private async Task<string> SaveMethodsAsync(ImListDraft methods, CancellationToken cancellationToken)
    {
        var submitted = methods.BuildEnabledList();
        var result = await _client.SetImListAsync(submitted, cancellationToken).ConfigureAwait(true);
        switch (result.Status)
        {
            case SettingsStatus.Ok:
                Methods = new ImListDraft(result.Value!);
                Log("save im-list: ok");
                return "输入方式列表：已保存。";

            case SettingsStatus.Indeterminate:
                // The Host may have applied the list. The reread is authoritative for what it
                // now holds, but it is not evidence that the request was dropped: the order the
                // user asked for stays pending unless the reread shows it. Nothing is replayed.
                var fresh = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
                if (!fresh.IsOk)
                {
                    Log("save im-list: indeterminate, reread failed");
                    MarkDisconnected(fresh.Status, fresh.Error);
                    return "输入方式列表：写入结果不确定，可能已生效；重新读取也失败了，修改仍保留，请重试连接后核对。";
                }

                MarkConnected();
                methods.RefreshBaseline(fresh.Value!);
                if (methods.HasChanges)
                {
                    Log("save im-list: indeterminate, reread shows the request was not applied");
                    return "输入方式列表：写入结果不确定；重新读取显示这次修改尚未生效，修改仍保留，可再次保存。";
                }

                Log("save im-list: indeterminate, reread shows the request was applied");
                return "输入方式列表：写入结果不确定，但重新读取显示已生效。";

            case SettingsStatus.Busy:
                return "输入方式列表：当前正在输入或组合中，未保存。";

            default:
                Log($"save im-list: {result.Status} {result.Error}");
                NoteIfConnectionLost(result);
                return $"输入方式列表：未保存（{Describe(result.Status, result.Error)}）。修改已保留。";
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
        SettingsStatus.Busy => "Host 正在处理输入，暂时无法写入。",
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
