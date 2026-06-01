# SmoothPan

Smooth sub-tile WASD camera panning for **Dwarf Fortress Premium** (DF 50.x), implemented as a [DFHack](https://github.com/DFHack/dfhack) plugin.

The game continues to simulate on integer tile coordinates (`window_x` / `window_y`). Sub-pixel motion is applied only at render time by shifting map SDL blits. Mouse input is compensated so designation, hover, and UI clicks stay aligned with what you see on screen.

**Current release: 3.11.46** — functional for normal play; user-verified pan, mouse sync, mining designation, and UI clicks.

---

## Features

| Feature | Description |
|---------|-------------|
| **Smooth pan** | WASD / arrow keys pan with fractional tile motion at full refresh rate |
| **Stable HUD** | Toolbar, panels, and overlays stay fixed while the map shifts underneath |
| **Mouse sync** | World picks, mining designation drag, and UI clicks while panned (GPS + designation-only patch) |
| **Multi-z alignment** | Lower-z show-through and off-map passes shift with the main map |
| **Smart performance** | Conditional z-rebake on cliffs only; lazy minimap rebuilds during pan |
| **Diagnostics** | F7/F9 hotkeys and console commands for perf and telemetry |

---

## Quick start

### Requirements

- Dwarf Fortress **Premium** (Steam)
- DFHack built for your DF version (Windows tested)
- Plugin built as `smoothpan.plug.dll` in `hack/plugins/`

### Enable in-game

```
plugin load smoothpan
enable smoothpan
```

You should see: `SmoothPan 3.11.46 enabled`

No setup ritual required — mouse compensation boots to GPS-only automatically.

### Controls

| Input | Action |
|-------|--------|
| **WASD** / arrow keys | Smooth camera pan |
| **F7** | Detailed perf capture (~450 frames → log file) |
| **F8** | Debug: toggle GPS ↔ both mouse layers |
| **F9** | Telemetry dump (blits, mouse, shift) |
| **F10** | Classify log (HUD false-positive hunt) |
| **F11** | Pipeline probe |

---

## Default behavior (3.11.46)

These are the production defaults — tuned for **180 Hz** play with acceptable minimap lag:

| Subsystem | Default | Notes |
|-----------|---------|-------|
| Pan z-rebake | **`smart`** | Full rebake only when lower-z / cliffs visible |
| Minimap | **`lazy`** | Full rebuild at most every **2 s** while panning; sync on pan stop |
| Mouse | **`gps`** | Bump `precise_mouse_x/y` in feed/logic; designation drag also patches `mouse_x/y` briefly |

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

## Build

The plugin is a standard DFHack plugin (C++, MinHook, SDL2 headers). Source layout matches `dfhack/plugins/smoothpan/` in a full DFHack tree.

### Windows (Visual Studio + existing DFHack build)

From the plugin directory inside your DFHack checkout:

```powershell
# Build target (adjust path to your DFHack build dir)
cmake --build "D:\dfhack\build\VC2022" --target smoothpan --config Release

# Optional: deploy.ps1 copies DLL to Steam DF and verifies version string
powershell -ExecutionPolicy Bypass -File deploy.ps1
```

Output: `build/.../plugins/smoothpan/Release/smoothpan.plug.dll` → copy to `{DF}/hack/plugins/`.

After replacing the DLL:

```
plugin unload smoothpan
plugin load smoothpan
enable smoothpan
```

Keep **`version.h`** and **`deploy.ps1`** `$version` in sync on each release.

---

## Documentation

| Document | Contents |
|----------|----------|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | **How it works** — hooks, coordinates, render pipeline, subsystems |
| [docs/STATUS_3.11.md](docs/STATUS_3.11.md) | **Project status** — what's done, version history, file map |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | **Performance** — FFD policy, minimap throttling, perf captures |
| [docs/MOUSE_SYNC.md](docs/MOUSE_SYNC.md) | Mouse compensation — production model and regression checklist |
| [docs/GOLDEN_PATH.md](docs/GOLDEN_PATH.md) | Target behavior and anti-patterns (do not reintroduce) |
| [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) | F7 perf profiler, F9 telemetry, console commands |
| [docs/PIPELINE.md](docs/PIPELINE.md) | Premium DF render pipeline research notes |
| [docs/PLAN_3.11.md](docs/PLAN_3.11.md) | 3.11.0 diagnosis and fix plan (historical) |

---

## Architecture (summary)

```
WASD → feed hook → SmoothCamera.update()
                      ├─ window_x/y  (integer tiles, on boundary cross)
                      ├─ frac_x/y    (sub-tile remainder)
                      ├─ minimap flags + ffd policy
                      └─ frozen render_shift at render start

render → update_full_viewport (main + lower-z passes)
       → SDL_RenderCopy* hooks shift map blits by -render_shift
       → HUD / UI blits unshifted

feed/logic → GPS mouse bump (+render_shift on precise_mouse_x/y)
           → designation_sync during live rectangle drag (mouse_x/y + selection_rect)
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full diagram, coordinate spaces, and file map.

---

## Known limitations

- **Dwarf Fortress mode only** (`viewscreen_dwarfmodest`) — not adventure/legends
- **Windows** — physical key release uses `GetAsyncKeyState`
- **Minimap** — intentionally laggy during pan in lazy mode; catches up on pan stop
- **Cliff / open-air views** — still pay full z-rebake cost while panning (required for visual correctness)

---

## License / credits

Built on [DFHack](https://github.com/DFHack/dfhack). Uses [MinHook](https://github.com/TsudaKageyu/minhook) for SDL interception.
