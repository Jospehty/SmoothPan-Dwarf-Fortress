Adding smooth sub-tile panning to Dwarf Fortress using DFHack.

## Usage

```
enable smoothpan
```

Pan the map with WASD (or arrow keys / numpad). Disable with `disable smoothpan`.

## Debug

```
smoothpan dump 30 150       # capture 30 frames after 150-frame delay
smoothpan classify 60       # log suspected HUD misclassification
```

While SmoothPan is enabled in-game:

- **F9** — dump 30 frames immediately (no console needed)
- **F10** — classify HUD blits for 60 frames

Logs and BMPs are written to `dfhack-config/smoothpan/`, with fallback to `D:\dfhack\smoothpan\`.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for design details.
