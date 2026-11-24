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

**Early prototype (Steam D3D12 validated).** Core dual-monitor capture and presentation are working on Steam (`3840x1080` split to two `1920x1080` monitors); two-controller co-op gameplay, menu polish, and Epic Games Store remain to be tested.

- [x] Proof of concept: hook the game's `Present` call and log frame info
- [x] Capture the backbuffer and split it into left/right halves
- [x] Create one borderless window per monitor with its own swapchain
- [x] Present each half on the correct monitor
- [ ] Suppress the original presentation to avoid duplicates
- [ ] Config file (monitor selection via `gothamduo.ini` works; resolution and hotkeys config pending)
- [ ] Handle menus, pause screen and the shared-camera case (`F9` currently toggles the split windows)
- [ ] Test on Steam (2-controller co-op) and Epic versions

## How it works

The game runs one instance and draws a vertical split screen (player 1 on the left, player 2 on the right) into a single window. Gotham Duo does not try to render a second camera. Instead it:

1. Resizes the game window across the combined width of both target monitors (for example `3840x1080`), so each half is a full `1920x1080` view.
2. Hooks DXGI (`CreateSwapChainForHwnd`, `Present`, `Present1`, `ResizeBuffers`) via a `version.dll` proxy.
3. Copies the left half of the D3D12 backbuffer to a borderless window on monitor 1 and the right half to monitor 2 using `CopyTextureRegion`.
4. Forwards focus back to the game window when clicking either output monitor.

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
2. Copy `version.dll` (and `gothamduo.ini` if customizing monitors) next to the shipping executable:
   `LEGOBatmanLotDK\Binaries\Win64\` (alongside `LEGOBatmanLotDK-Win64-Shipping.exe`, **not** the root launcher folder).
3. Launch the game (Windowed or Borderless mode is recommended) and enable co-op by pressing Start on the second controller.
4. Hotkeys:
   - `F9`: Toggle the dual-monitor output windows on/off.
   - `F10`: Re-apply the combined dual-monitor window resize (`3840x1080`).
5. Check `gothamduo.log` in `LEGOBatmanLotDK\Binaries\Win64\` for detected monitor indices (`Monitor[0]`, `Monitor[1]`, ...) if you want to override `Player1` / `Player2` in `gothamduo.ini`.

## Known limitations (expected)

- Menus, HUD and pause screens are drawn across both halves.
- When players are close together, the game may merge back into a shared camera, which would stretch across both monitors.
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
