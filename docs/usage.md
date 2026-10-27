# Using tileWin

The parts of the desktop in more detail: keys, flyouts, the settings app, the themes and the screen savers. The [README](../README.md) is the short tour; every setting by hand is in [configuration.md](configuration.md).

## Updating

```sh
git pull && ./install.sh --no-deps && tilewinmsg restart
```

`restart` keeps your windows open, always: the config, the theme, the taskbar and the other programs of tileWin start afresh while the windows stay where they are. The helpers that run all session (`tilewin-nightlight`, `tilewin-clipboard`, `tilewin-polkit`) are started again when the update replaced them, so their fixes come in too; the clipboard history then keeps only its pinned entries. At its end install.sh says which of this applies: what a restart brings in, and whether the compositor itself changed. A Wayland app has no way to survive the compositor it is talking to (the display socket goes with it and no toolkit knows how to reconnect), so when an update replaced the compositor itself, the new one starts with your next login, and a notification says so. To start it at once: `tilewinmsg restart relaunch-apps` starts your apps again and puts them back where they were, `tilewinmsg restart session` only ends the session. If only the taskbar changed, `tilewinmsg restart panel` is enough.

## Volume, brightness and media keys

The volume, microphone mute, brightness and media player keys work in both modes (also on the lock screen) and show a popup like on Windows. A `bindsym` for the same key in your config replaces the default.

The same can be done from a terminal with `tilewin-media`:

```sh
tilewin-media                  # show volume, microphone, brightness and music
tilewin-media volume up        # louder (volume down: quieter)
tilewin-media volume 40        # set the volume to 40%
tilewin-media mute             # mute or unmute
tilewin-media mic              # mute or unmute the microphone
tilewin-media brightness 70    # set the brightness to 70% (up, down, +10, -10)
tilewin-media play             # play or pause (next, previous, stop)
tilewin-media help             # all examples
```

It uses `wpctl`, `pactl` or `amixer` for audio, `brightnessctl` or `light` for the backlight and `playerctl` for music and video players. The popup can also be shown on its own with `tilewinmsg panel osd volume|brightness <percent>`, `panel osd mic <muted>` or `panel osd media`.

## Notifications and the Action Center

The taskbar is the notification service of the desktop (`org.freedesktop.Notifications`), so `notify-send` and apps show their notifications on it. They pop up above the notification area as toasts (Windows 10 and 11) or balloons (95, XP and 7) and then go to the Action Center: the bell button next to the clock, `Win+N` or `tilewinmsg panel notifications`.

- Click a notification to open the app, use its buttons, or close it with ×. Hovering keeps it on screen.
- The Action Center groups the history by app (kept across restarts, up to 50), with *Clear all* and a *Do not disturb* switch. Do not disturb (`panel dnd on|off|toggle`) only lets urgent notifications pop up.
- If another notification daemon (mako, dunst, …) is running, tileWin waits and takes over when it quits.
- Theme keys: `notifications.style toast|balloon`, `notifications.center side|flyout`, `notifications.bg`, `.fg`, `.border`, `.badge`.
- Configs from older versions get the bell button after the clock; `notifications_button no` in taskbar.conf keeps it away (the Settings app writes this when you remove it).

## Quick settings

`Win+A` (or `tilewinmsg panel quicksettings`) opens quick settings like on Windows 11: buttons for Wi-Fi, Bluetooth, airplane mode, do not disturb, night light and tile mode, sliders for volume and brightness, the battery and a link to the settings. The arrows next to Wi-Fi, Bluetooth and the volume open their own flyouts. In the Windows 11 theme the network, volume and battery icons of the taskbar open it too (theme key `panel.quick_settings`, or `quick_settings yes|no` on those widgets).

Wi-Fi uses NetworkManager (`nmcli`), airplane mode `rfkill`, the volume `pactl`, the brightness `brightnessctl`.

## Clipboard history

`Win+V` (or `tilewinmsg panel clipboard`) lists the last 25 copied texts and pictures. Click one (or pick it with the arrow keys and Enter) to paste it into the window you were using; tileWin types Ctrl+V for you (Ctrl+Shift+V in terminals). Pin entries to keep them after a restart, × removes one, *Clear all* removes everything that isn't pinned.

Copies that password managers mark as secret are not recorded. The Clipboard group on the Taskbar page of the settings turns the history off (`clipboard_history no` in taskbar.conf) and can have a picked entry only copied instead of pasted (`clipboard_paste no`).

The history is kept by **tilewin-clipboard**, a program of its own that the taskbar starts. A single copied picture can be sixteen megabytes, and keeping that inside the taskbar made the taskbar look like it was growing for no reason; in its own process a look at `htop` says plainly what the memory is for. The taskbar only holds a thumbnail and the first few hundred bytes of each text. It keeps running when the taskbar restarts, so the history survives `tilewinmsg restart panel`.

## Git

The **git** widget shows the branch, the newest tag reachable from HEAD and the lines added and removed of the repository the focused window is working in:

```
main  v1.0.7  ↑2  +42 -7
```

The directory comes from the kernel rather than from the shell: the focused window has a process, and the foreground process of its terminal has a `/proc/<pid>/cwd`. Nothing is sourced into a shell, so bash, zsh, fish and the rest behave identically. Terminals that serve every window from one process (`xfce4-terminal`, `gnome-terminal` and `konsole` in their default mode) cannot be told apart this way; start them with `--disable-server` (`--disable-factory` for gnome-terminal) to give each window its own process.

The reading **stays on the last repository it saw**, so moving to a browser or an editor leaves the outstanding work on screen instead of blanking it. Clicking opens a terminal in the repository. The tooltip adds the path, what is staged, changed, untracked or conflicted, and how far the branch is from its upstream. Theme keys `git.clean`, `git.dirty` and `git.conflict` color it; `interval` (default 5 s), `show_tag`, `icon` and `max_width` are widget options.

git is asked in the background — one shell running two git commands (the tag is asked once a minute), whose answers the taskbar reads when they are ready — so the bar never waits for a slow repository, and nothing runs at all while the widget is off screen.

## Bluetooth

The Bluetooth flyout (`panel bluetooth`, or the arrow of the Bluetooth button) turns Bluetooth on and off, lists paired and nearby devices and connects, disconnects, pairs and forgets them. It talks to BlueZ directly, so `bluetoothctl` is not needed, but `bluetoothd` must run (`sudo systemctl enable --now bluetooth`). While a device pairs, codes to type on it are shown in the flyout.

## Networks and DNS

The **Network** page of the settings shows each saved network of NetworkManager: get the address by DHCP or set a fixed one (address, subnet mask, gateway, filled in from the current lease), and show or change the Wi-Fi password. Every "Network settings" link of tileWin (the network flyout, the network usage flyout) opens it. All DNS is set on the **DNS** page.

The **DNS** page drives `tilewin-dnsd`, tileWin's own DNS service, which systemd-resolved hands every lookup to while it is on (one switch; it hands DNS back to NetworkManager when turned off):

- **Servers for all networks**: in order, or of a list the fastest few: each server is timed once an hour (or as often as set) and the fastest are asked at the same time, the others standing by.
- **Servers of single networks**: pick a network and give it servers of its own (e.g. the router at home), which it asks instead of those for all networks.
- **Cache**: how long an answer is kept at least and at most, and how many are kept.
- **Prefetch**: the answers for the names asked for most (by default the top 5% of the last 7 days) are fetched again before they run out, so they are at hand at once even after nobody asked for a while. Only the questions of apps count for this, never the prefetches themselves, so a name cannot stay at the top just because it is prefetched. Names can be always or never prefetched.
- **Block lists** as on a Pi-hole: hosts files, plain lists and adblock rules from well-known lists, any address or a file of your own, downloaded once a day; names blocked and allowed by hand; blocked names answered with `0.0.0.0` or "no such name". The names blocked lately are listed, newest first with how often and whether a list or your own rule blocked them, each with a button to allow it.

The page shows what the service is doing: the servers it asks with their times, today's queries, cache hits, blocked and prefetched names, and the names it prefetches. The settings are in `/etc/tileWin/dns.conf` ([reference](configuration.md#the-dns-service-etctilewindnsconf)); the settings app changes them through `tilewin-dns-apply` with pkexec, which polkit allows administrators (the group wheel) at the computer without a password.

## Device manager

The **Device manager** page (Devices; also its own **Device Manager** entry in the start menu, `tilewin-settings --page devices`) lists every device Linux found, by kind as the Device Manager of Windows does: batteries, disk drives, display adapters, keyboards, monitors, network adapters, processors, sound, USB and the rest. A category opens with a click; one with a problem opens by itself.

- **Problems** are marked as on Windows. A device that needs a driver and has none goes to **Other devices**, as do a PCI device without one (a controller, a network or sound card) and a USB device whose functions lack one. A driver that could not load its firmware, or that failed to start the device, is a problem too. Bridges and the like that need no driver are not.
- **Properties** (double-click, Enter or the button):
  - *General*: the kind, manufacturer, location and status in words.
  - *Driver*: the driver and kernel module, the version, the package it comes with, its file, license and signer, and the firmware files it loaded.
  - *Details*: every property sysfs and udev have, picked from a list.
  - *Resources*: its IRQ, and the memory and I/O ranges.
  - *Events*: what the kernel said about the device since the start.
- **Disable**, **Enable**, **Restart** and **Scan for hardware changes** go through `tilewin-devices-root` with pkexec. A disabled device stays off after a restart (`/etc/udev/rules.d/90-tilewin-disabled.rules`) until it is enabled again. A USB device is switched off as a whole; the others are taken from their driver. Restart has the driver let go of the device and take it again, which helps one that hangs. The disk Linux runs from (also through LUKS and LVM) and every controller it hangs on are never disabled, nor the graphics of a connected screen: the page says why.
- **Show hidden devices** adds the computer's own software devices (loop devices, virtual networks), the jacks of the sound cards and the platform's parts.
- The list follows when a device is plugged in or out. Nothing is read that wakes a sleeping device (a GPU that is off stays off). `tilewin-settings --list-devices [--hidden]` prints it in a terminal.

## Kernels

The **Kernel** page builds Linux kernels of kernel.org for this computer and starts them, next to the kernel of the distribution, which stays as it is.

- **Version**: "Latest" (the newest stable release when the build starts), or any release, newest first, the long-term ones marked.
- **What goes in**: *Same as the distribution's kernel* (its configuration, also while a kernel built here runs; the new options at their defaults; builds in about an hour), *Only this computer* (the drivers this computer used, `localmodconfig`: those loaded now and, with **Remember the drivers in use** on, every one a timer noted every ten minutes since, so the USB stick of last week works too; builds in minutes), *This computer and common hardware* (and the drivers of the parts one may put in instead: graphics cards, disks, USB, Wi-Fi, sound, Bluetooth, file systems) or *Everything* (every driver that can be a module; hours). Every kernel keeps what this computer needs to start: the modules of the running kernel's initramfs (for an encrypted disk, `dm-integrity` too, loaded or not) and those of `kernel/boot.list`.
- **Compression** of the kernel, its modules and its initramfs: zstd (small and fast, the default), xz (the smallest, fits the most kernels on the EFI partition, the slowest to unpack), lz4 (the fastest start, the biggest), gzip (works with everything) or lzo. Modules can only be zstd, xz or gzip; with lz4 and lzo they stay uncompressed.
- **Live patching** (`CONFIG_LIVEPATCH`, and `CONFIG_KLP_BUILD` from Linux 6.19 on): fixes can then be applied to that kernel while it runs, see *Live fixes* below. Its build tree is kept for that (about 10 GB for *Only this computer*, more for the others). Making a fix needs patchutils, and compiles the kernel twice (with the patch and without): minutes, quicker with ccache.
- **For this processor only** (`X86_NATIVE_CPU`): built for the instructions of this computer's processor, a little faster; it may not start on another computer.
- **Rust**: the Rust drivers of the kernel (the QR code of the crash screen, NVIDIA's nova) need `rustc`, `bindgen` and `rust-src`. Without them the kernel leaves those out, which the page and the kernel's row say; **Install…** puts them on.
- **ccache**: with it installed, every build goes through it, and the next build of a kernel (a point release, the same version once more) takes minutes instead of the whole time.

The source is checked against the `sha256sums.asc` of kernel.org (and its signature, when the key is in your keyring). The build runs at a low priority in the background (`systemd-run --user`), so closing the settings does not stop it. A kernel is called `VERSION-tilewinN` (what `uname -r` says), or `VERSION-NAME` with a **Name** of your own (`7.3.0-mycustomname`: up to 32 letters, digits and `. _ + -`; a name a kernel has already is refused), and kept in `~/.local/share/tileWin/kernels`, with its log, configuration and the files DKMS builds modules with.

Each built kernel can be **added to the boot menu**, **loaded** (started straight from the running kernel with kexec: the programs end, the firmware and the boot menu are skipped; needs kexec-tools, which the page installs), **booted into** once (the computer restarts into it; the start after that is the usual one), **taken off** and **deleted**. The running kernel cannot be taken off. These go through `tilewin-kernel-root` with pkexec, which always asks for the password. What it touches and what it does not:

- Where the boot loader finds unified kernel images (systemd-boot, GRUB with its `uki` command, rEFInd, the firmware itself), the kernel becomes one: `EFI/Linux/tilewin-KVER.efi` on the EFI system partition, made by mkinitcpio (or dracut) with the same hooks and command line as the kernel of the distribution. It shows up in the boot menu by itself: grub.cfg, grubenv, `/etc/default/grub` and `loader.conf` are not touched. It is only written when the EFI partition has room for it, under a temporary name first.
- **Booting once** is systemd-boot's one-shot entry, or else the firmware's `BootNext` pointing at a boot entry made for the image without adding it to the boot order. A kernel that does not start does no harm: the next start is the usual one again.
- With a GRUB that knows no unified kernel images, `vmlinuz-KVER` and `initramfs-KVER.img` go to `/boot` and grub.cfg is made anew with `grub-mkconfig`, but only replaces the old one when `grub-script-check` finds no fault in it; the old one is kept as `grub.cfg.tilewin-backup`. Booting once is `grub-reboot` and needs `GRUB_DEFAULT=saved`.
- The modules go to `/usr/lib/modules/KVER`, marked by a `.tilewin-kernel` file; nothing without it is ever removed. DKMS builds its modules for the new kernel (VirtualBox, say). Drivers that came for the distribution's kernel only (NVIDIA's `nvidia-open`) are not in it: the page names them, and the DKMS kind of their package (`nvidia-open-dkms`) gives them to every kernel.

**Live fixes**: on the running kernel, when it was built with live patching, **Fix live…** takes a patch (a `.patch` file, a fix from the mailing list) and turns it into a livepatch module with the kernel's own `klp-build`, in the build tree kept for it. **Apply** loads it: the fix takes hold at once, and every program goes on with all it has open. **Undo** takes it out again. A livepatch changes what functions do; a change to the data the kernel keeps, or a whole new version, still takes a restart (or **Load**, which is quicker but ends the programs too). No kernel can take the place of the running one with the programs going on: that is not something Linux can do. A fix lasts until the next restart; **Apply at start** applies it at every start of that kernel (`/usr/lib/modules/KVER/livepatch`, loaded by `tilewin-kernel-livepatch.service`).

**Security fixes** (on every kernel built with live patching, running or not): kernel.org's fixes for the kernel become one livepatch module. That is what the newest stable release of its series changed since the kernel's own release (7.2.9 gets what 7.2.10 to 7.2.12 fixed; the first release of a series, 7.3, has none until 7.3.1 is out). The patch is checked against kernel.org's checksums and split by file. Each changed C file that is built into the kernel goes in; klp-build is run again without any file it refuses (a function whose data changes, a fix that needs a changed header). Left out, and listed in the fix's log: headers (the data the kernel keeps may change shape), assembly, new files, and what klp-build refused. Those need the newest release built. Applying a newer one takes the older one out. Nobody looked over a fix made this way, as a distribution does with its own livepatches: if the computer acts up after one, **Undo** takes it out again.

**CPU scheduler** (sched_ext): a scheduler of its own decides which program runs next, loaded and swapped while every program goes on running, through `scx_loader` (`scx-scheds` and `scx-tools`, which the page installs). Choose one and a mode (automatic, gaming, power saving, low latency, server) and **Use it now**, or **Start it with the computer** (`/etc/scx_loader/config.toml`). **While a window fills a screen** has the taskbar switch to a scheduler, in the mode chosen beside it (Gaming unless another is), whenever a window is fullscreen (a game), and back to what ran before once none is (`fullscreen_scheduler` and `fullscreen_scheduler_mode` in `taskbar.conf`). A rule of polkit lets administrators at the computer switch schedulers without the password.

`tilewin-kernel` does the building from a terminal too (`tilewin-kernel versions`, `build 7.3 device zstd on on [NAME]`, `list`, `cancel`, `delete KVER`, `patch-build KVER FILE`, `security-build KVER`, `livepatches KVER`, `seen`).

## Night light

`tilewin-nightlight` makes the screen colors warmer, like the night light of Windows. tileWin starts it; it does nothing while the night light is off. Turn it on with the Screen page of the settings, the quick settings or `tilewin-nightlight on|off|toggle`.

`~/.config/tileWin/nightlight.conf`:

```
temperature 3400   # 1900 (warmest) to 6500 K
schedule yes       # turn on at "from" and off at "to"
from 21:00
to 07:00
```

Turning it off by hand lasts until the next start time of the schedule. Night light needs a screen with gamma tables (most real screens; not nested or virtual ones) and no other program such as gammastep or wlsunset controlling them.

## Settings app

`tilewin-settings` (Super+I in window mode, "Settings" in the start menu, "Taskbar settings" in the taskbar menu) edits the config files for you. Type anywhere in the window (or press Ctrl+F) to search all settings; a result opens its page and highlights the setting.


| Page | What you can change |
|---|---|
| Theme | Window/tile mode and the theme, with wallpaper previews, dark mode |
| Wallpaper | Each theme's own wallpaper, your own picture per theme, or one solid color, gradient or picture for all themes; with several screens, a wallpaper of its own for each screen |
| Desktop | Desktop icons on or off, the icon size and the grid they and the widgets sit in (cell width and height, margin); the widgets on the desktop, a widget added more than once under a name of its own, and for each its style, its cells (column, row, size), seconds for the clocks, its screen and its card |
| Animations | All animations on or off and their speed; for opening, closing, minimizing, maximizing/snapping windows and switching desktops each: on or off, the style, and a preview. **Expensive calculations** lays a window out again for every frame while it is snapped, maximized or resized, instead of stretching a picture of it |
| Window behavior | Alt+Tab switcher style, Snapping to screen edges, sticking windows together and the sticking distance, the key that moves touching windows together, stretching by double-clicking a side, the key to move and resize windows anywhere, focus follows mouse, what happens when an app asks for attention, and **Gravity mode** with its drag and bounce |
| Screen | Resolution, refresh rate, scale, orientation and arrangement of the screens (asks to keep a change, like Windows), which one is the main display, brightness, dimming / screen off / lock / sleep after idle time, what the power button and closing the lid do (sleep, hibernate, hybrid sleep, ...; where the computer cannot hibernate yet it says why and lists the steps to set it up, with the commands for this computer: Secure Boot, swap on the disk as big as the memory, the resume hook of the initramfs), the lock screen command and locking before sleep, night light (strength and schedule) |
| Screen saver | The screen saver, with a little monitor that shows it running, a Preview on the whole screen, how long to wait, whether coming back shows the lock screen, and the options of the one picked: its speed, the words of 3D Text, the folder and pace of Photos |
| Sound | Output and input device with volume and mute, the volume of every app playing sound, a link to pavucontrol |
| Date & time | The clock of the computer: time server on or off, the time zone from a list or by clicking a map of every zone tzdata knows, setting date and time by hand, asking a list of time servers directly (all at once, taking the first answer, the quickest one or the middle of all of them), and the format of the taskbar clock, with every code offered as you type |
| Bluetooth | Bluetooth on/off, paired devices (connect, disconnect, remove), search and pair nearby devices |
| Network | For each saved network: the address by DHCP or fixed (address, subnet mask, gateway) and the Wi-Fi password to show and change |
| DNS | tileWin's DNS service on or off; servers for all networks, in order or the fastest of a list, timed as often as set; servers of single networks; the cache times and size; prefetching of the names asked for most (top part, days counted, names always and never prefetched, the names prefetched now); block lists like Pi-hole's with names blocked and allowed by hand |
| Taskbar | Font, layouts of both modes (position, height, widgets in the left/center/right sections, put in order by dragging each one by its handle, also from one section into another), settings of each widget (the quick launch widget's with its apps and the icon of each of them), custom script widgets, and the right-click menus of the taskbar, of the taskbar buttons and of the start button (with submenus) |
| Start menu | The style of the start menu, its pinned apps, its places and its power entries |
| Launcher & apps | Built-in launcher, rofi, wofi, fuzzel, tofi, bemenu or any command; terminal, file manager, task manager, locker and screenshot programs |
| Keyboard | Keyboard layouts and variants, layout switch shortcut, Caps Lock and Compose key, Num Lock, key repeat, and the shortcuts of window mode and tile mode (with a key recorder) |
| Mouse & touchpad | Pointer speed and acceleration, scrolling speed and direction, left-handed buttons, tap to click, tap and drag, disable while typing, touchpad scroll and click methods, swiping in from the touchscreen edges, cursor theme and size, showing the pointer when Ctrl is tapped, growing it when the mouse is shaken, pointer trail lifetime, double-click speed with a folder to try it on |
| Apps | Default apps (web browser, email, file manager, terminal, task manager, text editor, pictures, music, videos, PDF) and startup apps: turn them on or off, add or remove them. Each one can be picked from the installed apps or, with **Other...**, given any program at all: a command to type or an executable to pick off the disk, for the things that have no entry of their own. The menus, shortcuts and flyouts all use these, so a program is named in one place only. A task manager that is not installed opens whichever one the machine does have, or the terminal running btop, htop or top, instead of nothing at all |
| Kernel | Linux kernels of kernel.org built for this computer: the version, what goes in (as the running kernel, only this computer, common hardware too, everything), the compression and live patching; the kernels built, each added to the boot menu, loaded with kexec, booted into once, taken off or deleted; how this computer boots, the room on the EFI partition and the drivers a new kernel will not have |
| Backup | All settings saved to one `.tar.gz` file and restored from one (after asking); a restore keeps the settings it replaces, listed there to go back to. `tilewin-settings --backup FILE` / `--restore FILE` do the same without the window |
| Account | Your account picture (`~/.face`, shown on the lock screen and login screens) and name |
| About | What this computer is running, the way a fetch tool prints it but laid out as a page: the operating system, model, kernel, uptime, packages, shell and locale; the tileWin version, mode, theme, screens and terminal; the processor, graphics, memory, swap, disk and battery. The logo beside it can be the normal one, a tiny one or an uwu one, and one button copies the whole page as text |

Neither the menus nor the start menu has to be typed: on both pages **Add item...** and **Choose...** pick an installed app, one of tileWin's own actions (arrange windows, show the desktop, task view, run, lock, shut down, switch theme, ...) or a folder and fill in the label, icon and command, the icon button opens a grid of the icons of your icon theme and of the installed apps, and a second button makes an entry bold, checked or greyed out.

Changes apply immediately (the compositor gets the matching command, the taskbar reloads its config) and are written to `common.conf` and `taskbar.conf`. Only the changed lines are rewritten, so your comments and formatting stay. Open a page directly with `tilewin-settings --page taskbar`. The app only runs while its window is open.

## Themes

```sh
tilewin-theme list              # * marks the active theme, [window mode] [tile mode] their themes
tilewin-theme set win95         # theme of the current mode, applies immediately
tilewin-theme set win10 tile    # theme of tile mode, used when you switch to it
tilewin-theme current tile
tilewin-theme info win7
```

Window mode and tile mode each remember their own theme: switching the mode (Super+Shift+W) also switches to the theme that mode used last. Choosing a theme in the taskbar menu or with `theme <name>` changes the theme of the current mode; in tileWin Settings, "Theme for" picks the mode whose theme you change. Over IPC: `theme <name> [window|tile]`.

| Theme | Look |
|---|---|
| `win1` | Windows 1.0 as it looked on EGA: bright blue title bars with the title white on black in the middle, the system box with its three bars on the left (a click opens the window menu, a double click closes the window) and the zoom box on the right, thin black borders, inactive title bars dithered with white, yellow menus, and the cyan of the icon area as the desktop and the taskbar, where minimized windows lie as icons; its windows did not overlap, so it suits tile mode |
| `win3` | Windows 3.x: navy title bars with the title in the middle, the control-menu box (a click opens the window menu, a double click closes the window) and arrow buttons, thick grey borders notched at the corners, flat white menus, the plain light grey desktop Windows 3.1 came with, the **Program Manager** as the Start menu (program groups made from the apps' categories, the pinned apps as Main; a group opens as a window of icons) and minimized windows as **icons along the bottom of the desktop** (a double click restores one) |
| `win95` | Classic gray bevels, navy title gradient, teal desktop, cascading start menu |
| `win2000` | Windows 2000: the classic look of 95 in the warmer grey of its 3D face, title bars fading from navy to light blue, the plain blue desktop and Tahoma |
| `winxp` | Luna blue title bars, green start button, two-column start menu |
| `winxp-dark` | Windows XP in black: the same glossy Luna title bars, taskbar, Start menu and dialogs in black and greys, under a night sky; apps go dark while it is on |
| `vista` | Windows Vista: the glass of 7 a shade smokier, a taskbar of black glass with the window titles on its buttons, the dark Start orb, a Start menu framed in dark glass, and an aurora of green and blue light |
| `win7` | Aero glass title bars, orb start button, icons-only superbar |
| `win8` | Colored window frames with centered titles, translucent blue taskbar, a start screen of colored tiles over the whole screen |
| `win10` | Flat white title bars, dark taskbar with search box, list start menu |
| `win11` | Rounded light title bars, centered taskbar, grid start menu |
| `sway` | A sway + waybar desktop: dark Catppuccin-like colors, 3px lavender borders and gaps in tile mode, a floating bar with rounded groups and colored widgets, GoMono Nerd Font |

Your own themes, and every key a theme can set: [docs/configuration.md](configuration.md#your-own-themes-themeconf).

## Icons

Every built-in theme draws its own icons in its style: the file manager, terminal, browser, text editor, calculator, image viewer, task manager, mail and media player, folders (Documents, Pictures, Music, Downloads, ...), common file types, the trash and the start menu entries. They appear in the taskbar, start menu, title bars, Alt+Tab and on the desktop; apps without a themed icon keep their own.

- `icons/aliases` maps app icon names and desktop entry categories to these icons, e.g. `web-browser firefox chromium @WebBrowser`. Remove a name there to keep an app's own icon. A window that names itself after its build, like `vivaldi-stable` or `google-chrome-beta`, also finds the icon of the plain name.
- `icons { set winxp }` in `theme.conf` uses the icons of another theme; `icons { theme <name> }` is the icon theme for everything else.
- The notification area icons (volume, network, battery, brightness) are `icons/tray-*.svg`; `tray { icons symbolic }` tints them with the taskbar text color (Windows 10 and 11).
- The SVGs are generated by `tools/gen-icons.py` (only needed to change them).

**Other apps** use the theme's icons as well: tileWin turns them into the icon theme `~/.local/share/icons/tileWin-<theme>` (everything it has no icon for comes from your own icon theme) and switches GTK (GNOME settings and `settings.ini`), KDE (`kdeglobals`) and qt5ct/qt6ct to it whenever the theme changes. A theme with `icons { theme <name> }` but no icons of its own, like Sway with `breeze`, sets that icon theme. Your icon theme is remembered and comes back when you turn this off, in the settings app (Theme, "Theme icons in apps") or with:

```sh
tilewin-app-icons off      # apps use your own icon theme again
tilewin-app-icons on
tilewin-app-icons status
```

Running apps pick up the change right away when they follow the GNOME settings (most GTK apps); others after a restart.

## Wallpapers

Each theme has its own default wallpaper:
- **win95:** the classic teal desktop.
- **The other themes:** original look-alike artwork (`themes/<name>/wallpaper.svg`). Microsoft's own wallpapers are copyrighted and can't be included.

If you have your own copy of an original wallpaper (or any picture you like for a theme), save it as `~/.config/tileWin/wallpapers/<theme>.jpg` (or `.png`, `.webp`, `.svg`), e.g. `~/.config/tileWin/wallpapers/winxp.jpg`. tileWin then uses it whenever that theme is active. `wallpaper ...` in `common.conf` overrides the wallpaper for all themes, and `output_wallpaper <screen> ...` gives one screen a wallpaper of its own.

Wallpapers are rendered once per screen size and cached in `~/.cache/tileWin/wallpapers`.

## Your own screen savers

Each screen saver is a directory: `saver.so`, a module that draws it, and
`screenshot.png`, its picture in the list of the settings. tileWin's own are
installed in `/usr/local/share/tileWin/screensavers`; put a directory of your
own into `~/.local/share/tileWin/screensavers` and it is on the Screen saver
page, with its settings and the preview, and `tilewin-screensaver` shows it.
`tilewin-screensaver --list` shows every saver found and where it is, and
`tilewin-screensaver --screenshot <directory> <file>` draws a saver's picture.

How to write one, with a whole example (a lava lamp) to start from:
[screensaver/README.md](screensaver/README.md).
