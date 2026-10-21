# Fusion settings client (C# + WinUI 3)

The Windows settings surface, per D087: a Fusion-owned C# + WinUI 3 client over the
existing Host settings protocol. Official Fcitx configuration semantics, storage and
defaults are unchanged; the client is another consumer of `settings_protocol.hpp`, not a
new configuration path.

## Layout

| Path | What it is | Builds here? |
|---|---|---|
| `FusionSettings.Core/` | Transport, framing, codecs, endpoint naming, connection/handshake, draft/baseline model, key and input-method models | **yes** |
| `FusionSettings.Core.Tests/` | Focused checks plus real-Host checks over an isolated pipe | **yes** |
| the WinUI views | The settings window itself | **not yet written** — see *Prerequisites* |

`FusionSettings.Core` has **no `PackageReference`** on purpose. That is what lets it build
and test with nothing but a .NET SDK, and it keeps the client's correctness-critical logic
independently verifiable.

## Prerequisites

- **.NET SDK 10.0.402** (current LTS at the time of writing; SDK 10 is supported until
  2028-11-14). Any SDK that supports `net10.0` works; the version is pinned here because
  that is what was used and verified.
- For the WinUI views only: **`Microsoft.WindowsAppSDK`** and
  **`Microsoft.Windows.SDK.BuildTools`** from a reachable NuGet feed. Neither is restorable
  in the environment this core was developed in, which is why the views do not exist yet.

No other dependency is required, and none is distributed with the core.

## Build and test

```powershell
# from the repository root
dotnet build src/settings-winui3/FusionSettings.Core/FusionSettings.Core.csproj
dotnet build src/settings-winui3/FusionSettings.Core.Tests/FusionSettings.Core.Tests.csproj
dotnet run --project src/settings-winui3/FusionSettings.Core.Tests
```

Exit code 0 means every check passed. `--no-integration` skips the real-Host section.

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
