# Configuring tileWin

Everything the settings app sets is plain text in `~/.config/tileWin/`, and can be written by hand too. This is the reference of those files; the
[README](../README.md) describes what tileWin does.

## The files


All configuration lives in `~/.config/tileWin/` and is plain text:

| File | Purpose |
|---|---|
| `common.conf` | Settings shared by both modes: programs, outputs, inputs, wallpaper, autostart |
| `tilemode.conf` | Tile mode: a normal sway config (`man 5 sway`) |
| `windowmode.conf` | Window mode: same syntax, with Windows-style shortcuts |
| `taskbar.conf` | Taskbar layouts, widgets, menus and start menu |
| `current-theme` | Name of the active theme (managed by `tilewin-theme`) |
| `themes/<name>/` | Your own themes |

Both mode configs `include common.conf`. Missing files fall back to the installed defaults in `/usr/local/share/tileWin/config/`. Reload with `Super+Shift+C` or `tilewinmsg reload`. The taskbar reloads its config automatically when you save it.

## Commands added by tileWin

Use these in configs, key bindings, menus, or with `tilewinmsg <command>`. Window commands act on the focused window or on `[criteria]`.

| Command | Description |
|---|---|
| `wm_mode tile\|window\|toggle` | Switch mode (`tilewinmsg mode ...` is a shortcut) |
| `theme <name>` | Switch theme |
| `color_scheme light\|dark\|toggle` | Light or dark variant of the theme; also runs `tilewin-color-scheme` for GTK, GNOME and KDE apps |
| `accent wallpaper\|theme` | The accent color of the themes (Windows 10 and 11) from the most striking color of the wallpaper, or the theme's own |
| `session_restore yes\|no` | Reopen the apps of the last session at login (default yes, set in `common.conf`) |
| `maximize [enable\|disable\|toggle]` | Maximize a floating window |
| `minimize [enable\|disable\|toggle]` | Minimize to the taskbar (tile mode: scratchpad) |
| `snap left\|right\|up\|down\|topleft\|topright\|bottomleft\|bottomright\|restore` | Snap a window |
| `snap left\|right\|topleft\|topright\|bottomleft\|bottomright <fraction> [<fraction>]` | Snap a window into that part of a layout whose lines are at those fractions of the screen (`snap left 0.66`: two thirds) |
| `snap_layouts enable\|disable` | Resting on the maximize button offers layouts to snap into (default enable) |
| `battery_saver off\|<percent>` | On battery at or below that level (default 20, 100: always on battery) the animations, the pointer trail and the background charts of the taskbar widgets stop until the computer is plugged in; the taskbar says so in a notification |
| `arrange cascade\|vertical\|horizontal\|optimal` | Arrange the windows of the focused workspace. Tile mode uses the matching split layouts. |
| `showdesktop` | Minimize all windows / restore them |
| `always_on_top [enable\|disable\|toggle]` | Keeps a floating window over the other ones; also in the right-click menu of a window |
| `alttab next\|prev\|commit\|cancel` | Window switcher |
| `taskview [toggle\|open\|close]` | Task view (Win+Tab) |
| `desktop new\|close\|rename [name]\|move left\|move right` | Create a desktop, close the current one (its windows move to the desktop before it), give it a name (`rename` with nothing after it takes the name away again), or move it one place in the desktop order |
| `restart [panel\|session\|relaunch-apps]` | Restart the taskbar, or tileWin. Plain `restart` keeps the session and the windows open unless the compositor binary has been replaced; `session` ends the session either way and `relaunch-apps` starts the apps again afterwards |
| `panel <action>` | Taskbar actions: `startmenu [toggle\|search\|close]`, `run`, `calendar`, `network`, `volume`, `power`, `shutdown [logoff]`, `memory`, `desktop refresh\|new folder\|new text\|new shortcut\|folder\|terminal`, `activate <n>`, `window_menu`, `menu <name>`, `reload` |
| `launcher` | Open the application launcher |
| `launcher_command builtin\|<command>` | Launcher to use: the built-in one, or e.g. `rofi -show drun` |
| `panel_command <cmd>\|none` | Taskbar program started and restarted by tileWin |
| `wallpaper theme\|none\|solid <color>\|gradient <c1> <c2> [vertical\|horizontal]\|image <path> [fill\|fit\|stretch\|center\|tile]` | Wallpaper drawn by the compositor (`tile` repeats the picture over the screen) |
| `output_wallpaper <screen> <wallpaper>\|default` | A wallpaper of its own for one screen, with the arguments of `wallpaper`; the screen is its name (`DP-1`) or `"make model serial"`. `default` gives it the wallpaper of every screen again |
| `main_output <screen>\|auto` | The main display: it gets the desktop icons and the taskbar that `outputs main` fills with the windows of every screen. `auto` (default) takes the screen at the top left |
| `idle_timeout dim\|screen_off\|lock\|sleep\|screensaver <seconds>\|never` | After that long without input: dim the screen, turn it off, lock it (`lock_command`), sleep, or start the screen saver (`screensaver_command`), which input ends again. Apps that keep the screen on (videos) pause it. |
| `focus_priority off\|raised\|high\|highest\|<nice>` | The process of the focused window (all its threads) runs at a lower nice value, so it gets the processor first and draws fast: raised is -5, high -10, highest -15, or a value from -20 to 0; when another window gets the focus, the one before gets its own nice value back, and a process running lower already keeps its value (default off; the Window behavior page, "Priority of the focused window"). Lowering a nice value takes CAP_SYS_NICE: `install.sh` gives it to `tilewin` (`sudo setcap cap_sys_nice=ep $(which tilewin)` by hand); without it the setting does nothing and says so once in the log. With it tileWin also runs itself with the round-robin scheduling sway uses when it may |
| `screensaver_lock yes\|no` | Lock the screen (`lock_command`) when someone comes back to the screen saver, like "On resume, display the logon screen" (default no) |
| `screensaver_command <command>` | The screen saver program (default `tilewin-screensaver`); which saver it shows is the `screensaver { name <saver> }` block of `taskbar.conf`, with `speed`, `stats` (frames a second and CPU use in a corner), `text` (3D Text; `time` shows the clock), `photos` (a folder) and `photo_seconds`, and each saver's own settings as `<saver>_<setting> <value>` (e.g. `pipes_joints balls`, `matrix_color red`, `doomsday_kind blizzard`; the commented `taskbar.conf` lists them all, and the Screen saver page of the settings shows those of the saver picked). `tilewin-screensaver --list` lists the savers |
| `screensaver start\|stop` | Starts the screen saver right away, as if the computer had been left alone (the keys that ran the command do not end it), or ends it |
| `lid_action closed\|docked default\|nothing\|sleep\|hibernate\|hybrid_sleep\|lock\|screen_off\|shutdown` | What closing the laptop lid does, without and with an external screen: `sleep` stands by in RAM, `hibernate` saves to disk and turns off, `hybrid_sleep` does both. Anything but `default` takes over lid handling from logind. Where the computer cannot hibernate (logind's CanHibernate, e.g. no swap partition or file on disk as big as the memory, only zram), `hibernate` and `hybrid_sleep` sleep instead. |
| `power_key_action default\|nothing\|sleep\|hibernate\|hybrid_sleep\|lock\|screen_off\|shutdown` | What pressing the power key does, also on the lock screen. Anything but `default` takes over the power key from logind. `hibernate` and `hybrid_sleep` sleep where the computer cannot hibernate, like for the lid. |
| `lock_on_sleep yes\|no` | Lock the screen (`lock_command`) before the computer sleeps, hibernates or goes into hybrid sleep, however that was asked for: the power key, the lid, a menu, idle time or `systemctl`. tileWin holds logind back until the lock screen is up, at most 3 seconds (default yes) |
| `animations enable\|disable` | Animations of opening, closing, minimizing, maximizing and snapping windows and of switching desktops (default enable) |
| `animation_speed <factor>` | Faster (e.g. `2`) or slower (e.g. `0.5`) animations |
| `expensive_calculations on` | While a window is snapped, maximized or resized, lay it out again for every frame instead of stretching a picture of it: never the wrong shape, at the cost of a redraw per frame |
| `animation open\|close\|minimize\|maximize\|desktop <style> [enable\|disable]` | Style of one animation, or turn it off. open: `rise` (default), `fade`, `zoom`, `pop`, `drop`; close: `shrink`, `fade`, `grow`, `drop`, `explode` (shatters with fire, smoke, sparks and a shock wave); minimize: `taskbar`, `fade`, `shrink`, `drop`; maximize (also snapping): `morph`, `bounce`, `fade`; desktop: `slide`, `vertical`, `fade`, `zoom` |
| `window_gravity enable\|disable` | Gravity mode: a window let go of while it is still moving carries on sliding and comes back off the edges of the screen. Not a pull downwards; it behaves like a flat thing pushed across a table (default disable) |
| `window_gravity_drag <factor>` | How quickly a sliding window comes to rest, 0.5 to 10 (default 3) |
| `window_gravity_bounce <factor>` | How much speed it keeps at an edge, 0 to 1 (default 0.5) |
| `window_stick enable\|disable` | Moved and resized windows stick to the edges of other windows and of the screen (default enable) |
| `window_stick_distance <pixels>` | How close an edge has to come to stick (default 12) |
| `window_snap enable\|disable` | Dragging a window to a screen edge or corner snaps it (default enable) |
| `window_stretch enable\|disable` | Double-clicking a side of a window stretches it (default enable) |
| `window_stretch_mode both\|side` | Whether stretching pulls the window out on both sides of that axis or only towards the side that was clicked (default both) |
| `window_group_modifier <key>\|none` | Held while dragging a window, the windows touching it move along, snapped ones in the size they have (default `Shift`; e.g. `Alt`, `Ctrl+Alt`) |
| `alttab_style theme\|icons\|flip3d` | What the Alt+Tab switcher shows: what the theme has (default), a grid of icons, or a 3D stack of the windows themselves |
| `pointer_shake enable\|disable` | Shaking the mouse quickly back and forth makes the pointer grow, like on KDE (default disable) |
| `pointer_shake_max <percent>` | How big it gets, in percent of the normal pointer, 100 to 100000 (default 5000) |
| `pointer_shake_rate <percent>` | How fast it grows and shrinks again, in percent per second, 10 to 5000 (default 400) |
| `pointer_shake_shakes <count>` | Changes of direction per second before it starts growing (default 6) |
| `pointer_locate theme\|enable\|disable` | Tapping Ctrl on its own draws rings that shrink onto the pointer; `theme` (default) leaves it to `pointer { locate }` of the theme |
| `not_responding enable\|disable` | Show apps that do not answer as "(Not Responding)" and offer to end them when they are closed (default enable) |
| `end_task` | Ends the app of the window at once (SIGKILL), as "Close the program" of the not-responding dialog does; `[con_id=…] end_task` for another window |
| `pause_minimized off\|<seconds>\|<n>m` | Stop apps all of whose windows have been minimized that long, until one is shown again (default off) |
| `pause_minimized_sound keep\|pause` | Whether minimized apps playing sound keep running (default keep; asks `pactl`) |
| `pause_minimized_except <app>...` | Apps that never stop, by desktop id (`firefox.desktop`) or app id; `none` empties the list |
| `remember_windows enable\|disable` | Apps open where their window was last closed (default enable) |
| `remember_windows_except <app>...` | Apps that open as new windows do; `none` empties the list |
| `peek <con_id>\|desktop\|off` | Shows only that window, or only the desktop, the other windows as outlines, until `peek off` or a click (the taskbar uses it) |
| `magnifier Alt\|Super\|Ctrl\|Shift\|off` | The key that zooms the screen with the scroll wheel (default Alt) |
| `magnify in\|out\|off\|<factor>` | Zooms the screen around the pointer, 1 to 32 |
| `pointer_trail <ms>` | Copies of the pointer stay behind it while it moves, like the mouse trails of Windows; each copy fades away that many milliseconds after it was left, 0 to 2000 (default 0, off). While `pointer_shake` has the pointer grown, the copies it leaves are grown as well |
| `double_click_time <ms>` | How long after the first click the second one still makes a double-click, in title bars, on window frames and on the desktop (default 400) |
| `xdg_autostart enable\|disable` | Start the apps of `~/.config/autostart` and `/etc/xdg/autostart` when tileWin starts (default enable). Programs that already run are not started twice. |
| `lock_command <command>` | Lock screen used by `idle_timeout lock`, `lid_action ... lock`, `power_key_action lock` and `lock_on_sleep` (default `tilewin-lock -f`) |

- `tilewinmsg -t get_tilewin` prints the current mode, theme, panel pid and main display.
- IPC clients can subscribe to `["tilewin"]` events.
- The `get_tree` output has `minimized` and `maximized` fields.

## The taskbar: taskbar.conf

`~/.config/tileWin/taskbar.conf` uses sway-style blocks. The installed file is fully commented; the short version:

```
layout window {
    position bottom
    left start search taskbar
    right tray keyboard volume network battery clock notifications showdesktop
}
layout tile {
    position top
    height 26
    left workspaces title
    right tray cpu memory clock modeswitch
}

widget clock { format "%H:%M\n%d.%m.%Y"; on_click panel calendar }
widget custom:weather {
    exec "curl -sf 'https://wttr.in/?format=%c+%t'"
    interval 900
    on_click exec xdg-open https://wttr.in
}

menu taskbar {
    item "Cascade windows" arrange cascade
    item "Show windows stacked" arrange vertical
    item "Show windows side by side" arrange horizontal
    item "Arrange windows optimally" arrange optimal
    separator
    submenu "Tools" { item "htop" exec xfce4-terminal -e htop }
}
```

**Widgets:**

| Widget | Options |
|---|---|
| `start` | `label`, `width` |
| `taskbar` | `icons_only theme\|yes\|no`, `group`, `workspaces current\|all`, `outputs current\|all\|main` (the windows of the bar's own screen, of every screen, or of every screen on the main display's bar and the own ones elsewhere, as on Windows), `middle_click close\|new`, `max_width`, `thumbnails yes\|no` (live window previews when hovering a button, instead of the title tooltip; theme key `taskbar.thumbnails`), `peek yes\|no` (resting on a preview shows only its window; theme key `taskbar.peek`) |
| `quicklaunch` | `item <desktop-id or command> [icon]`; on the Taskbar page, Quick launch has **Add app…** and **Add command…** (a command of your own, then its icon), and the button beside an entry changes its icon |
| `workspaces` | (none) |
| `title` | `max_width`; click for the window: its desktop, screen and process, and Minimize, Maximize and Close |
| `tray` | (none) |
| `clock` | `format`, `tooltip_format` (strftime), `settings` (command of the flyout's "Change date and time" link); click opens the clock flyout: an analog and a digital clock, the calendar (the wheel and ←/→ change the month) and that link |
| `notifications` | `always yes` shows the button also in the 95/XP/7 themes when there are no new notifications |
| `volume` | `format "{volume}%"`, `mixer`, `step`, left click opens the volume flyout (`mixer` is its link) |
| `network` | `interface`, `interval`, `settings` (command of the flyout's settings link), left click opens the Wi-Fi flyout (a wired connection shows the speed and duplex the link runs at, e.g. "1 Gbit/s, full duplex"; a saved network has a **Show password** button: it finds the saved connection of the network even when the connection was given another name, reads the WPA key, the WEP key or the 802.1X password, NetworkManager may ask for authorization first, and clicking the password copies it) |
| `battery` | `device`, `format "{capacity}% {status}"`, `interval`, `settings` (adds a link to the flyout), left click opens the power flyout |
| `cpu` | `format "CPU {usage}%"`, `style text\|graph`, `interval`, `task_manager <command>`; click for a popup with the usage of the last minute, the cores, load, up time and the processes using the most CPU (`panel cpu` opens it too) |
| `memory` | `format "{used_percent}% {used}/{total} GiB"`, `interval`, `task_manager <command>`; click for a flyout with the usage of the last minute, what the memory is made up of, the swap and the processes holding the most of it (`panel memory` opens it too) |
| `gpu` | Load of a graphics card: `device card0`, or `command <cmd>` for cards that report nothing in `/sys` (e.g. `nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits`), `format "GPU {usage}%"`, `interval`; click for a flyout with the load of the last minute, video memory, temperature, power and clock where the card tells them (from `nvidia-smi` for cards that tell `/sys` nothing) |
| `net` | What goes through a network interface: `device wlan0` (the busiest one by default), `format "↓ {down} ↑ {up}"` with `{down}`, `{up}`, `{total}` and `{device}`, `max_rate <KiB/s>` for the scale of the chart (default 12500), `interval`; click for a flyout with down and up over the last minute, each connection with its address, and what went in and out since they came up |
| `storage` | How full a file system is: `path /home` (default `/`), `format "{path} {used_percent}%"` with `{used}`, `{free}` and `{total}` in GiB, `interval`; click for every drive with how full it is (clicking one opens it) |
| `power` | Watts the computer is drawing: `device BAT0`, `format "{watts} W"`, `max_watts` for the scale (default 60), `interval`; click for a flyout with the draw of the last minute and the battery: charge, state, energy, time left, health and charge cycles (RAPL measures the processor where there is no battery) |
| `disk` | A round LED that lights up while the disks are busy, like the drive lamp of a PC; while they are idle it stays a dark lens. `devices nvme0n1` picks the disk it watches (empty watches every whole disk, no partitions; several names separated by spaces also work). The settings app offers the disks of the machine in a dropdown, with their model and size. `threshold <KiB/s>` before it lights up (default 50), `interval` (default 1), `format "{rate}"` (`{rate}` is e.g. `1.2 MB/s`, `{kbps}` the plain number; empty shows only the lamp). Themes color the lit lamp with `disk { active_fg }`; click for a flyout with reading and writing over the last minute, each disk and what was read and written since the computer started |

The usage widgets (`cpu`, `memory`, `gpu`, `net`, `storage`, `power`) all take `style text|graph|bar` (text, a chart of the last measurements, or a bar), `width` for the chart and the bar, `warning` and `critical` levels in percent with `warning_fg` and `critical_fg` colors, and `fg` for the normal color; without them the theme's `<type>.warning`, `<type>.critical` and `<type>.fg` decide. Several of the same kind can be used at once by naming them `gpu:1`, `net:wlan0`, `storage:home` and so on.
| `brightness` | (none; scroll changes it via brightnessctl) |
| `keyboard` | (none); click for the layouts to pick from, middle click switches to the next one straight away |
| `modeswitch` | (none) |
| `showdesktop` | `width`, `peek yes\|no` (resting on it shows the desktop through the windows) |
| `search` | `label`, `width` |
| `separator` | `width` |
| `spacer` | `width <px>\|expand` |
| `custom:<name>` | `exec` + `interval`, or `exec_listen` for long-running scripts, `format "{}"`, `icon`; click for what the script said last in full, with a button that runs it again |

**Custom scripts** print one line per update: plain text, or JSON such as `{"text": "...", "tooltip": "...", "icon": "..."}`.

Every widget that shows something opens a flyout when clicked. The git widget's flyout shows the branch, how far it is from its upstream, what is staged, changed and untracked and the latest commits, with **Open a terminal here** and **Open the folder** (a click used to open the terminal straight away).

All widgets are described in one place, `common/tw_widgets.c`: their names, what they are, whether they can go on the taskbar, on the desktop or both, and their options. The taskbar and the settings app both read that list, so a widget and its options are never described twice.

**Widgets on the desktop:**

```
desktop_widgets {
    clock { style analog; column -1; row 0 }   # the last two columns, at the top
    clock:digital { style digital; column -3; row 2 }
    cpu { style chart }                        # the first free cells at the upper right
    power { style chart; size 4x2 }
    custom:weather { column 0; row -1; output all }
}
```

| Option | |
|---|---|
| `style` | How it looks; the first is the default. Every widget: `compact` (as on the taskbar, drawn bigger) and `tile` (with its name under it). The meters `cpu`, `memory`, `gpu`, `net`, `storage` and `power`: `chart` (3x2 cells), `gauge` (2x2), `ring` (2x2), `bar` (3x1). `volume`, `network`, `battery` and `brightness`: `ring`, `gauge`, `bar`. `disk`: `chart`. `clock`: `digital` (3x2), `analog` (2x2), `binary` (3x2) |
| `column`, `row` | The cell of its upper left corner in the grid of the desktop icons, counted from 0; negative numbers count from the right and the bottom, `-1` being the last column or row. Without them a widget takes the free cells closest to the upper right corner. Two cards never cover each other: one put onto another moves to the closest free cells |
| `size` | Cells it takes, `<columns>x<rows>`; the style decides by default, and `compact` is as wide as it needs |
| `seconds` | `yes` or `no`: the analog clock has a second hand and the binary clock shows the seconds (default `yes`); the digital clock shows them only with `yes` |
| `output` | `main` (the main display, default), `all`, or a screen by name |
| `background` | `yes` (default) or `no`: the card. Without it the widget is drawn in white with a shadow, like the icon labels |

The size of the cells is `desktop_icon_width` and `desktop_icon_height`, so bigger cells make the widgets bigger too. Any option of the widget itself can be given here too and then only applies on the desktop. A widget can be on the desktop more than once under names of its own (`clock:digital`, `cpu:2`). A right click on a card without a menu of its own offers **Put back in its place** and the settings.

All styles and their sizes are listed with the widgets in `common/tw_widgets.c`; `panel/gadgets.c` draws them.

**Commands and menus:**
- A `themes` line in a submenu (`submenu "Theme" { themes }`, the default in the taskbar's right-click menu) lists every installed theme, the current one checked; the Menus settings add it with "Add theme list".
- Every widget accepts `on_click`, `on_middle_click`, `on_right_click`, `on_scroll_up`, `on_scroll_down` and a `menu { ... }` block.
- Commands are tileWin commands (`exec ...`, `arrange cascade`, ...) or `panel <action>`.
- `menu taskbar` is the right-click menu of the empty taskbar area.
- `menu start` is the right-click menu of the start button.
- `menu window` adds entries to the menu of taskbar buttons. `{id}` is replaced with the window's con_id.
- `menu desktop` adds entries to the right-click menu of the empty desktop.

The `startmenu { }` block sets the style of the menu (`layout theme|classic|twocolumn|list|tiles|centered`, `theme` being the one the current theme asks for), pinned apps (`pinned <desktop ids>`), `place "Label" <icon> <command>` links and `power "Label" <command>` entries.

### Accent color

A theme writes `$accent` where Windows uses its accent color, `$accent_light` and `$accent_dark` for lighter and darker shades, each with an optional alpha (`$accent/d0`). It is the theme's `accent { color #0078d7 }`, or with `accent wallpaper` (the switch on the Theme page of the settings) the most striking color of the wallpaper of the first screen, worked out whenever the wallpaper changes and kept in `~/.cache/tileWin/accent`.

### Snap layouts

The layouts on a window's maximize button, in order (up to eight). Without the block the layouts of Windows 11 show. The Window behavior page of the settings edits the list.

```
snap_layouts {
    layout columns 0.66              # two side by side, the line at two thirds
    layout quarters 0.5 0.5          # four quarters
    layout left_quarters 0.5 0.5     # one on the left, two on the right
    layout quarters_right 0.66 0.5   # two on the left, one on the right
}
```

The numbers are where the line down and the line across go, 0.1 to 0.9 of the screen.

## Your own themes: theme.conf

A theme is a directory with:
- `theme.conf`: decorations, taskbar, menus, start menu, Alt+Tab, wallpaper
- `tile.conf`: sway `client.*` colors and font for tile mode
- `icons/`: the theme's own icons (optional)

To make your own:
1. Create `~/.config/tileWin/themes/mytheme/theme.conf` starting with `inherit win10`.
2. Override only the keys you want, e.g. `decoration { active { title_bg #202020; title_fg #ffffff } }`.
3. Look at the built-in theme files for all available keys.

Keys for the widgets on the desktop:
- `desktop_widget { look classic|luna|aero|metro|flat|fluent }` picks how the cards and their charts, gauges and clocks are drawn; by default it follows `style` (win95, winxp, win7, win8, win10, win11).
- `desktop_widget { bg; bg2; fg; dim; accent; accent2; border; track; grid; face; warning; critical; radius; font }` sets their colors; `bg2` is the lower end of the gradient of the XP and 7 looks, `accent2` the green of their progress bars, `face` the face of charts and gauges. Put them in the `dark { }` block for the dark scheme. Without them each look has colors of its own for light and dark; the Windows 8 look gives every widget a tile color of its own. The Sway theme colors them like its bar.

Keys for the flyouts of the taskbar widgets (CPU, memory, network, volume, battery, Bluetooth, clock, quick settings, notifications and the others):
- `flyout { bg; fg; disabled_fg; border; radius; field_bg; field_fg; font }` color the box; without them it takes the `menu { }` colors. `bg_gradient` fills it with a gradient instead.
- `flyout { frame_width 4; frame <color>; frame_gradient "..."; frame_gloss "..."; inner_border <color> }` draws a frame around the content: the Luna blue of Windows XP, the glass of Windows 7.
- `flyout { accent; link; bar }` color switches and buttons, the links at the bottom and the filled part of usage bars; `chart_bg; chart_grid; chart_line; chart_radius` the charts of the last minute (green on black, as the Task Manager drew them, in the 95 and XP themes). On Windows 95 the charts are sunken and the bars filled with blocks.
- Every built-in theme sets them to match its taskbar and Start menu: the dark panes of Windows 8 and 10, glass on 7, the Luna frame on XP, the raised grey box on 95. Put them in the `dark { }` block for the dark scheme.

Keys for a whole theme:
- `scheme dark` (or `light`) makes a theme one of a single scheme: its `dark { }` colors are always used, and apps (GTK, GNOME, KDE) follow it while it is on, whatever scheme is chosen in the settings; switching to another theme brings the chosen one back. Windows XP (dark) uses it.
- `dialog { bg; fg; heading_fg; field_bg; field_fg; field_border; button_gradient; button_fg; button_border; default_border }` color the Run and "not responding" dialogs; the Windows XP ones take the title bar and frame of the theme's windows.
- `startmenu { divider; header_line; right_hl_bg; separator; dim }` color the lines, the highlight of the right column and the second lines of the two-column (XP and 7) Start menu; `shutdown { line_gradient; middle_gradient }` the XP shut down dialog.

Keys for Alt+Tab:
- `alttab { style flip3d }` shows the windows themselves as a 3D stack that flies past, like Flip 3D on Windows 7, instead of the grid of icons. `alttab { wash <color> }` is what the desktop behind it is covered with. The Windows 7 theme uses it; the others show icons (`style icons`).

Keys for the pointer:
- `pointer { locate yes }` draws gray rings shrinking onto the pointer when Ctrl is tapped on its own, like "Show location of pointer when I press the CTRL key" on Windows. `pointer { locate_color <color> }` colors them. Ctrl held as part of a shortcut or a Ctrl+click does nothing. The Windows XP, 7, 8, 10 and 11 themes turn it on.

Keys for a bar like waybar (see `themes/sway/theme.conf`):
- `panel { margin 20; margin_side 20 }` keeps the bar away from the screen edges.
- `panel { groups yes; group_bg <color>; group_border <color>; group_radius 14; group_inset 3; group_padding 6 }` draws a rounded background behind the left, center and right widgets instead of one full bar.
- `workspaces { style pill; active_bg; active_fg; hover_bg; urgent_bg; urgent_fg; radius; inset }` draws rounded workspace buttons.
- `<widget> { fg <color>; format "..." }` sets the text color and default format of a widget type, e.g. `cpu { fg #7eb8f7; format "󰍛 {usage}%" }`. A `format` in `taskbar.conf` wins unless the theme sets `panel { theme_formats yes }`.
- Like waybar's `format-icons`, volume, battery, brightness and network take `format "{icon} {volume}%"` with `icons "<low> ... <high>"` (text icons replace the drawn glyph), plus `format_muted`, `format_charging`, `format_full`, `format_plugged`, `format_disconnected` and `format_ethernet`.
- `cpu`, `memory` and `battery` take `warning`/`critical` levels and `warning_fg`/`critical_fg` colors (for the battery the levels count down).
- `workspaces { padding 9; margin 12; icon { 1 <icon>; 2 <icon>; urgent ! } }` replaces workspace names with icons; `padding` is the space inside a button, `margin` around all buttons.
- `start { icon "<text>"; icon_font "<font>" }` shows a text icon such as a Nerd Font logo on the start button (flat and fluent styles).
- `shutdown { style classic|luna|security|tiles }` picks the shut down dialog (default: from the theme style). `tint`, `bg`, `fg`, `accent`, `button_bg`, `hover_bg`, `border`, `radius` and `font` color the security and tiles looks, `scale` enlarges the XP dialog, `title_bg`, `header_bg` and `footer_bg` color the 95 and XP dialogs. `startmenu { power_dialog yes|no }` decides whether the start menu's power button opens it or a menu (default: yes for Windows 95 and XP).
- `panel { item_padding 12; item_inset 5; item_radius 12 }` sets the space around widgets and their hover shape; `tooltip { radius 12 }` rounds tooltips.
- `layout { tile { position top; height 44; left ...; center ...; right ... } window { ... } }` gives the theme its own taskbar layouts; `theme_layout no` in `taskbar.conf` keeps yours.
- `decoration { frame_width 3 }` draws thicker window frames with the Windows 11 style.
