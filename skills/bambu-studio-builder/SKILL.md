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

**Source:** `~/workspace/BambuStudio` (or `~/work/projects/BambuStudio`)
**Deps:** `../BambuStudio_dep` (relative to source — auto-detects `usr/local` vs `destdir/usr/local`)
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
cd ~/work/projects/BambuStudio
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

### `preset-hot-reload` (upstream PR)
- Reload user presets from disk without restart
- File → Reload Presets (Cmd+Shift+R) — was Cmd+R, changed to avoid conflict with slice
- Toast notification with reload stats
- Removed reload button from Tab toolbar (confusing next to profile-specific buttons)
- Menu entry below Batch Preset Management, cross-platform (was Mac-only)
- `load_current_presets()` called after reload to update all tab dropdowns
- PR reviewer: Max — feedback incorporated, awaiting next review

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
