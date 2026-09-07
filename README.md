# popn_timing_overlay

A [Spice2x](https://spice2x.github.io/) `-k` plugin that adds a real-time EARLY/LATE timing history graph.

The plugin only observes judgment timing. It does not change the game's judgment windows, result, or score calculation. The graph is rendered in a click-through transparent window, separate from the game's Direct3D rendering.

## Compatibility

Built and tested only with `M39-2026041500` on Windows x64. Other versions are unsupported.

Before installing its hook, the plugin verifies the expected judgment-function signature. On an unsupported build it leaves the game code untouched, writes a warning to the Spice log, and disables timing capture.

## Installation

1. Download `popn_timing_overlay-v1.0.0.zip` from the GitHub Release.
2. Extract `popn_timing_overlay.dll` and `popn_timing_overlay.ini` into the game directory, next to `spice64.exe`.
3. Add the plugin to your Spice launch arguments:

   ```text
   -k popn_timing_overlay.dll
   ```

4. Start the game normally. Press `F10` during play to hide or show the graph.

If you already use another `-k` plugin, add a separate `-k` argument for this DLL.

## Configuration

Edit `popn_timing_overlay.ini` before starting the game.

| Setting | Purpose |
| --- | --- |
| `RangeMs` | Horizontal timing range; values outside it are clamped |
| `History` | Number of recent successful judgments retained (`8`–`128`) |
| `VerticalPercent` | Vertical graph position as a percentage of the game viewport |
| `WidthPixels` / `HeightPixels` | Graph dimensions |
| `ResetSeconds` | Clears old samples after this much inactivity |
| `ToggleKey` | Windows virtual-key code; default `121` is `F10` |
| `ShowValue` | Shows the latest signed timing value |
| `Invert` | Reverses EARLY/LATE if your setup reports them backward |
| `JudgeZones` / `ZoneLabels` | Shows and labels GREAT/COOL/GREAT regions |
| `CoolEarlyMs` / `CoolLateMs` | COOL boundary on each side |
| `OverallAlpha` | Scales the opacity of every overlay element (`0`–`255`) |

Colors use six-digit `RRGGBB` values. A leading `#` or `0x` is accepted.

The default COOL boundaries for this supported build are `-22 ms` through `+18 ms`.

## Building

Requirements:

- Windows
- Visual Studio 2022 Build Tools with the x64 C++ toolchain
- PowerShell 5.1 or newer

Run:

```powershell
.\build.ps1
```

The build script downloads the pinned MinHook 1.3.4 source, verifies its SHA-256 checksum, and produces `build\Release\popn_timing_overlay.dll` using the static MSVC runtime.

To create the same ZIP and checksum files used by a release:

```powershell
.\package.ps1
```

## Technical notes

- Timing is captured immediately before the game's original judgment function consumes it.
- Negative values are drawn toward EARLY and positive values toward LATE.
- Only successful input judgments inside the game's active outer timing window are recorded.
- Misses and automatic timeout judgments are ignored.
- MinHook is downloaded at build time and is distributed under its own BSD 2-Clause license.

## License

Project code is released under the [MIT License](LICENSE).
