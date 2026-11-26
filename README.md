# Gotham Duo

> Experimental dual-monitor mod for local split-screen co-op: one monitor per player.
> Built for **LEGO Batman: Legacy of the Dark Knight** (PC).

![status](https://img.shields.io/badge/status-pre--alpha-orange)
![platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![license](https://img.shields.io/badge/license-MIT-green)

---

## What is this?

LEGO Batman: Legacy of the Dark Knight lets two players explore the open world with no distance limit, but the game always renders both views as a split screen on a single display.

**Gotham Duo** aims to send each player's view to its own monitor:

- Monitor 1 → Player 1
- Monitor 2 → Player 2

The goal is local co-op only. The game has no online or LAN co-op, and this project does not add any.

## Status

**Early prototype (Steam D3D12 validated).** Core Solo/Duo switching and dual-monitor capture/presentation are working on Steam (`1920x1080` Solo on the primary monitor, switching to `3840x1080` split across two `1920x1080` monitors in Duo mode); two-controller co-op gameplay, menu polish, and Epic Games Store remain to be tested.

- [x] Proof of concept: hook the game's `Present` call and log frame info
- [x] Capture the backbuffer and split it into left/right halves
- [x] Create one borderless window per monitor with its own swapchain
- [x] Present each half on the correct monitor
- [x] Automatic Solo/Duo mode switching (starts in Solo; switches to Duo on P2 `Start` or `F9`, returns to Solo on `Back + Start` hold or `F9`)
- [ ] Suppress the original presentation to avoid duplicates
- [ ] Config file (monitor selection and input triggers via `gothamduo.ini` work; full hotkey/resolution config pending)
- [x] Handle menus, pause screen, shared camera, and centered subtitles/HUD prompts (menus, loading screens, cutscenes, and shared-camera scenes auto-switch to native `1920x1080` mirrored across both monitors in Duo mode via structural D3D12 detection with `F1` override; centered subtitles and shared HUD banners over active split-screen are replicated onto each monitor via D3D12 Slate draw replay)
- [ ] Test on Steam (2-controller co-op) and Epic versions

## How it works

The game runs one instance and draws a vertical split screen (player 1 on the left, player 2 on the right) into a single window when two players are active. Gotham Duo does not try to render a second camera. Instead it hooks DXGI (`CreateSwapChainForHwnd`, `Present`, `Present1`, `ResizeBuffers`) via a `version.dll` proxy and manages two runtime modes:

1. **Solo mode (default at launch):** Keeps the game on the primary monitor (`1920x1080` borderless/fullscreen) with the extra output windows hidden and zero overlay/copy overhead.
2. **Duo mode (on Player 2 `Start` or `F9`):** Resizes the game window across the combined width of both target monitors (for example `3840x1080`) so each half is a full `1920x1080` view, creates per-monitor borderless output windows, copies the left half of the D3D12 backbuffer to monitor 1 and the right half to monitor 2 using `CopyTextureRegion`, and forwards focus back to the game window when clicking either output monitor.
3. **Returning to Solo mode (`Back + Start` held for 800 ms on Controller 2, or `F9`):** Releases the extra D3D12 swapchains, hides the output windows, and restores the game window to the primary monitor.

## Requirements (planned)

- Windows 10/11, x64
- LEGO Batman: Legacy of the Dark Knight (PC, Steam or Epic Games Store)
- Two monitors, ideally with the same resolution
- Two controllers
- A GPU able to handle the extra copy work at your target resolution

## Try it without any mod

Before this project is usable, you can test a similar effect with no code:

- Combine both monitors into one with Nvidia Surround or AMD Eyefinity and set the game to a double-wide resolution, or
- Use a borderless window spanning both monitors, or
- Use OBS windowed projectors with a crop filter on each half of the game window.

These workarounds are not verified for this specific game. Results may vary, and menus and HUD will span both monitors.

## Project structure

```text
├── config/          # Default gothamduo.ini template shipped with releases
├── docs/
│   ├── design/      # Architecture specs and implementation plans
│   └── dev/         # Developer testing notes
├── src/
│   ├── proxy/       # version.dll proxy, MASM trampolines, DLL entry point
│   ├── hooks/       # DXGI & D3D12 hooks and Slate draw-replay probe
│   ├── output/      # Dual-monitor window/swapchain output, resize guard, seam detector
│   ├── input/       # XInput controller / hotkey Solo↔Duo triggers
│   └── log.h        # Shared lightweight file logger
├── tests/           # CTest unit tests
└── tools/           # Local dev helper scripts (not shipped with the mod)
```

## Building

```bash
git clone https://github.com/CarlosAlexandredevv/Gotham-duo.git
cd Gotham-duo
cmake -B build -A x64
cmake --build build --config Release
```

The compiled proxy DLL will be at `build/Release/version.dll`.

Dependencies:

- C++20 (MSVC + MASM x64)
- Windows SDK (Direct3D 12, DXGI, Win32)
- [MinHook](https://github.com/TsudaKageyu/minhook) `v1.3.4` (fetched automatically by CMake `FetchContent`)

## Installation

1. Build or download `version.dll` (and optionally [config/gothamduo.ini](config/gothamduo.ini)).
2. Copy `version.dll` (and `gothamduo.ini` if customizing monitors or controller triggers) next to the shipping executable:
   `LEGOBatmanLotDK\Binaries\Win64\` (alongside `LEGOBatmanLotDK-Win64-Shipping.exe`, **not** the root launcher folder).
3. Launch the game (Windowed or Borderless mode is recommended). It starts in **Solo mode** on the primary monitor (`1920x1080`).
4. Switching modes:
   - **Solo → Duo:** Press `Start` on Controller 2 (auto-detected) or press `F9`.
   - **Duo → Solo:** Hold `Back + Start` on Controller 2 for `800 ms` (configurable) or press `F9`.
   - `F1`: Override mirrored `1920x1080` menu mode manually in Duo mode (press again to return to `Auto`).
   - `F10`: Re-apply the combined dual-monitor window resize (`3840x1080` or `1920x1080`) while in Duo mode.
5. Check `gothamduo.log` in `LEGOBatmanLotDK\Binaries\Win64\` for detected monitor indices (`Monitor[0]`, `Monitor[1]`, ...) if you want to override `Player1` / `Player2` in `gothamduo.ini`. Under `[Input]`, you can also configure `AllowControllerToggle` (`1` or `0`), `P2ControllerIndex` (`-1` to watch slots `1..3`, or `0..3` for a specific XInput slot), and `HoldBackStartMs` (default `800`). Under `[MenuMirror]`, you can configure `AutoDetect` (`1` default for structural D3D12 auto-mirroring on menus, loading, and cutscenes; `0` for manual `F1` only), `EnterMirrorMs` (default `200`), and `ExitMirrorMs` (default `100`).

## Known limitations (expected)

- Menus, loading screens, cutscenes, and shared-camera scenes auto-mirror at native `1920x1080` across both monitors in Duo mode (`AutoDetect=1` / `F1`), and centered subtitles/HUD banners over split-screen are replicated onto both monitors. Custom off-center widgets that span the seam without symmetric centering may still require per-screen tuning.
- Input may be ignored when the game window does not have focus.
- Audio stays on a single output.
- Anti-tamper or anti-cheat protection could block hooks. This has not been checked yet.

## Contributing

Ideas, bug reports and pull requests are welcome, especially:

- Notes on how the game's renderer presents frames
- Findings from RenderDoc captures
- Tests on different GPUs, monitor setups and launchers

## Disclaimer

This is an unofficial, fan-made project. It is not affiliated with, endorsed by, or sponsored by LEGO, DC, Warner Bros. Games, or TT Games. All trademarks belong to their respective owners.

Use at your own risk. Back up your saves before testing. This project is intended for local, offline co-op and does not bypass any copy protection. You need your own legitimate copy of the game.

## License

MIT. See [LICENSE](LICENSE).
