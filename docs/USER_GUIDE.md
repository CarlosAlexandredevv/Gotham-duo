# Gotham Duo – Player Guide

Play **LEGO Batman: Legacy of the Dark Knight** local co-op with **one monitor per player**.

## 1. Install

### Easiest: the installer

1. Download the latest `gotham-duo-<version>-win64.zip` from the [Releases](https://github.com/CarlosAlexandredevv/Gotham-duo/releases) page and extract it anywhere.
2. **Close the game.**
3. Double-click **`install.bat`** and choose **[1] Install**.

The installer finds your Steam or Epic Games Store copy automatically (or asks for the folder), checks that the game is really there, keeps your existing `gothamduo.ini`, and never overwrites another mod's `version.dll`.

### Manual

Copy `version.dll` and `gothamduo.ini` into the folder that contains `LEGOBatmanLotDK-Win64-Shipping.exe`:

| Store | Typical location |
| --- | --- |
| Steam | `<Steam library>\steamapps\common\LEGO Batman - Legacy of the Dark Knight\LEGOBatmanLotDK\Binaries\Win64\` (default library: `C:\Program Files (x86)\Steam`) |
| Epic Games Store | `<Epic install folder>\<game folder>\LEGOBatmanLotDK\Binaries\Win64\` (default: `C:\Program Files\Epic Games\`) |

> Use the `Binaries\Win64` folder, **not** the game's root launcher folder.
> Steam: right-click the game → *Manage* → *Browse local files* to open the install folder quickly.

## 2. Uninstall

- **Installer:** double-click `install.bat` and choose **[2] Uninstall**. It removes `version.dll` and `gothamduo.log`, and asks before deleting `gothamduo.ini`.
- **Manual:** delete `version.dll`, `gothamduo.ini` and `gothamduo.log` from `...\Binaries\Win64\`.

## 3. Controls & hotkeys

| Input | What it does |
| --- | --- |
| `Start` on Controller 2 | Solo → Duo (second monitor turns on) |
| Hold `Back + Start` on Controller 2 (800 ms) | Duo → Solo |
| `F9` | Toggle Solo ↔ Duo from the keyboard |
| `F1` | In Duo: force/stop the mirrored full-screen menu layout (press again for Auto) |
| `F10` | In Duo: re-apply the window resize if the picture looks wrong |

Hotkeys can be rebound or disabled in `gothamduo.ini` (see below). The game starts in **Solo** mode.

## 4. Configuration (`gothamduo.ini`)

Edit with Notepad; changes apply the next time the game starts. Missing or invalid values fall back to the defaults and a warning is written to `gothamduo.log`. Deleting the file restores all defaults.

### `[Monitors]`
| Option | Default | Meaning |
| --- | --- | --- |
| `Player1`, `Player2` | `-1` | Monitor numbers from `gothamduo.log` (`Monitor[0]`, `Monitor[1]`, …). `-1` = pick the two landscape monitors left-to-right. Both must be set (and different) to override. |

### `[Input]`
| Option | Default | Meaning |
| --- | --- | --- |
| `AllowControllerToggle` | `1` | `1` = Controller 2 `Start` / `Back+Start` switch modes; `0` = keyboard only. |
| `P2ControllerIndex` | `-1` | `-1` watches controllers 2–4; `0`–`3` picks one specific controller slot. |
| `HoldBackStartMs` | `800` | How long to hold `Back+Start` to return to Solo. |

### `[Hotkeys]`
| Option | Default | Meaning |
| --- | --- | --- |
| `ToggleDuoKey` | `F9` | Solo ↔ Duo. |
| `ToggleMirrorKey` | `F1` | Manual menu-mirror override. |
| `ReapplyResizeKey` | `F10` | Re-apply window resize. |

Accepted values: `F1`–`F24`, `A`–`Z`, `D0`–`D9`, `Num0`–`Num9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `PrintScreen`, a hex key code such as `0x78`, or `None` to disable.

### `[MenuMirror]`
| Option | Default | Meaning |
| --- | --- | --- |
| `AutoDetect` | `1` | Automatically show menus, loading screens and cutscenes mirrored on both monitors. `0` = only with the `F1` hotkey. |
| `EnterMirrorMs` | `200` | How long a shared screen must be stable before mirroring. |
| `ExitMirrorMs` | `100` | How long split-screen must be stable before splitting again. |

### `[Display]`
| Option | Default | Meaning |
| --- | --- | --- |
| `TargetWidth`, `TargetHeight` | `0` | Override the combined Duo size in pixels (each player = width ÷ 2 × height). `0` = automatic. |
| `SoloWidth`, `SoloHeight` | `0` | Override the Solo / mirrored size. `0` = automatic. |

By default both players render at the **smaller** monitor's resolution; mixed resolutions, ultrawide and portrait monitors are scaled/letterboxed, never cropped.

### `[Performance]`
| Option | Default | Meaning |
| --- | --- | --- |
| `SuppressGamePresent` | `1` | In Duo, skip the game's own (hidden) frame presentation to save GPU work. Set `0` if you see stutter or frame-pacing problems. |

### `[Logging]`
| Option | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Write `gothamduo.log` next to `version.dll`. |
| `Verbose` | `0` | Extra diagnostic lines (only for troubleshooting). |

## 5. Troubleshooting / FAQ

**Nothing happens / no `gothamduo.log` appears.**
`version.dll` is in the wrong folder. It must sit next to `LEGOBatmanLotDK-Win64-Shipping.exe` in `...\LEGOBatmanLotDK\Binaries\Win64\`. Also check that `[Logging] Enabled=1`.

**Only one monitor / "Need at least 2 monitors" in the log.**
Windows must be set to **Extend** these displays (not Duplicate). Press `Win + P` → *Extend*.

**Players are on the wrong monitors (swapped).**
Open `gothamduo.log`, note the `Monitor[n]` numbers, and set `Player1=` / `Player2=` in `gothamduo.ini` (Player 1 gets the left half).

**The picture is cropped, offset or the wrong size.**
Use **Borderless / Windowed** in the game's video options (not exclusive Fullscreen), then press `F10` in Duo mode. If Windows scaling is above 100%, right-click the game `.exe` → *Properties* → *Compatibility* → *Change high DPI settings* → *Override* → **Application**.

**Menus look stretched across both monitors.**
Press `F1` to toggle the mirrored menu layout, or set `AutoDetect=1` in `[MenuMirror]`.

**Alt+Tab shows the Duo windows over other apps.**
Update to the latest release; Duo windows hide automatically when the game loses focus.

**My antivirus flags `version.dll`.**
Gotham Duo is a proxy DLL that hooks the game's graphics calls, which some antivirus tools flag by heuristic. The source code and checksums (`SHA256SUMS.txt`) are published on every release; you can build it yourself (see the README) or add an exclusion for the game's `Win64` folder.

**Stutter or frame-pacing issues in Duo mode.**
Set `SuppressGamePresent=0` in `[Performance]`, then report it (see below).

**The installer says the game is running / can't find the game.**
Close the game first. If detection fails, run `install.ps1 -Action Install -GamePath "<game install folder>"`.

## 6. Reporting a bug

[Open an issue](https://github.com/CarlosAlexandredevv/Gotham-duo/issues/new/choose) and attach **`gothamduo.log`** from `...\Binaries\Win64\` (re-create the problem first, then close the game so the log is complete). Please include your GPU, monitor setup (resolutions, scaling), and store (Steam/Epic).
