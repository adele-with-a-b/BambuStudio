# AGENTS.md — BambuStudio (adele-with-a-b fork)

Project-specific context for agents working in this repo. Global agent
rules live in the `agent-definitions` repo; this file holds only what's
specific to this BambuStudio fork.

## Purpose

Patched BambuStudio slicer. Ships as the daily-driver slicer for a
triathlete + EUC rider's workshop, with features upstream Bambu Lab
hasn't merged yet (Reload Presets, preset explorer, post-process
G-code re-parsing, build-infra fixes).

## Branch strategy

- **`master`** — tracks upstream `bambulab/BambuStudio` exactly. Never
  commit directly. Used for syncing + clean upstream PRs.
- **`dev`** — daily driver. Merges all feature branches. The installer
  shipped by `3d-engineer-infra` checks out this branch. This is where
  meta files (AGENTS.md, skills/) live.
- **`preset-hot-reload`** — PR #9919 (Reload Presets Cmd+R)
- **`preset-explorer`** — WIP batch preset management
- **`post-process-preview`** — G-code reparse after post-processing
- **`build-infra`** — cmake 4.x fixes, dev-build.sh, dep patches

## Skills

- `skills/bambu-studio-builder/SKILL.md` — build, dev, PR workflow,
  fork management, dev-build.sh semantics. Reference this before any
  build or branch-management work.

## Remotes

```
origin    git@github.com:adele-with-a-b/BambuStudio.git   # our fork
upstream  git@github.com:bambulab/BambuStudio.git         # upstream
```

## Build layout

- Source: `~/.3d-engineer/repos/bambu-studio` (M5) or
  `~/workspace/BambuStudio` (M2 Pro if checked out there)
- Deps: sibling `BambuStudio_dep/` directory (relative path, auto-detected)
- Build: `./dev-build.sh [command]` — see the skill for semantics

## When Mason operates on this repo

- Project path: `~/.3d-engineer/repos/bambu-studio` (M5 canonical)
- Task categories Mason can take:
  - Fix a bug on a specific feature branch
  - Add/modify C++ files following existing patterns
  - Update dep patches
  - Run build + test + report results
- Never:
  - Commit to `master` (tracks upstream)
  - Force-push `dev` or any feature branch without explicit approval
  - Modify patches in `deps/` without reading the patch first

## Related repos

- `3d-engineer-infra` — the bot platform that installs this fork on
  user machines. Has `BambuStudioBuilder.md` skill (legacy location;
  the canonical version is now here in `skills/`).

## Commits in this file

Keep AGENTS.md + skills/ changes as small, self-contained commits on
`dev`. Don't bundle with code changes.
