# GroupLauncher

macOS-style app groups ("stacks") for the Windows 11 taskbar. Pin a group, click it, and a popup with all of its apps slides up above the taskbar.

- **Lightweight:** one ~190 KB exe, written in C against the Win32 API. No runtime, no installer, no background process. The popup starts when you click a group and exits once you have picked an app.
- **Groups are plain folders:** every group is a folder of shortcuts under `%USERPROFILE%\Groups`. Back them up or move them by copying that folder.
- **Manager window:** create, rename and delete groups, add apps from a searchable list of everything installed (including Microsoft Store apps), remove apps, and choose an icon.
- **Group icons:** a grid made from the group's app icons, a searchable Windows icon (Segoe Fluent Icons) on one of 10 tile colors, or any `.ico` / `.dll` / `.exe` icon.
- **Pinning:** uses Windows' own "pin to taskbar" confirmation where the system allows it, and falls back to a one-step drag otherwise.
- **Correct placement:** per-monitor DPI aware, and the popup stays inside the monitor's work area, so it never covers the taskbar, whichever edge the taskbar is on.
- **Follows Windows:** dark/light mode, accent color, and the "Animation effects" setting.

## Requirements

- Windows 11, x64 (tested). Windows 10 mostly works but is not tested; there, pinning falls back to dragging.

## Install

1. Download `GroupLauncher.exe` from the [latest release](../../releases/latest).
2. Run it. It copies itself to `%LOCALAPPDATA%\Programs\GroupLauncher` (no admin rights needed), adds a **GroupLauncher** entry to the Start menu, and opens the manager.

The exe is not code-signed, so Windows SmartScreen may show "Windows protected your PC" the first time. Click **More info**, then **Run anyway**. The full source is in this repository if you'd rather build it yourself (see below).

## Usage

1. Open **GroupLauncher** from the Start menu.
2. Click **New group**, type a name, and tick the apps (use the search box).
3. Click **Pin to taskbar** and confirm Windows' prompt.

Click the pinned group to open the popup. Click an app to start it; click outside, press <kbd>Esc</kbd> or click the group again to close it. Arrow keys and <kbd>Enter</kbd> work too.

Right-click inside the popup to remove an app, add apps, open the manager, or open the group folder.

An app that's missing from the list (for example a portable program with no Start menu entry) can be added with **Browse for a file...**, or by putting a shortcut to it in `shell:programs` (press <kbd>Win</kbd>+<kbd>R</kbd> and type that).

## Where things are stored

| What | Where |
|---|---|
| Program | `%LOCALAPPDATA%\Programs\GroupLauncher\GroupLauncher.exe` |
| Generated group icons | `%LOCALAPPDATA%\Programs\GroupLauncher\icons\` |
| Groups (one folder each) | `%USERPROFILE%\Groups\` |
| Start menu entries | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\GroupLauncher.lnk` and `...\Programs\Groups\` |

Each group folder also holds two hidden files: `groupid.txt` (the group's ID for the taskbar) and, if set, `groupicon.txt` (the chosen icon).

## Uninstall

1. Unpin the groups from the taskbar.
2. Delete `%LOCALAPPDATA%\Programs\GroupLauncher`, the Start menu entries listed above, and `%USERPROFILE%\Groups` if you no longer need your groups.

## Known limitations

- **Don't click "Pin to taskbar" for a group that is already pinned.** That rebuilds the icon file the pinned button still uses, and the button turns blank until it is pinned again. To change the icon of a pinned group: unpin it, change the icon, pin it again.
- Icon search uses Microsoft's English icon names.
- The dark context menus use an undocumented Windows setting (also used by apps such as Notepad++). If a Windows update changes it, the menus simply turn light.

## Building from source

You need MinGW-w64 (on Linux/WSL: `gcc-mingw-w64-x86-64`, or MSYS2 on Windows). Then run:

```sh
./build.sh
```

The output is `build/GroupLauncher.exe`. Everything is in `src/grouplauncher.c`; the icon and manifest (per-monitor DPI awareness, common controls v6) are in `res/`.

## License

[GNU General Public License v3.0](LICENSE). You may use, change and share GroupLauncher, but any distributed version, including a modified one, must be released under the GPL-3.0 with its source code.
