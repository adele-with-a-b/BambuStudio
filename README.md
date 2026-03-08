![image](https://user-images.githubusercontent.com/106916061/179006347-497d24c0-9bd6-45b7-8c49-d5cc8ecfe5d7.png)
# BambuStudio (Patched Fork)

This is a patched fork of [Bambu Lab's BambuStudio](https://github.com/bambulab/BambuStudio) with additional features and fixes.

## Added Features

### Reload Presets (Cmd+R)

Reload user presets from disk without restarting the app.

- **Menu:** File → Reload Presets (Cmd+R)
- **Button:** Refresh icon next to the preset dropdown on all tabs (process, filament, printer)
- **Auto-select:** When new presets are detected after reload, the most recent one is automatically selected

This enables external tools (scripts, AI assistants, CLI workflows) to generate Bambu Studio preset JSON files that appear in the slicer immediately — no restart required.

#### Preset Directory

User presets are stored at:

| OS | Path |
|---|---|
| macOS | `~/Library/Application Support/BambuStudio/user/<user_id>/process/` |
| Windows | `%APPDATA%\BambuStudio\user\<user_id>\process\` |
| Linux | `~/.config/BambuStudio/user/<user_id>/process/` |

Replace `<user_id>` with your Bambu account ID (visible in the directory), or `default` if not logged in. Filament presets go in `filament/` instead of `process/`.

Each preset needs two files: a `.json` and a `.info`.

#### Process Preset Example

**Structural PETG-CF 0.6mm @BBL H2C.json** — a structural profile with 4 walls and 40% infill:
```json
{
    "from": "User",
    "inherits": "0.24mm Balanced Strength @BBL H2C 0.6 nozzle",
    "name": "Structural PETG-CF 0.6mm @BBL H2C",
    "print_extruder_id": ["1", "1", "2", "2"],
    "print_extruder_variant": ["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow"],
    "print_settings_id": "Structural PETG-CF 0.6mm @BBL H2C",
    "version": "2.5.0.7",
    "wall_loops": "4",
    "sparse_infill_density": "40%"
}
```

**Structural PETG-CF 0.6mm @BBL H2C.info**:
```
sync_info = create
user_id = <your_user_id>
setting_id = 
base_id = <parent_profile_setting_id>
updated_time = 0
```

The `base_id` is the `setting_id` from the parent system profile JSON (found in the `system/BBL/process/` directory). Only settings that differ from the inherited base need to be included.

Drop both files into the preset directory and hit Cmd+R.

### Post-Processing Script Preview

After slicing with post-processing scripts configured, the G-code is automatically re-parsed so the preview shows the post-processed toolpath. No need to export and drag the file back in.

Especially useful with [BrickLayers](https://github.com/GeekDetour/BrickLayers) for interlocking layer preview.

### Feature Branches for Upstream PRs

| Branch | Feature |
|---|---|
| [`preset-hot-reload`](../../tree/preset-hot-reload) | Reload Presets button + Cmd+R |
| [`post-process-preview`](../../tree/post-process-preview) | G-code re-parse after post-processing |

---

## Original README

Bambu Studio is a cutting-edge, feature-rich slicing software.  
It contains project-based workflows, systematically optimized slicing algorithms, and an easy-to-use graphic interface, bringing users an incredibly smooth printing experience.

Prebuilt Windows, macOS 64-bit and Linux releases are available through the [github releases page](https://github.com/bambulab/BambuStudio/releases/).

Bambu Studio is based on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [wiki](https://github.com/bambulab/BambuStudio/wiki) and the [documentation directory](https://github.com/bambulab/BambuStudio/tree/master/doc) for more information.

# What are Bambu Studio's main features?
Key features are:
- Basic slicing features & GCode viewer
- Multiple plates management
- Remote control & monitoring
- Auto-arrange objects
- Auto-orient objects
- Hybrid/Tree/Normal support types, Customized support
- multi-material printing and rich painting tools
- multi-platform (Win/Mac/Linux) support
- Global/Object/Part level slicing parameters

Other major features are:
- Advanced cooling logic controlling fan speed and dynamic print speed
- Auto brim according to mechanical analysis
- Support arc path(G2/G3)
- Support STEP format
- Assembly & explosion view
- Flushing transition-filament into infill/object during filament change

# How to compile
Following platforms are currently supported to compile:
- Windows 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Windows-Compile-Guide)
- Mac 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Mac-Compile-Guide)
- Linux, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Linux-Compile-Guide)
  - currently we only provide linux appimages on [github releases](https://github.com/bambulab/BambuStudio/releases) for Ubuntu/Fedora, and a [flathub version](https://flathub.org/apps/com.bambulab.BambuStudio) can be used for all the linux platforms

# Report issue
You can add an issue to the [github tracker](https://github.com/bambulab/BambuStudio/issues) if **it isn't already present.**

# License
Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

The bambu networking plugin is based on non-free libraries. It is optional to the Bambu Studio and provides extended networking functionalities for users.
By default, after installing Bambu Studio without the networking plugin, you can initiate printing through the SD card after slicing is completed.
