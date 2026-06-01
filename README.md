# SmoothPan

Smooth sub-tile WASD camera panning for **Dwarf Fortress Premium** (DF 50.x), implemented as a [DFHack](https://github.com/DFHack/dfhack) plugin.

The game continues to simulate on integer tile coordinates (`window_x` / `window_y`). Sub-pixel motion is applied only at render time by shifting map SDL blits. Mouse input is compensated so designation, hover, and UI clicks stay aligned with what you see on screen.

**Latest release: [v1.0.0](releases/v1.0.0/)** (plugin build 3.11.46) — first public release; pan, mouse sync, and mining designation verified.

---

## Install (Windows — copy one file)

You need **two things**: DFHack (for your exact DF version) and the SmoothPan plugin DLL.

### 1. Install DFHack

If you do not already have it:

1. Download [DFHack](https://github.com/DFHack/dfhack/releases) for your **exact** Dwarf Fortress version.
2. Follow the DFHack install instructions so `hack/` exists inside your game folder.

Steam default game path:

```
C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\
```

### 2. Drop in SmoothPan

1. Download [`smoothpan.plug.dll`](releases/v1.0.0/smoothpan.plug.dll) from this repo (or grab the whole [v1.0.0](releases/v1.0.0/) folder).
2. Copy it to:

```
{Your Dwarf Fortress folder}\hack\plugins\smoothpan.plug.dll
```

Example (Steam):

```
C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress\hack\plugins\smoothpan.plug.dll
```

That is the only file you need from SmoothPan. No raw edits, no init file required (optional auto-load below).

### 3. Enable in-game

Launch the game **through DFHack**, open the DFHack console (`ctrl` + `shift` + `d` in-game if needed), then:

```
plugin load smoothpan
enable smoothpan
```

You should see:

```
SmoothPan 3.11.46 enabled
```

Enter a fortress and pan with **WASD** (or arrow keys).

### Optional — auto-load every session

Add to `dfhack-config\init\dfhack.init` (create the file if missing):

```
plugin load smoothpan
enable smoothpan
```

### Updating or removing

```
plugin unload smoothpan
```

Replace `hack\plugins\smoothpan.plug.dll`, then `plugin load smoothpan` / `enable smoothpan` again.

To uninstall: unload the plugin and delete `smoothpan.plug.dll` from `hack\plugins\`.

More detail: [releases/v1.0.0/INSTALL.txt](releases/v1.0.0/INSTALL.txt)

---

## Features

| Feature | Description |
|---------|-------------|
| **Smooth pan** | WASD / arrow keys pan with fractional tile motion at full refresh rate |
| **Stable HUD** | Toolbar, panels, and overlays stay fixed while the map shifts underneath |
| **Mouse sync** | World picks, mining designation drag, and UI clicks while panned |
| **Multi-z alignment** | Lower-z show-through and off-map passes shift with the main map |
| **Smart performance** | Conditional z-rebake on cliffs only; lazy minimap rebuilds during pan |
| **Diagnostics** | F7/F9 hotkeys and console commands for perf and telemetry |

---

## Controls

| Input | Action |
|-------|--------|
| **WASD** / arrow keys | Smooth camera pan |
| **F7** | Detailed perf capture (~450 frames → log file) |
| **F8** | Debug: toggle GPS ↔ both mouse layers |
| **F9** | Telemetry dump (blits, mouse, shift) |
| **F10** | Classify log (HUD false-positive hunt) |
| **F11** | Pipeline probe |

---

## Default behavior (v1.0.0 / build 3.11.46)

| Subsystem | Default | Notes |
|-----------|---------|-------|
| Pan z-rebake | **`smart`** | Full rebake only when lower-z / cliffs visible |
| Minimap | **`lazy`** | Full rebuild at most every **2 s** while panning; sync on pan stop |
| Mouse | **`gps`** | Automatic; no F8 ritual required |

Console tuning:

```
smoothpan                  # show current modes
smoothpan ffd smart        # pan z-rebake policy (smart | always | off)
smoothpan minimap lazy     # minimap cost (lazy | outline | full | fast)
smoothpan minimap interval 5000   # ms between mustmake rebuilds (80–60000)
smoothpan perf summary     # reprint last F7 capture summary
```

Logs: `{DF folder}/dfhack-config/smoothpan/`

---

## Building from source

Only needed if you are developing SmoothPan or your DF/DFHack version has no pre-built release yet.

Requirements: a full [DFHack](https://github.com/DFHack/dfhack) source tree, Visual Studio 2022, CMake.

Copy or symlink this repo’s `src/` into `dfhack/plugins/smoothpan/` (or use the canonical layout in `src/CMakeLists.txt`), then:

```powershell
cmake --build "{DFHack build dir}" --target smoothpan --config Release
```

Output: `plugins/smoothpan/Release/smoothpan.plug.dll` → copy to `{DF}/hack/plugins/`.

Or use `deploy.ps1` (edit `$DfPath` if your Steam install is elsewhere):

```powershell
powershell -ExecutionPolicy Bypass -File deploy.ps1
```

---

## Documentation

| Document | Contents |
|----------|----------|
| [docs/STATUS_3.11.md](docs/STATUS_3.11.md) | Project status and version history |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How it works — hooks, coordinates, pipeline |
| [docs/MOUSE_SYNC.md](docs/MOUSE_SYNC.md) | Mouse + designation compensation |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | FFD policy, minimap throttling |
| [docs/GOLDEN_PATH.md](docs/GOLDEN_PATH.md) | Target behavior and anti-patterns |
| [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) | F7/F9 tooling |
| [releases/README.md](releases/README.md) | Pre-built DLL index |

---

## Known limitations

- **Dwarf Fortress fortress mode only** — not adventure/legends
- **Windows** — tested on Windows; Linux would need a port (key release uses `GetAsyncKeyState`)
- **DFHack required** — plugin must match your DF + DFHack version; after a game update, wait for updated DFHack and a new SmoothPan release if the DLL stops loading
- **Minimap** — may lag during pan in lazy mode; catches up when you stop
- **Cliff / open-air views** — full z-rebake while panning (required for visual correctness)

---

## License / credits

Built on [DFHack](https://github.com/DFHack/dfhack). Uses [MinHook](https://github.com/TsudaKageyu/minhook) for SDL interception.
