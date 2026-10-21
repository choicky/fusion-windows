# Fusion settings client (C# + WinUI 3)

The Windows settings surface, per D087: a Fusion-owned C# + WinUI 3 client over the
existing Host settings protocol. Official Fcitx configuration semantics, storage and
defaults are unchanged; the client is another consumer of `settings_protocol.hpp`, not a
new configuration path.

## Layout

| Path | What it is |
|---|---|
| `FusionSettings.Core/` | Transport, framing, codecs, endpoint naming, connection/handshake, draft/baseline model, key and input-method models |
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
- **A write whose outcome is unknown says so.** Once the request frame has started, a lost,
  truncated, late or cancelled reply returns `Indeterminate` with
  `MutationMayHaveBeenApplied` — never "nothing was written". A failure before the frame
  starts is `Unreachable`. Nothing is ever replayed; the caller rereads and reconciles.
- **An unparsable key binding is preserved verbatim** (`KeyChord.IsOpaque`) instead of being
  replaced with an empty one, and empty entries keep their positions through a copy or a
  replacement.
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

Exit code 0 means every check passed: **371 checks** as of this revision — 336 focused (including a controlled pipe
peer for mutation outcomes and handshake versions) and 35 against a real Host when one is supplied. `--no-integration`
skips the real-Host section.

### Running the real-Host checks

They need a Host on an isolated endpoint, and they never fall back to the installed
product's pipe. Start one with no user data, so it cannot touch the owner's profile:

```powershell
$pipe = "\\.\pipe\fusion-settings-test"
$env:FUSION_FCITX_SKIP_USER_PATH = "1"
Start-Process .\FusionHost.exe -ArgumentList '--pipe-name', $pipe
```

Then point the checks at the same name (the logical name, without the device path):

```powershell
$env:FUSION_SETTINGS_TEST_PIPE = "fusion-settings-test"
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
