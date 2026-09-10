---
name: bambu-studio-builder
description: Build, develop, and contribute to the patched BambuStudio slicer. Use when building/compiling BambuStudio, working on C++ features (preset explorer, reload presets), managing git branches/PRs, creating slicer presets programmatically, or debugging BambuStudio crashes.
---

# BambuStudioBuilder

Build, develop, and contribute to the patched BambuStudio slicer.

## When to Use
- Building or compiling BambuStudio
- Working on BambuStudio C++ features (preset explorer, reload presets, etc.)
- Managing git branches, PRs, and commits for BambuStudio
- Creating or modifying slicer presets programmatically
- Debugging BambuStudio crashes or behavior

## Repository

**Source:** `~/workplace/oss/bambu-studio` (canonical OSS-fork roof; `origin` = our fork, `upstream` = `bambulab/BambuStudio`)
**Deps source:** `deps/` in-tree (CMake superbuild)
**Deps build tree:** `deps_build/` — installs to `deps_build/destdir/usr/local` (`dev-build.sh` auto-detects `usr/local` vs `destdir/usr/local`)
**Fork:** https://github.com/adele-with-a-b/BambuStudio

## Branch Strategy

- `master` — tracks upstream exactly, never commit directly
- `dev` — **daily driver**, merges all feature branches. Installer checks out this branch.
- `preset-hot-reload` — PR #9919 (Reload Presets Cmd+R)
- `preset-explorer` — WIP batch preset management (nozzle filter, compatible toggle, compare)
- `post-process-preview` — G-code reparse after post-processing
- `build-infra` — cmake 4.x fixes, dev-build.sh, dep patches
- When upstream updates: `git fetch upstream && git rebase upstream/master dev`

## Build

```bash
cd ~/workplace/oss/bambu-studio
./dev-build.sh          # incremental build + install to /Applications
./dev-build.sh clean    # reconfigure cmake + full build + install
./dev-build.sh build    # build only, don't install
./dev-build.sh nuke     # delete build dir + reconfigure + full build + install
./dev-build.sh configure # cmake configure only (no build)
./dev-build.sh help     # show usage
```

All commands also work with `--` prefix (`--clean`, `--nuke`, etc.).

Build uses: Unix Makefiles, arm64, Release, `-DBBL_INTERNAL_TESTING=1` (→ BambuStudioInternal data dir).

After install, `dev-build.sh` automatically:
- Backs up BambuStudioInternal config to `~/.3d-engineer/bambu-config-backup/`
- Copies network plugin from Beta/Release if missing
- Swaps in orange dev icon
- Clears build.log before build (prevents progress bar confusion from configure output)

### Troubleshooting

**Never pipe a build into `tail`.** `cmake --build ... | tail -30` returns *tail's* exit status, so a failed build reports success — and the truncation hides the compiler errors that explain it. Redirect instead:
```bash
cmake --build build --target <t> -j$(sysctl -n hw.ncpu) > /tmp/build.log 2>&1; echo "exit=$?"
```
Same for `dev-build.sh`. This bit me on 2026-09-05: the harness reported "exit code 0" while `make` had died with `Error 2`, and I only noticed because the expected binary didn't exist. Note `dev-build.sh` is itself safe to read from — it routes the real compile log to `build.log` and only echoes that file's tail to stdout, so **`build.log` is the authoritative record**, not the captured stdout. Grepping the stdout fragment for `Building CXX` will show zero compiles even on a build that compiled 600 files.

**`CMake Error: OpenMeshCraft is required` — new hard dependency as of 2026-09.** Upstream added an OpenMeshCraft mesh-boolean backend (`b5d1d3b5e0`) and then made it mandatory (`d7fdb018e6`, reverted by `50ce2592ba`, re-landed as `6850c21971`). `src/CMakeLists.txt:32` is now a `FATAL_ERROR` if the package isn't found. Two ways out:

```bash
# Proper: build the dep (deps/OpenMeshCraft exists, target dep_OpenMeshCraft)
cmake -S deps -B deps_build && cmake --build deps_build --target dep_OpenMeshCraft -j$(sysctl -n hw.ncpu)

# Fast escape hatch when you only need to compile-check GUI code:
cmake -S . -B build -DSLIC3R_ALLOW_MCUT_BOOLEAN=ON
```

The fallback flag is upstream's own, documented in the error message, and it only swaps the boolean backend — fine for verifying unrelated changes, but it diverges from what CI builds, so don't leave the build tree configured that way when you're testing boolean/mesh behaviour. Note this is the same class of gap as the libharu case above: rebasing across months of upstream can introduce deps your prefix predates, and it fails at *configure* time, not compile time — so the error is in `dev-build.sh`'s stdout, not in `build.log`.

**The test suite does not compile — `-DSLIC3R_BUILD_TESTS=ON` fails.** Verified 2026-09-05 against upstream `66e405477`: the `fff_print_tests` target dies with 33 errors across four files, all pre-existing upstream rot, none of it ours:

| file | errors | cause |
|---|---|---|
| `tests/fff_print/test_gcodewriter.cpp` | 19 | `GCodeWriter::lift` and `GCodeConfig::retract_lift` no longer exist |
| `tests/fff_print/test_print.cpp` | 9 | `Print::brim()` no longer exists (upstream lines 102/112/124) |
| `tests/fff_print/test_skirt_brim.cpp` | 2 | same class |
| `tests/fff_print/test_support_material.cpp` | 1 | same class |

Consequences worth internalising before promising anything to a maintainer: you cannot add a *runnable* Catch2 regression test under `tests/fff_print/` without first repairing upstream's rot; upstream CI doesn't run these either (the flag defaults OFF at `CMakeLists.txt:91`, forced OFF only for cross-compile at `:96`); and "I'll add a regression test" is therefore not a commitment you can honour in an upstream PR body for this repo. Small fixes here ship without tests as a matter of house practice — cf. `ef96c2001`, 4 insertions, no test.

If you do need to run one test, the harness itself is fine (`tests/fff_print/test_data.hpp:67` gives `init_print(std::initializer_list<TriangleMesh>, Print&, Model&, config)`); you just have to exclude the four rotted files from `tests/fff_print/CMakeLists.txt` first. Remember to set `SLIC3R_BUILD_TESTS=OFF` and reconfigure afterwards — leaving it ON invalidates libslic3r and forces a ~600-file rebuild on the next app build.

**`./dev-build.sh: no such file or directory` — the script is branch-scoped.** `dev-build.sh` and `skills/` are fork-only files tracked on `dev` (and branches cut from `dev`). They are deliberately ABSENT from upstream-PR branches (`preset-hot-reload` etc.) so the PR diff stays clean. `git switch preset-hot-reload` therefore DELETES the script from the working tree, and the next build launch dies with exit 127. Verified 2026-09-05 — do not misdiagnose as a cwd, PATH, or sandbox problem; run `pwd; ls dev-build.sh; git rev-parse --abbrev-ref HEAD` in the FOREGROUND first.

Restore it as an untracked file (won't pollute the PR branch):
```bash
git show dev:dev-build.sh > dev-build.sh && chmod +x dev-build.sh
```
**Then delete it before switching back**, or `git switch` aborts with "untracked working tree files would be overwritten" — and if you chain a `git stash pop` onto that aborted switch with `&&`/newline, the pop applies to the WRONG branch and conflicts. Sequence: `rm -f dev-build.sh` → `git switch <branch>` → verify the branch → `git stash pop`.

**New upstream dependency after a big rebase (e.g. `Could not find HPDF_LIBRARY ... names: hpdf, hpdfd`).** Rebasing onto months of upstream can introduce deps our `deps_build` prefix predates — libharu/HPDF arrived this way (`src/slic3r/CMakeLists.txt` gained `find_library(HPDF_LIBRARY ... REQUIRED)`). The deps *superbuild* cache is also stale, so `dep_libharu` isn't yet a known target. Reconfigure the superbuild, then build only the missing target — do NOT rebuild all deps:
```bash
cmake -S deps -B deps_build
cmake --build deps_build --target dep_libharu -j$(sysctl -n hw.ncpu)
```
Confirm the artifact landed (`deps_build/destdir/usr/local/lib/libhpdf.a`) before re-running the app build. Failures of this shape are upstream/deps drift, never your feature change.

**cmake 4.x compatibility:** All `cmake_minimum_required(VERSION < 3.5)` fixed in the fork. Dep downloads (wxWidgets, OpenCV, CGAL) have PATCH_COMMANDs that fix their versions automatically.

**Boost patch hangs:** Removed — the original patch targeted installed Boost layout, not source tree. PATCH_COMMAND is now a no-op echo.

**deps install to different paths:** `dev-build.sh` auto-detects `usr/local` vs `destdir/usr/local`.

**deps build incomplete:** The installer checks for `libTKernel.a` (OCCT, last dep to build). If missing, offers to clean and rebuild.

**Network plugin "Failed to download":** Custom builds have a different `SLIC3R_VERSION` so the plugin download fails. Fixed: `#ifdef BBL_INTERNAL_TESTING` skips the version check — any plugin version is accepted. `dev-build.sh` auto-copies the plugin from Beta/Release after install.

**Progress bar stuck at 0:** The `find` for stamp files must not use `-path "*/stamp/*"` — the actual path is `dep_Boost-stamp`, not `stamp`. Use `find "$path" -name "*-done"` without path filter.

## Three Apps, Three Data Dirs

| App | Data Dir | Purpose |
|---|---|---|
| `/Applications/BambuStudio.app` | `BambuStudio` | Official release |
| `/Applications/BambuStudio Beta.app` | `BambuStudioBeta` | Official beta — sends prints to printer |
| `/Applications/BambuStudio Dev.app` | `BambuStudioInternal` | Our patched build — slice here, send from Beta |

Dev build has an orange icon to distinguish from official apps. `dev-build.sh` swaps both `Icon.icns` and `BambuStudio-mac_256px.ico` (dock icon set programmatically at startup).

## Signing Limitation — RESOLVED

**LAN mode printing works from the Dev build.** Requires:
1. Developer Mode enabled on the printer (Settings → Developer Mode)
2. `BBL_RELEASE_TO_PUBLIC=0` in the build (already set in dev-build.sh)
3. LAN mode connection (not cloud)

The beta network plugin (v02.05.01.52) loads successfully, cert validation is skipped, and LAN mode print sending + live view work. No code changes needed beyond the build flag.

Cloud mode not tested and likely doesn't work, but LAN mode is the preferred workflow.

## Git Configuration

- Commit email: `nabdel07@icloud.com` (per-repo config)
- Always commit with descriptive messages

## Active Branches

### Emboss / text-gizmo crash workstream (state 2026-09-05, nothing filed)

Three *distinct* bugs, not one. Treating them as a single blocked thing is what parked this for three months. Nothing is on upstream — `gh pr list --head <branch>` returns empty for every branch below.

> ### ⛔ Bug A IS DEAD — upstream root-caused it 2026-09-01. Do not file pr1.
>
> `e654e5afc8` ("FIX: fix macOS SVG/text emboss use_surface crash (GMP arm64 x18)") — verified an ancestor of `upstream/master`, never reverted, patch wired unconditionally into `deps/GMP/GMP.cmake` as `0002-GMP_arm64_avoid_x18_reserved_on_darwin.patch`.
>
> **It was never a stack overflow.** GMP 6.2.1's arm64 mpn assembly uses register `x18`, the reserved platform register on Darwin. Clobbering it corrupts memory inside CGAL's exact predicates. Upstream applied GMP changeset `5f32dbc41afc` (the Homebrew/MacPorts patch) to use x17/x6/x14 instead. The rebuilt arm64 libgmp passes GMP's full `make check`; **36 of 50 tests were failing on Apple Silicon before it.**
>
> Everything below about Bug A was a wrong diagnosis, and the new one explains the evidence better: macOS-arm64-only (a stack theory never explained why 4 MB sufficed on Windows/Linux); intermittent at 3–13% per *identical* input (memory corruption, not varying recursion depth); `__stack_chk_fail` in traces (the canary tripped **by** the corruption — we read it backwards); and the 16 MB bump helping partially while leaving QA's ~50% residual (a bigger stack changes layout, changing whether the corruption lands fatally — masking, not fixing). **Bambu QA's revert of #10847 was correct, for a better reason than they gave.**
>
> Dead as a result: `emboss-pr1/cgal-helper-subprocess`, `emboss-pr1/cgal-helper-v2`, `emboss-crash-fix`, and the emboss half of `strip-cruft` — subprocess isolation, the Mach exception handler, `snap_expolygons_to_grid`, the size pre-flight. All symptom suppression around a deps patch. Bug B (#12137) and Bug C (pr3) are unaffected; they are different defects.
>
> **Local builds need `deps_build`'s GMP rebuilt** to pick this up, otherwise the crash still reproduces locally and misleads you toward the old theory.
>
> ⚠️ **The meta-lesson: a prior-art check is perishable.** The 571-commit sweep on 2026-09-05 correctly found no upstream fix — master was `66e405477` (Aug 31) and the fix landed Sep 1. Treating that point-in-time result as durable nearly pushed a refuted premise upstream. **For any parked workstream, re-run the redundancy check immediately before filing, not once when you pick it up.** What actually surfaced this was an unrelated configure failure (upstream's new hard `OpenMeshCraft` requirement) forcing a look at the commit log — luck, not process.

**Bug A (WRONG DIAGNOSIS — kept for provenance) — CGAL/GMP stack-stomp in `cut_surface()`.** `emboss-pr1/cgal-helper-subprocess`, plus the 16 MB stack bump on the local-only `emboss-crash-fix`. This is the contested one. Upstream **merged and then reverted** the stack bump: PR #10847 merged 2026-05-21, reverted 2026-05-28 by `ced8934c7908` with reason `<测试确认修复不行>`; QA measured *"still a 50% chance of crashing"* and asked twice for a QuickRecorder GIF that was never supplied. The PR page still displays **MERGED** — a search that stops at PR state will conclude wrongly that this shipped. `upstream/master:src/libslic3r/Thread.hpp:53` is back to 4 MB. OrcaSlicer cherry-picked the identical patch the same day ([OrcaSlicer#13772](https://github.com/OrcaSlicer/OrcaSlicer/pull/13772)) and still ships `16 * 1024 * 1024`.

  **`emboss-pr1` is NOT filable as-is.** The macOS Mach-exception handler that stops the helper's signal-death from writing a user-visible `.ips` crash report exists *only* on `emboss/strip-cruft-20260609` — `task_set_exception_ports` appears 2× and `EXIT_HELPER_OVERFLOW` 6× there, **0× on `emboss-pr1` and `emboss-crash-fix`**. Filing pr1 without absorbing that surplus reproduces the exact signal QA used to revert #10847. Also relevant: `upstream/master:src/libslic3r/TryCatchSignal.hpp` is a **no-op stub on every non-MSVC platform**, so there is no in-process recovery primitive to reuse on macOS/Linux.

**Bug B — empty text mesh → null deref in slicing prep.** `emboss-pr4/empty-text-mesh-throws` @ 6 insertions / 3 deletions, build-verified, ready. Superseded `emboss-pr4/empty-mesh-chokepoint` (118 lines / 3 files), which must NOT be filed: its `PrintApply.cpp` hunk stripped empty volumes from the vector `update_volume_bboxes` walks, and that function is the **sole writer** of `PrintObjectRegions::cached_volume_ids` (`PrintApply.cpp:955-959`) while the consumer at `:687-690` indexes that vector in a loop with **no bounds check** — so the strip desynchronised them and turned a deterministic fault into an OOB read.

  Root cause and trigger, both source-verified: `create_all_char_mesh` calls `result.clear()` at `EmbossJob.cpp:1284` and only *then* tests all-space input at `:1291-1293` (`wxRegEx("^ +$")`), so all-space text returns an empty result; `process()` bare-returns at `:1404`; `generate_mesh_according_points` never runs so `m_final_text_mesh` stays default-constructed (`InputInfo` is a stack local at `GLGizmoText.cpp:3336`, and `:1922` is its only writer); `finalize()` tests only `canceled || eptr` and commits it; `Print::apply` → `update_volume_bboxes` → `transformed_its_bbox2d` reads `its.vertices[its.indices.front()(0)]` at `PrintApply.cpp:587` with the guarding assert one line above compiled out (build type is Release, `-DNDEBUG`).

**Bug C — modal dialog on recoverable failure.** `emboss-pr3/recoverable-toast`, rebased and build-verified, but **do not file it in its current shape: it is a no-op for the path its own commit message describes.** pr3 only modifies `exception_process()`. `GLGizmoText` dispatches `GenerateTextJob` (`GLGizmoText.cpp:3374`), and `GenerateTextJob::finalize` never calls `exception_process()` — it tests `canceled || eptr` and discards `eptr`.

**The `_finalize` asymmetry (the key structural fact behind both B and C).** Five of the seven `finalize()` methods in `EmbossJob.cpp` call `if (!_finalize(canceled, eptr, *m_input.base)) return;` (`:350`, `373`, `456`, `535`, `552` on master), which routes the exception through `exception_process()` → `create_message()` and shows the user something. `GenerateTextJob::finalize` and `CreateObjectTextJob::finalize` instead test `canceled || eptr` and drop `eptr`. Consequences: the five `throw JobException` calls already in `GenerateTextJob::process` are **dead code today**; pr3 cannot reach the text path; and pr4's new throws stop the crash without telling the user anything. Rewiring those two `finalize()` methods is the missing piece and belongs in pr3.

`CreateObjectTextJob` has the same bare return at `EmbossJob.cpp:1978` but is **not** buggy — its `finalize()` guards `m_input.m_position_points.empty()`. That is the contract `GenerateTextJob` is missing, and it's the strongest argument in pr4's PR body.

**Build-prereq commits are obsolete.** Every emboss branch originally carried `build: tolerate unknown -W flags on AppleClang 21+` and `build: switch wxMediaState sentinels from constexpr to const`. Upstream `3f3c1bd46` supersedes both with strictly better fixes (`static inline wxMediaState` at `MediaPlayCtrl.h:97`; `check_cxx_compiler_flag` at `CMakeLists.txt:307`). They were also the **sole** cause of every rebase conflict — drop them and cherry-pick only the emboss commit onto fresh `upstream/master` for a zero-conflict result.

**Still live upstream:** #7782, #5995, #9418 all open; #6403 same class, never commented on by us. Corrections for the stale "fixed in main" claim were posted to the first three on 2026-09-05.

`emboss/strip-cruft-20260609` is a daily-driver integration branch, explicitly not for upstream; it also has a real build defect (`tests/libslic3r/CMakeLists.txt` references 8 nonexistent files, benign only because tests default OFF). Its uncommitted WIP is a finished "Reset to applied values" UX feature that depends on pr4's guards.

**Treat every validation number in these commit messages as an unverified claim.** "316 helper spawns / 6 absorbed SIGBUS / 0 parent crashes", "30/30 clean", "fired 29 times" — none has a log, artifact, or test behind it, and upstream QA already contradicted exactly this class of self-report on #10847. Re-measure before citing.

### `preset-hot-reload` (upstream PR #9919)
- Reload user presets from disk without restart
- File → Reload Presets (**Cmd+Shift+P**) — was Cmd+R, then Cmd+Shift+R; both collided with the slice handler, since `Cmd+Shift+R` also matches `CmdDown() && GetKeyCode()=='R'`. In `MainFrame.cpp` the reload block must come FIRST and `return`, or the slice branch swallows it.
- Toast notification with reload stats
- `PresetReloadResult` counts **added and removed** user presets for process, filament, AND printer kinds (`any_change()` gates the toast). In-place edits to an existing preset are invisible to the name-diff by design — the side UI refreshes unconditionally.
- `load_user_presets(user_id, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)` to match the other non-interactive load sites (ConfigWizard, WebGuideDialog, PresetUpdater). Behavior-neutral: that overload discards the substitution report (`PresetBundle.cpp` returns an empty `PresetsConfigSubstitutions()`).
- Removed reload button from Tab toolbar (confusing next to profile-specific buttons)
- Menu entry below Batch Preset Management, cross-platform (was Mac-only)
- `load_current_presets()` called after reload to update all tab dropdowns
- **2026-09-05:** rebased onto 571 new upstream commits (one semantic conflict in `MainFrame.cpp`), tonghao-bbl's two review points implemented and answered, compile-verified clean. Awaiting next maintainer pass.

### `preset-explorer` (new feature, branched from preset-hot-reload)
- Complete rewrite of Batch Preset Management dialog
- `PresetExplorerDialog.cpp/.hpp` — new files
- Replaces `UserPresetsDialog` in File menu

**What's working:**
- Expandable preset cards with key settings columns (Layer, Walls, Infill, Nozzle, PP)
- Faceted filter sidebar:
  - Nozzle size checkboxes with counts, dynamic based on compatible filter
  - Base profile checkboxes, dynamic based on nozzle selection
  - Compatible-only checkbox with tooltip, clears other filters and updates lists dynamically
  - Has post-processing checkbox
  - Material type filter (filament tab)
  - Last remaining checkbox grayed out (disabled) to prevent empty selection
- Clickable column headers for sorting (▼=A-Z, ▲=Z-A), active column highlighted
- Expanded details: flat grid with category headers (icons + bold text), localized setting labels via OptionsSearcher, orange (#F1754E) override values, gray base values
- Compare button: opens DiffPresetDialog parented to modal, pre-selects first two checked presets, enables show_all for incompatible presets. Only enabled with exactly 2 selected.
- Select All checkbox in bottom bar, syncs with individual selections
- Batch delete with confirmation dialog, cloud sync, UI refresh
- Green ✓ / Red ✗ compatibility indicators per preset with tooltips
- Search by preset name only
- Empty state icon (preset_empty/preset_empty_dark) when no results
- Dark mode aware: all colors, backgrounds, text adapt
- Expanded/checked state preserved across filter changes and list rebuilds
- +/- toggle on expand/collapse

**Known issues to fix:**
- Expanded view column alignment: values not perfectly aligned when setting names vary in length. The flat grid with `wxFlexGridSizer` growable columns is close but not pixel-perfect. May need fixed-width first column or a `wxDataViewListCtrl` for proper column resize.
- Post-processing script display: should show full path without truncation, wrapping to multiple lines if needed. Currently using `get_string_value` which may truncate.
- The old `UserPresetsDialog` still exists in the codebase — not removed, just replaced in the menu. Can be removed once preset-explorer is stable.
- DiffViewCtrl flashing bug in the Compare dialog (wxWidgets macOS issue) — not fixable from our side without replacing the tree control. The preset explorer expanded view avoids this by using a flat grid.

**Future improvements discussed:**
- Inline compare (diff panel slides in instead of modal)
- Settings search (search by setting value, not just preset name)
- Quick actions per preset on hover (duplicate, export, edit)
- Inheritance visualization (tree view of parent→child relationships)
- Import presets button (already exists as File → Import Configs, could be surfaced here)
- Phase 4: detail panel (single-click shows key settings in right panel without expanding)

### `post-process-preview`
- Re-parses G-code after post-processing scripts run
- Preview shows post-processed toolpath automatically

### `patched`
- Combined branch with all features + README (what we build locally)

## Key Files

| File | Purpose |
|---|---|
| `src/slic3r/GUI/PresetExplorerDialog.cpp/.hpp` | New preset explorer dialog |
| `src/slic3r/GUI/UserPresetsDialog.cpp/.hpp` | Old batch management (still exists, replaced in menu) |
| `src/slic3r/GUI/UnsavedChangesDialog.cpp/.hpp` | DiffPresetDialog, DiffViewCtrl, DiffModel |
| `src/slic3r/GUI/GUI_App.cpp` | `reload_user_presets_from_disk()` |
| `src/slic3r/GUI/MainFrame.cpp` | Menu items, keyboard shortcuts, `diff_dialog` |
| `src/slic3r/GUI/Tab.cpp/.hpp` | Settings tabs, preset combo boxes, category icons |
| `src/slic3r/GUI/PresetComboBoxes.cpp/.hpp` | Preset dropdown widgets |
| `src/libslic3r/Preset.hpp/.cpp` | Preset data model, `is_compatible`, `inherits()`, `dirty_options()` |
| `src/libslic3r/PresetBundle.cpp` | `import_presets()`, `load_user_presets()` |
| `src/libslic3r/PrintConfig.cpp/.hpp` | `print_config_def` — setting definitions, labels, categories |
| `src/slic3r/GUI/Search.hpp` | `OptionsSearcher` — localized setting labels and categories |

## User Preset Directories

```
~/Library/Application Support/BambuStudioInternal/user/1615318752/
├── filament/   ← .json + .info pairs
├── process/    ← .json + .info pairs
└── printer/    ← .json + .info pairs
```

Same structure in `BambuStudioBeta`. Presets must be copied between both for the slice-in-Dev-send-from-Beta workflow.

## Preset JSON Format

**Process:**
```json
{
    "from": "User",
    "inherits": "0.20mm Standard @BBL H2C",
    "name": "My Preset",
    "print_settings_id": "My Preset",
    "print_extruder_id": ["1", "1", "2", "2"],
    "print_extruder_variant": ["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow"],
    "version": "2.5.0.7",
    "<changed_setting>": "<value>"
}
```

**Filament:**
```json
{
    "from": "User",
    "inherits": "Bambu ASA @BBL H2C 0.6 nozzle",
    "name": "My Filament",
    "filament_settings_id": ["My Filament"],
    "filament_extruder_variant": ["Direct Drive Standard", "Direct Drive High Flow"],
    "version": "2.5.0.7",
    "<changed_setting>": ["<value>"]
}
```

**Info file** (same name, `.info` extension):
```
sync_info = create
user_id = 1615318752
setting_id = 
base_id = <parent profile's setting_id from system JSON>
updated_time = 0
```

## Nozzle Size Convention

System preset names encode nozzle size:
- No suffix = 0.4mm (default): `0.20mm Standard @BBL H2C`
- `0.2 nozzle` = 0.2mm: `0.08mm High Quality @BBL H2C 0.2 nozzle`
- `0.6 nozzle` = 0.6mm: `0.24mm Balanced Strength @BBL H2C 0.6 nozzle`
- `0.8 nozzle` = 0.8mm

## Extruder Variants

H2C Vortek system has per-variant settings (Direct Drive Standard / High Flow). Array values like `["300","300","200","200"]` are per-variant. When displaying, show first value; if values differ, show ⓘ with tooltip listing all variants.

## Known Bugs

### DiffViewCtrl flashing (wxWidgets macOS)
During collapse/expand animation in `wxDataViewCtrl`, parent node text flashes across all rows. Affects both Compare dialog and any tree-based view. Workaround: use flat grid layout instead of tree for the preset explorer expanded details. The Compare dialog still has this bug — it's a wxWidgets issue.

### Filament sync overwrite
When syncing from printer, custom filament selection gets overwritten with the system profile matching the AMS-reported filament. No setting to disable. Workaround: re-select custom profile after sync.

### Develop mode grays out the Advanced toggle

**Two unrelated "developer" concepts get confused. Keep them separate:**

| Concept | What it is | How to enable | Required for |
|---|---|---|---|
| **Printer Developer Mode** | Setting on the H2C touchscreen | Printer Settings → Developer Mode | LAN-mode print sending from the Dev build (combined with `BBL_RELEASE_TO_PUBLIC=0` at compile time) |
| **Slicer `user_mode: develop`** | UI verbosity level in Bambu Studio | Preferences → Develop mode checkbox (writes `user_mode: develop` in `BambuStudio.conf`) | Showing extra internal/debug settings in Process/Filament/Printer tabs |

They share the word "developer" and do completely different things. Enabling slicer Develop mode has **zero** effect on LAN printing — only the printer-side toggle + the compile flag matter for that.

**The gotcha:** turning on slicer Develop mode **disables the Advanced/Simple switch** in the param panel sidebar. The switch still renders but won't respond to clicks. This is intentional — see `src/slic3r/GUI/ParamsPanel.cpp::ParamsPanel::update_mode()`:

```cpp
if (app_mode == comDevelop)
{
    mode_view->Disable();
    return;
}
```

Why: Develop is a visibility superset of Advanced — every Advanced-gated field is already showing, plus the Develop-gated ones. The switch would have no effect on what you see, so the code disables it rather than present a redundant control. To get back to just-Advanced visibility, turn Develop off in Preferences.

**Fix if stuck in this state:** Preferences → uncheck Develop mode (calls `save_mode(comAdvanced)`, re-enables the switch). Or edit `BambuStudio.conf` directly: change `"user_mode": "develop"` to `"advanced"` while the app is closed.

**Rule of thumb for the Dev build:** leave `user_mode` at `advanced`. Printer Developer Mode is what you need for LAN printing, not slicer Develop mode.


## Import Configs (existing feature)
File → Import → Import Configs handles `.json`, `.zip`, `.bbscfg`, `.bbsflmt`. Validates, detects type from keys (`print_settings_id` → process, `filament_settings_id` → filament), resolves `inherits`, generates `.info` file automatically via `preset.save()`, prompts on overwrite conflicts.

## Patched BambuStudio Features

### Post-process preview re-parse
Branch `post-process-preview`. After post-processing scripts modify G-code, the preview re-parses to show the actual toolpath.

### BrickLayers wipe fix
Fixed in `bricklayers.py` — added `and not feature.wiping` check to prevent Travel Fix Up injection inside wipe blocks.

## Fork Management (appended from 3d-engineer prompt)

Formerly part of the 3d-engineer agent prompt. Collected here because
fork-management IS builder-work for this repo.

- `master` tracks upstream exactly — never commit directly.
- `dev` is the daily driver — merges all feature branches. Installer
  checks out `dev`.
- Keep PR branches clean — only PR-relevant commits. WIP goes on
  feature branches.
- Force-push carefully — when cleaning up branches, verify nothing is
  lost first.
- `dev-build.sh` commands:
  - (none) — incremental build
  - `clean` — reconfigure
  - `build` — no install
  - `nuke` — delete build dir
  - `configure` — cmake only
  - `help` — usage
