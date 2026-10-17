# Stage 6 WP2 validation record

Date: 2026-10-17

The Release candidate was built from this worktree against the rebuilt `runtime-wp1-20261017` Fcitx runtime. Mock-only Release CTest passed 66/66, including the diagnostics default-off and opt-in redaction checks. The real-Fcitx Release smoke executables `stage3b`, `stage3c`, and `stage5_real_projection` passed with exit codes 0, 0, and 0. The ordered real settlement fixture returned `CommitText("你好")`, `SetPreedit("")`, `CommitText("，")` in order.

Diagnostics are disabled unless `FUSION_TSF_DIAGNOSTICS` is explicitly set to `1`, `true`, or `on`. Opt-in records technical lifecycle and ownership information only; key/text-bearing fields are redacted. The default-off and opt-in tests verify no diagnostic file by default and no `vk=` or `text=` content in the opt-in output.

The TSF hard-policy path probes `GUID_PROP_INPUTSCOPE` through `ITfInputScope` and blocks password/private/numeric PIN scopes before backend admission. Unknown metadata is recorded as unknown; there is no heuristic password classification and no password PASS claim without metadata evidence. The mandatory physical Notepad baseline oracle passed for preedit, Space commit, Backspace cancellation, and empty-preedit Enter.

This record is validation evidence only. It does not authorize distribution, merge, or Stage 3C/WP3 work.
