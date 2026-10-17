# Stage 6 WP2 repair accounting

| Finding | Evidence/status | Final disposition | Authority/dependency |
|---|---|---|---|
| U3 diagnostics were always-on and content-bearing in the historical plan text | The implementation now gates file creation on `FUSION_TSF_DIAGNOSTICS`; opt-in markers are redacted; Release tests `diag_default_off` and `diag_optin_redacted` pass | Repaired in WP2; no owner acceptance inferred | U3 remains an owner decision; implementation evidence does not accept it |
| InputScope admission could consume a queued read, classify the document start, and retain a bare entry pointer | Source review found `TF_ES_READ`, `GetStart`, and raw `ContextEntry*` | Repaired in one bounded change: `TF_ES_SYNC|TF_ES_READ`, current `TF_SELECTION` range, shared entry hold plus generation fence, explicit Unknown/Ordinary/Sensitive result | U5 bounded policy; faithful Password/Sensitive path remains deferred |
| Sensitive scopes | Actual policy path blocks Sensitive before backend admission; no backend learning/composition is admitted | Resolved for bounded U5 policy | Browser validation still required for final exit |
| Unknown metadata/refusal | Unknown is preserved and logged; it is not silently converted to Sensitive or used as stale success | Resolved as an explicit limitation | Browser/app metadata availability dependency |
| Final Release registration/profile | Elevated registration and profile query identify the final Release DLL and expected profile | Verified | Existing TSF registration authority |
| Firefox/Edge password fields | No browser surface was available in this execution environment | NOT RUN; WP2 remains incomplete for this dependency | Requires authorized Firefox and Edge sessions; synthetic input only |
| Historical Notepad oracle | Existing record was Stage 3B, not final WP2 Release proof | Preserved as historical evidence; removed from final-candidate proof claims | Original Stage 3B record |

The repair does not change Fcitx/LibIME semantics, ownership, PASS ordering, composition lifecycle, or teardown fencing. No WP3 or distribution work is included.
