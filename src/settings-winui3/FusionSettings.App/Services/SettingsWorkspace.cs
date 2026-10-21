using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;

namespace Fusion.Settings.App.Services;

/// <summary>One configuration the window can edit.</summary>
public enum SettingsTarget
{
    Pinyin,
    Shuangpin,
    Global,
}

/// <summary>
/// The window's state: one connection to the Host, one draft per underlying
/// configuration, and the result of the last save.
/// </summary>
/// <remarks>
/// <para>
/// Two rules from the core drive the shape here. Pinyin and Shuangpin are one
/// configuration with two schemas, so there is exactly one draft for both and the
/// schema used for an edit is the one belonging to the view the user is looking at.
/// Global configuration is a different object and gets its own draft.
/// </para>
/// <para>
/// Nothing here activates an input method. Choosing which method to edit only changes
/// the schema the editor renders; selecting the active method is a separate, explicit
/// operation the Host guards with its own composition check.
/// </para>
/// </remarks>
public sealed class SettingsWorkspace : INotifyPropertyChanged
{
    public const string PinyinUri = "fcitx://config/inputmethod/pinyin";
    public const string ShuangpinUri = "fcitx://config/inputmethod/shuangpin";
    public const string GlobalUri = "fcitx://config/global";

    private readonly SettingsClient _client;
    private readonly List<string> _log = new();

    private SettingsStatus _status = SettingsStatus.Unreachable;
    private string _statusText = "尚未连接 Host。";
    private bool _isBusy;
    private bool _isLoaded;
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

    public bool IsBusy
    {
        get => _isBusy;
        private set => Set(ref _isBusy, value);
    }

    public bool IsLoaded
    {
        get => _isLoaded;
        private set => Set(ref _isLoaded, value);
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

    public string EditingMethodLabel =>
        EditingMethod == "shuangpin" ? "双拼" : "拼音";

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

    /// <summary>The schema the editor should use for the current target.</summary>
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

    public SettingsTarget CurrentTarget =>
        EditingMethod == "shuangpin" ? SettingsTarget.Shuangpin : SettingsTarget.Pinyin;

    public bool HasChanges =>
        (PinyinDraft?.HasChanges ?? false) || (GlobalDraft?.HasChanges ?? false) ||
        (Methods?.HasChanges ?? false);

    public IReadOnlyList<string> Diagnostics => _log;

    /// <summary>Loads every configuration the window shows. Safe to call again after a failure.</summary>
    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        IsBusy = true;
        StatusText = "正在读取 Host 配置…";
        try
        {
            // The Host allows one settings request at a time and answers Busy to a second,
            // so these are sequential and stop at the first failure: on an unreachable Host
            // that costs one timeout, not four.
            var pinyin = await _client.GetConfigAsync(PinyinUri, cancellationToken).ConfigureAwait(true);
            if (!pinyin.IsOk)
            {
                Fail(pinyin.Status, pinyin.Error);
                return;
            }

            var shuangpin = await _client.GetConfigAsync(ShuangpinUri, cancellationToken).ConfigureAwait(true);
            if (!shuangpin.IsOk)
            {
                Fail(shuangpin.Status, shuangpin.Error);
                return;
            }

            var global = await _client.GetConfigAsync(GlobalUri, cancellationToken).ConfigureAwait(true);
            if (!global.IsOk)
            {
                Fail(global.Status, global.Error);
                return;
            }

            var methods = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
            if (!methods.IsOk)
            {
                Fail(methods.Status, methods.Error);
                return;
            }

            PinyinSnapshot = pinyin.Value;
            ShuangpinSnapshot = shuangpin.Value;
            GlobalSnapshot = global.Value;
            PinyinDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(PinyinUri), pinyin.Value!);
            GlobalDraft = SettingsDraft.FromSnapshot(ConfigIdentity.ForUri(GlobalUri), global.Value!);
            Methods = new ImListDraft(methods.Value!);

            // Both URIs must resolve to one configuration; if a Host ever disagreed, the
            // shared draft would be wrong, so say so instead of silently editing one view.
            if (!ConfigIdentity.ForUri(PinyinUri).SharesStorageWith(ConfigIdentity.ForUri(ShuangpinUri)))
            {
                Log("warning: pinyin and shuangpin identities diverged");
            }

            Status = SettingsStatus.Ok;
            StatusText = $"已连接 Host（incarnation {pinyin.HostIncarnation}）。";
            IsLoaded = true;
            Log($"loaded: pinyin={pinyin.Value!.Types.Count} types, global={global.Value!.Types.Count} types");
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception)
        {
            Status = SettingsStatus.Unreachable;
            StatusText = Describe(SettingsStatus.Unreachable, exception.Message);
            Log($"load threw: {exception.GetType().Name}: {exception.Message}");
            IsLoaded = false;
        }
        finally
        {
            IsBusy = false;
            Raise(nameof(HasChanges));
        }
    }

    private void Fail(SettingsStatus status, string error)
    {
        Status = status;
        StatusText = Describe(status, error);
        Log($"load failed: status={status} error={error}");
        IsLoaded = false;
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

    public bool TrySet(string optionName, SettingsValue value)
    {
        var snapshot = SnapshotFor(CurrentTarget);
        var draft = DraftFor(CurrentTarget);
        if (snapshot is null || draft is null)
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

    public bool TrySetNested(string nestedTypeName, string optionName, SettingsValue value)
    {
        var snapshot = SnapshotFor(CurrentTarget);
        var draft = DraftFor(CurrentTarget);
        if (snapshot is null || draft is null)
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

    public void Discard()
    {
        PinyinDraft?.RevertAll();
        GlobalDraft?.RevertAll();
        Methods?.Revert();
        SaveSummary = "已放弃未保存的修改。";
        Raise(nameof(HasChanges));
    }

    /// <summary>
    /// Saves every configuration that changed, one request each, and reports each result.
    /// </summary>
    /// <remarks>
    /// A refusal keeps the edits for that configuration. An <see cref="SettingsStatus.Indeterminate"/>
    /// outcome is not retried: the state is reread and reconciled, and the user is told the
    /// write may have applied.
    /// </remarks>
    public async Task SaveAsync(CancellationToken cancellationToken = default)
    {
        IsBusy = true;
        var reports = new List<string>();
        try
        {
            if (PinyinDraft is { HasChanges: true } pinyinDraft)
            {
                reports.Add(await SaveConfigurationAsync(
                    PinyinUri, PinyinDraft, pinyinDraft.BuildChangeSet(), cancellationToken)
                    .ConfigureAwait(true));
            }

            if (GlobalDraft is { HasChanges: true } globalDraft)
            {
                reports.Add(await SaveConfigurationAsync(
                    GlobalUri, GlobalDraft, globalDraft.BuildChangeSet(), cancellationToken)
                    .ConfigureAwait(true));
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
            IsBusy = false;
            Raise(nameof(HasChanges));
        }
    }

    private async Task<string> SaveConfigurationAsync(string uri, SettingsDraft draft,
                                                      SettingsValue changeSet,
                                                      CancellationToken cancellationToken)
    {
        var result = await _client.SetConfigAsync(uri, changeSet, cancellationToken).ConfigureAwait(true);
        var name = uri == GlobalUri ? "全局配置" : "拼音/双拼";

        switch (result.Status)
        {
            case SettingsStatus.Ok:
                draft.AdoptConfirmed(result.Value!);
                Log($"save {uri}: ok");
                return $"{name}：已保存。";

            case SettingsStatus.Indeterminate:
                // The Host may have applied it. Reread and reconcile; never replay.
                await ReconcileAsync(draft, uri, cancellationToken).ConfigureAwait(true);
                Log($"save {uri}: indeterminate, reconciled");
                return $"{name}：写入结果不确定，可能已生效。已重新读取当前配置，请核对后再决定。";

            case SettingsStatus.Busy:
                Log($"save {uri}: busy");
                return $"{name}：当前正在输入或组合中，未保存。请完成输入后重试。";

            default:
                Log($"save {uri}: {result.Status} {result.Error}");
                return $"{name}：未保存（{Describe(result.Status, result.Error)}）。修改已保留。";
        }
    }

    private async Task ReconcileAsync(SettingsDraft draft, string uri, CancellationToken cancellationToken)
    {
        var fresh = await _client.GetConfigAsync(uri, cancellationToken).ConfigureAwait(true);
        if (fresh.IsOk)
        {
            draft.RefreshBaseline(fresh.Value!);
            if (uri == PinyinUri)
            {
                PinyinSnapshot = fresh.Value;
                ShuangpinSnapshot = await RefreshShuangpinAsync(cancellationToken).ConfigureAwait(true);
            }
            else
            {
                GlobalSnapshot = fresh.Value;
            }
        }
    }

    private async Task<SettingsSnapshot?> RefreshShuangpinAsync(CancellationToken cancellationToken)
    {
        var fresh = await _client.GetConfigAsync(ShuangpinUri, cancellationToken).ConfigureAwait(true);
        return fresh.IsOk ? fresh.Value : ShuangpinSnapshot;
    }

    private async Task<string> SaveMethodsAsync(ImListDraft methods, CancellationToken cancellationToken)
    {
        var result = await _client.SetImListAsync(methods.BuildEnabledList(), cancellationToken)
            .ConfigureAwait(true);
        switch (result.Status)
        {
            case SettingsStatus.Ok:
                Methods = new ImListDraft(result.Value!);
                Log("save im-list: ok");
                return "输入方式列表：已保存。";
            case SettingsStatus.Indeterminate:
                var fresh = await _client.GetImListAsync(cancellationToken).ConfigureAwait(true);
                if (fresh.IsOk)
                {
                    Methods = new ImListDraft(fresh.Value!);
                }

                Log("save im-list: indeterminate, reread");
                return "输入方式列表：写入结果不确定，可能已生效。已重新读取，请核对。";
            case SettingsStatus.Busy:
                return "输入方式列表：当前正在输入或组合中，未保存。";
            default:
                Log($"save im-list: {result.Status} {result.Error}");
                return $"输入方式列表：未保存（{Describe(result.Status, result.Error)}）。修改已保留。";
        }
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
