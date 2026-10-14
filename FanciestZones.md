# FanciestZones reference

FanciestZones is a personal fork of PowerToys FancyZones. It docks windows by drag instead of snapping them to zones drawn in advance, in the style of TrellisWM on macOS. This page covers how to run it, use it, configure it and install it on another computer.

## Run it

| What | Where |
|---|---|
| Executable | `C:\Users\carlo\Tools\FanciestZones\x64\Debug\FanciestZones.exe` |
| Starts at sign-in through | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\FanciestZones.lnk` |
| Stop it | End `FanciestZones.exe` in Task Manager |
| Start it again | Run the shortcut or the executable |
| Logs | `%LOCALAPPDATA%\Microsoft\PowerToys\FancyZones\Logs` |

Two rules keep it running:

- Keep **FancyZones turned off in PowerToys**. The fork and the stock FancyZones share one instance lock, so only the one that starts first runs.
- Do not turn FancyZones on and then off in PowerToys. Turning it off sends a shutdown signal that the fork also obeys. If the fork closes, start it again.

## Use it

### Dock with the mouse

Hold **Shift** while you drag a window (see `fancyzones_shiftDrag` to change that). A preview shows where the window lands.

| Drop the window | Result |
|---|---|
| Near an edge of the screen | It takes that half of the screen |
| On the outer part of a docked window | It splits that window on that side. Splits nest. |
| On an empty half | It fills it |
| In the middle of a docked window | Nothing: the window floats |

- **Undock:** drag the window without Shift. Its neighbors take the space. Holding the title bar without moving the window leaves it docked.
- **Smallest place:** a window isn't docked where it would get less than 100 pixels in width or height.
- **Resize:** drag a border of a docked window. The neighbors follow when you release the mouse.
- **Minimize, restore, close:** the neighbors take the space. A restored window returns to its place.

### Move with the keyboard

Win+arrows work when `fancyzones_overrideSnapHotkeys` is on.

| Window | Win+arrow does |
|---|---|
| Floating | Docks it on that side of its monitor |
| Docked, with a neighbor on that side | Swaps the two. Also moves it into an empty half. |
| Docked, with nothing on that side | Moves it to the monitor on that side, docked on the edge it came in from |

Shift+Win+arrows, with the same setting on, move the focus instead: from a docked window to the docked window on that side, on the next monitor too. Nothing moves, and nothing happens when there is no docked window on that side. On a window that isn't docked the keys keep their Windows meaning (move the window to the other monitor, stretch it vertically); on a docked window they lose it.

### Save and recall layouts

| Keys | Action |
|---|---|
| Win+Ctrl+Alt+Shift+digit | Save the layout of the monitor under the cursor to that digit |
| Win+Ctrl+Alt+digit | Recall it on the monitor under the cursor |

Saving a monitor with nothing docked changes nothing: the layout already saved under that digit is kept. The keys are ignored while you drag a window.

### What it remembers

Layouts are kept per monitor and per virtual desktop in `dock-layouts.json`, and they survive a restart. When a docked window is gone at startup, its place is kept for that app, and the next window of the app takes it. After a reboot, apps return to their places as you open them. A kept place can also be filled by hand: Shift-drag any window onto it. Layouts of virtual desktops that were deleted are dropped.

To forget all layouts and saved digits, stop the fork, delete `%LOCALAPPDATA%\Microsoft\PowerToys\FancyZones\dock-layouts.json`, and start the fork again.

## Configure it

All settings are in one file, shared with stock FancyZones:

`%LOCALAPPDATA%\Microsoft\PowerToys\FancyZones\settings.json`

Edit the file in a text editor and save it. The fork reloads it right away, with no restart. Each setting is an entry under `properties` with a `value`:

```json
{
  "properties": {
    "fancyzones_dockGap": { "value": 8 },
    "fancyzones_shiftDrag": { "value": true }
  },
  "name": "FancyZones",
  "version": "1.0"
}
```

### Docking

These three settings exist only in the fork.

| Setting | Meaning | Default | Allowed |
|---|---|---|---|
| `fancyzones_dockGap` | Pixels left between docked windows. A change is applied to the docked windows at once. | 8 | 0 to 100 |
| `fancyzones_dockEdgeBand` | How close to a screen edge, in pixels, the cursor must be for the window to take half of the screen | 48 | 8 to 500 |
| `fancyzones_dockWindowEdgePercent` | How far into a docked window, in percent from each of its edges, the cursor splits that window. The rest of the window is the middle, where nothing happens. | 30 | 5 to 50 |

A value outside the allowed range is brought back to the nearest allowed value.

### Behavior

| Setting | Meaning | Default |
|---|---|---|
| `fancyzones_shiftDrag` | `true`: dock only while Shift is held. `false`: dock on every drag, and Shift turns docking off. | `true` |
| `fancyzones_mouseSwitch` | A right click during a drag toggles docking | `false` |
| `fancyzones_overrideSnapHotkeys` | Win+arrows move windows in the layout | `false` |
| `fancyzones_excluded_apps` | Apps that are never docked: one name or path fragment per line (`\r` or `\n` between entries in the JSON string) | empty |
| `fancyzones_allowChildWindowSnap` | Also dock windows that have an owner window, such as dialogs | `false` |
| `fancyzones_restoreSize` | A window that is undocked gets back the size it had before | `false` |

### Look

| Setting | Meaning | Default |
|---|---|---|
| `fancyzones_zoneHighlightColor` | Color of the drop preview, as `#RRGGBB` | `#008CFF` |
| `fancyzones_highlight_opacity` | Opacity of the drop preview, 0 to 100 | 50 |
| `fancyzones_zoneBorderColor` | Border color of the drop preview | `#FFFFFF` |
| `fancyzones_makeDraggedWindowTransparent` | The dragged window is see-through while docking is active | `true` |
| `fancyzones_layoutNameLabelEnabled` | Show a label when a layout is saved or recalled | `true` |

### Settings that no longer do anything

The fork ignores the zone settings: the zone editor hotkey, zone numbers, "show zones on all monitors", "move newly created windows to their last known zone", "move windows based on their position", and the overlapping zones choice.

### About the PowerToys settings window

The FancyZones page of PowerToys Settings writes the same file. Two cautions:

- Its controls are expected to be unavailable while FancyZones is off in PowerToys, which is how the fork needs it. This was not checked.
- PowerToys only knows its own settings. If it rewrites the file, it may drop the three docking settings. The fork then uses their defaults until you add them again.

A copy of the file from before the docking settings were added is next to it: `settings.json.before-dock-settings.bak`.

## Known limits

- Windows that are not aware of per-monitor scaling sit a few pixels inside the outer screen edges when the monitors use different scaling. Stock FancyZones does the same.
- A window that crosses between monitors of different scaling can land about 4 pixels off until the layout changes again.
- Layouts are stored per monitor. When a monitor is turned off or its identity changes, the windows docked on it stay where they are but are no longer in a layout. Dock them again.
- The hotkeys for saved layouts are fixed in code.
- Windows of apps that run as administrator can't be docked unless the fork runs as administrator too.
- Docking is off when "span zones across monitors" is on.

## Install it on another computer

The release build is one file, `FanciestZones.exe`. It needs Windows 10 or 11 (64-bit) and nothing else: no PowerToys, no Visual Studio, no runtime to install.

1. On this computer, build the release version (see "Rebuild it", with `-p:Configuration=Release`). The file is `C:\Users\carlo\Tools\FanciestZones\x64\Release\FanciestZones.exe`.
2. Copy that file to the other computer, for example to `%LOCALAPPDATA%\FanciestZones\`.
3. If PowerToys is installed there, turn FancyZones off in it.
4. Make it start at sign-in, and start it now. In PowerShell:

   ```powershell
   $exe = "$env:LOCALAPPDATA\FanciestZones\FanciestZones.exe"
   $link = (New-Object -ComObject WScript.Shell).CreateShortcut("$([Environment]::GetFolderPath('Startup'))\FanciestZones.lnk")
   $link.TargetPath = $exe; $link.Save()
   Start-Process $exe
   ```

5. Optional: copy your `settings.json` to `%LOCALAPPDATA%\Microsoft\PowerToys\FancyZones\` on the other computer (create the folder if it isn't there). Without it the defaults apply: docking needs Shift, and Win+arrows are left to Windows. Don't copy `dock-layouts.json`: layouts belong to the monitors of one computer.

Things to expect:

- The file isn't signed. If you download it instead of copying it from a drive, Windows SmartScreen may ask before the first run: choose "More info", then "Run anyway".
- To update, end `FanciestZones.exe`, replace the file, and start it again.

This was checked on this computer only: the release file, copied alone into an empty folder, passed the whole live test. It was not tried on a computer without PowerToys.

## Rebuild it

Only the C++ part of PowerToys builds with Visual Studio 2022, and that is all the fork needs. Stop the fork first, because the executable is locked while it runs.

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  C:\Users\carlo\Tools\FanciestZones\src\modules\fancyzones\FancyZones\FancyZones.vcxproj `
  -p:Platform=x64 -p:Configuration=Debug "-p:SolutionDir=C:\Users\carlo\Tools\FanciestZones\\" -m
```

The result is `x64\Debug\FanciestZones.exe`, which is what the startup shortcut on this computer runs. With `-p:Configuration=Release` the result is `x64\Release\FanciestZones.exe`, the one to copy to other computers: the debug file only runs where Visual Studio is installed.

`tools\dock-live-test\docktest.ps1 -Monitor 0` (or `-Monitor 1`) checks the whole feature with real test windows. It takes over the mouse and keyboard for about a minute.
