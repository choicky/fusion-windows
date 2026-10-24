# Fusion settings client (C# + WinUI 3)

The Windows settings surface, per D087: a Fusion-owned C# + WinUI 3 client over the
existing Host settings protocol. Official Fcitx configuration semantics, storage and
defaults are unchanged; the client is another consumer of `settings_protocol.hpp`, not a
new configuration path.

## Layout

| Path | What it is |
|---|---|
| `FusionSettings.Core/` | Transport, framing, codecs, endpoint naming, connection/handshake, draft/baseline model, key and input-method models, and the workspace that routes edits, splits saves and reconciles uncertain writes |
| `FusionSettings.Core.Tests/` | Focused checks plus real-Host checks over an isolated pipe |
| `FusionSettings.App/` | The WinUI 3 window: four pages, the key editor, the schema-driven option rows |

`FusionSettings.Core` targets `net10.0` and `net8.0`: the checks run on net10, the app on
net8, which is the runtime it is published against.

`FusionSettings.Core` has **no `PackageReference`** on purpose. That is what lets it build
and test with nothing but a .NET SDK, and it keeps the client's correctness-critical logic
independently verifiable.

## Behaviour the core guarantees

These are the properties the presentation may rely on, each covered by a check:

- **One draft, one value baseline, one schema per view.** `SettingsDraft` holds values only;
  every edit passes the requesting view's snapshot, so the Pinyin view cannot write a field
  only the Shuangpin view describes, and adopting a reply from either view cannot replace the
  other's schema.
- **Only changed options are submitted**, and a no-change save sends no request at all.
- **A list is written as an indexed map, empty or not.** The Host reads an empty list back as the
  empty scalar but refuses one on write, so `StringListValue` and `KeyBindingList.ToValue` emit a
  map and an empty map for an empty list. An empty scalar, an empty list and a list holding an
  empty positional entry stay three different values.
- **A write whose outcome is unknown says so.** Once the request frame has started, a lost,
  truncated, late or cancelled reply returns `Indeterminate` with
  `MutationMayHaveBeenApplied` — never "nothing was written". A failure before the frame
  starts is `Unreachable`. Nothing is ever replayed; the caller rereads and reconciles.
- **An uncertain method-list write keeps the request.** The reread becomes the baseline and the
  requested order stays pending unless the reread shows it applied, so "not applied" is never
  reported as saved.
- **A reconnect keeps pending edits, and only pending edits.** A configuration draft keeps its
  pending values. An input-method list the user actually reordered keeps the requested order while
  the baseline moves to the Host's. A list nobody edited adopts the Host's order instead: a cached
  list is not user intent, and keeping it would report a stale order as a local edit and let a
  later save submit a reversal. A retry after a first load that never succeeded creates the drafts
  it does not have, and losing the connection is tracked separately from having loaded, so the
  window offers the retry in both cases.
- **An unparsable key binding is preserved verbatim** (`KeyChord.IsOpaque`) instead of being
  replaced with an empty one, and empty entries keep their positions through a copy or a
  replacement.
- **A refusal says which page it is about, and never reports it as saved.** The save bar
  reports each configuration separately, so one page can say `已保存` while another says `未保存`
  in the same click. Which pages can be refused at all is decided by the Host from what its own
  apply step does, not by the client: the candidate-window and global pages are stored while a
  composition is live, and only the Pinyin/Shuangpin configuration and the input-method list are
  refused, because their official apply step ends the composition that is running. The edit is
  kept in every refusal.
- **The font list is the system's.** 候选字体 is an editable list of the families this session
  has, read from GDI's own enumeration — the same collection the popup draws with — and it never
  rewrites the stored value on open: a family this machine does not have keeps its name and the
  row says Windows will substitute.
- **One click does the waiting.** `Busy` is the one answer the Host gives without having written
  anything, so a save that receives it keeps the edit, says it is not saved yet, and resubmits
  **the pages that answered `Busy`** every 400 ms until the Host is free — serially, off the UI
  thread, and cancellable through 停止等待. Nothing else is retried: a timeout, a lost
  connection, a cancellation, an `Indeterminate` outcome or an `Invalid` rejection ends the save
  exactly as it did before, and that page keeps its edit unsent even while another page waits. A
  page that already succeeded is not sent again either, and a different Host incarnation ends the
  wait rather than receiving an intent formed against another instance.
- **A newer Host is accepted and an older one refused**, per the additive settings-API
  contract; the IPC protocol version is still matched exactly.

## Prerequisites

- **.NET SDK 10.0.402** (current LTS at the time of writing; SDK 10 is supported until
  2028-11-14). Any SDK that supports `net8.0` and `net10.0` works; this is the version the
  build and the checks were verified with.
- **`Microsoft.WindowsAppSDK` 2.5.1** and **`Microsoft.Windows.SDK.BuildTools` 10.0.28000.2705**
  from a NuGet feed. On a machine with normal nuget.org access the project files restore from
  there; in the environment this was built in a mirror was used as transport only, and every
  package was verified against the SHA-512 nuget.org publishes (`fusion-controller` →
  `evidence/settings-winui3-2026-10-21/blocker.md`).

The published app is **self-contained for both .NET and the Windows App SDK**, so it needs
nothing installed on the target machine. That is also why the package is large.

## Build and test

```powershell
# from the repository root
dotnet build src/settings-winui3/FusionSettings.Core.Tests/FusionSettings.Core.Tests.csproj
dotnet run   --project src/settings-winui3/FusionSettings.Core.Tests

# the app, and the folder the package ships
dotnet build   src/settings-winui3/FusionSettings.App/FusionSettings.App.csproj -c Release
dotnet publish src/settings-winui3/FusionSettings.App/FusionSettings.App.csproj -c Release -o <publish-dir>
```

`publish` does not carry the compiled XAML resource index, and the published app then fails
with "Cannot locate resource from ms-appx:///MainWindow.xaml". The project copies
`FusionSettings.pri` across in a post-publish target, so the published folder is complete.

Exit code 0 means every check passed, and the count depends on the runtime the Host is built
from. Against the enhanced runtime these documents are written against it is **846 checks**; against
the pinned `runtime-wp3-20261018` this workspace builds, which has no 辅助筛选 (`AuxiliaryFilter`),
the mixed-option section stops at that assertion and the run prints **814**, with that one check
failing — the option is not in the engine there, which is not a client defect. **575 of the 846**
(**585 of the 814**) need no Host (`--no-integration`), and **271 more run against one**
 (the protocol checks, a controlled pipe peer for mutation outcomes and handshake versions, the
workspace path — Global routing, a Shuangpin-only save, clearing the last binding, clearing a string list, the method list,
a failed start and its retry — a scripted peer for an uncertain method-list write, for the three reconnect cases of the
list, and for the cases of a save that meets a busy Host (busy twice then stored with no second click, a saved page not
resubmitted, the wait reported as unsaved and stoppable, an ordinary refusal that ends in one request, a page that failed
another way — `Error`, `Invalid` or `Indeterminate` — while a different page waits and is submitted exactly once, and the
failed half of the shared Pinyin/Shuangpin draft not resubmitted), a
successful global save with its persistence check, a save that stores one page and is refused for another with the refused
edit kept, and read/apply/read-back with incarnation stability). The no-Host half also covers the
installed-font list the 候选字体 row offers: the families this session has, once each, sorted, a
name it does not contain reported as missing, and a search that narrows. It also covers the
Chinese presentation of the options this engine added: the labels, the group each row lands in,
one distinct label per stored value, and — the part that is easy to get wrong — that a mapped
enumeration is keyed by the string the Host stores rather than the C++ identifier, compared
against the schema a real Host publishes. The same half covers dictionary management: the
list, an import, the name conflict and its confirmed overwrite, a corrupt file that leaves the
imported dictionary alone, enable/disable, reload and delete — each read back from the Host,
with the learned dictionary compared before and after, and the whole chain again for a Chinese
name, which is the case that travels as UTF-8 and has to come back unchanged. A third
run, pointed at the same profile after the Host has been stopped through its stop event and restarted, prints **3 checks**:
the saved global value and the two saved candidate-presentation values. `--no-integration` skips the real-Host section.

### Running the real-Host checks

They need a Host on an isolated endpoint, and they never fall back to the installed
product's pipe. The Host must have a **writable** profile, because the global configuration is
applied through Fcitx's own `safeSave()` and cannot be saved without one: a Host started with
`FUSION_FCITX_SKIP_USER_PATH=1` has no user configuration directory, so the global-save checks
would report the Host's own failure instead of exercising the path.

Point the Host's per-user directories at a throwaway profile. Nothing outside it is read or
written, and the pipe name is refused if it is the production one:

```powershell
$profileRoot = "$env:TEMP\fusion-settings-profile"
New-Item -ItemType Directory -Force "$profileRoot\User\AppData\Roaming","$profileRoot\AppData\Local" | Out-Null
$env:APPDATA = "$profileRoot\User\AppData\Roaming"
$env:LOCALAPPDATA = "$profileRoot\AppData\Local"
$env:USERPROFILE = "$profileRoot\User"
Remove-Item Env:\FUSION_FCITX_SKIP_USER_PATH -ErrorAction SilentlyContinue
Start-Process .\FusionHost.exe -ArgumentList '--pipe-name', '\\.\pipe\fusion-settings-test',
  '--stop-event-scope', 'fusion-settings-test'
```

The Host prints where it resolved its paths; both lines must point inside `$profileRoot`:

```
[FusionHost] fcitx user config: ...\fusion-settings-profile\User\AppData\Roaming\Fcitx5\config
[FusionHost] fcitx user data:   ...\fusion-settings-profile\User\AppData\Roaming\Fcitx5\data
```

Then point the checks at the same name (the logical name, without the device path) and at the
profile root, so the no-change save can be checked against the files on disk:

```powershell
$env:FUSION_SETTINGS_TEST_PIPE = "fusion-settings-test"
$env:FUSION_SETTINGS_GLOBAL_PROFILE = $profileRoot
dotnet run --project src/settings-winui3/FusionSettings.Core.Tests
```

The run ends by printing `GLOBAL_EXPECT=<option>=<value>`. Stop the Host with its stop event
(signalling `Local\FusionHost-stop-<scope>` requests the orderly shutdown that saves state),
start it again against the same profile, and confirm the value survived:

```powershell
$env:FUSION_SETTINGS_GLOBAL_EXPECT = "DefaultPageSize=6"
dotnet run --project src/settings-winui3/FusionSettings.Core.Tests
```

`--pipe-name` takes the **full device path** (`\\.\pipe\<name>`); a bare name fails with
`ERROR_INVALID_NAME` (123). The client takes either form.

## Where the design is written down

- Option coverage, per-item dispositions and the shared-configuration rule:
  `fusion-controller/docs/` → `evidence/settings-winui3-2026-10-21/migration-inventory.md`.
- Decisions: D085 (partially superseded), D086 (licence scope), D087 (presentation).
- Client/Host boundary and the shared-state model: `windows-architecture.md`.
- Per-level completion state: `windows-stage6-plan.md`.
