# MK-ROUTING-20261011 / B3 native selection

Canonical FFmpeg work: Goodwu/FFmpeg branch `routing/MK-ROUTING-20261011`, based on existing `lg-p84-d1-vui-fix-20261010` at d4bb79394b98d6247887e169998778474f43887e. Continue this branch; never recreate the repair elsewhere. Central plan and ledger are in Goodwu/media-kit PR4, docs/plans/media-kit-routing-implementation-20261011.md and media-kit-routing-handoff-20261011.json. App is Goodwu/PiliPlusX PR1 on the same task branch name.

## Scope

Existing media-kit ac4f7889 regression extracts the CURRENT production C selector and P8 caller. It reproduces P8 choosing a P5-only decoder when MIME-only selection runs before the P8-capable decoder. Repair only the declared native-DV profile matching and the empty-profile-list bypass. Preserve normal non-DV selection, P5/P8 configure mappings, RPU handling and no-silent-native-fallback behavior. No new Dart decoder-naming protocol or mpv protocol is planned for this fix.

A native DV request must have a supported profile (32 for P5 or256 for P8); missing/legacy experimental profile must fail before selecting/configuring a codec. Empty reported profiles do not establish native-DV support. This deliberately does not restore known-failed LG P8 native signaling. LG uses its existing correct alternate route when available.

## Verification

Reuse media-kit tool/routing_native_audit, extend its controls rather than making a second harness. Run against freshly checked-out current native sources; validate negative and positive cases, sanitizers where available, and actual Android-target compilation. Host fixtures do not prove configure/start, real decoder identity, HDR presentation, real-time throughput or an APK. Full latest-source mpv/FFmpeg/helper packaging and physical devices remain separate gates.

## State at registration

Branch created and pushed; production repair pending. Local user's preexisting FFmpeg configure worktree modification must not be reset, overwritten or silently included. This remote branch does not access or modify that worktree. Capture any additional applicable product build-tree patches explicitly before full candidate packaging.

## Repair checkpoint

Strict P5/P8 lookup and empty-DV-profile handling implemented in this source commit. Current-source host regression must pass before saving; Android compilation and independent rerun remain required. Do not recreate this repair in another branch.
