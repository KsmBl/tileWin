# What's new in tileWin

The newest release first. The About page of the settings shows the section of
the version that is installed, and the taskbar points to it once after an
update. Each section starts with "## " and the version, each change is a line
of its own starting with "- ", its name in **bold**.

## 1.10

- **Codex usage widget** for the taskbar and the desktop: the 5-hour and weekly limits of the ChatGPT plan, with the same looks, styles, charts and flyout as the Claude Code widget (contributed by Hawkynt).
- **Quick web:** a popup that opens a web address typed a word per key: each key types the word given to it, the next key one of the words that follow, set up on a page of its own in the settings and drawn like the theme's dialogs.
- **Buttons answer a click at once** in the manner of each theme: the old ones push the label in, XP to 11 and modernCC press in and fade back, the Starship Console flashes. **Update now** on the Kernel page shows Starting... right away.
- **Claude Code and Codex charts** draw usage that came from another computer as a hatched box between the readings around it, instead of a climb as if it were used here, and follow a **usage reset by hand**: a session still showing the old values no longer holds the widget at them.
- **Windows of their own:** the windows of browser extensions and the copy progress of file browsers get their own taskbar button and are no longer kept on top of their app; `own_window [criteria]` does the same for others.
- **A maximized window torn off by its title bar** goes back to its size at once under the pointer, as on Windows.
- **Kernel updates:** the live fixes learn all that objtool refuses in one round instead of one module per build, find the file of a changed table also when the fix has no line of it, and the bar keeps moving while the kernel with the fixes is built.

## 1.9

- **Drag windows between desktops** in the workspaces widget on the taskbar, as on KDE: press a window's outline on one desktop's small screen, carry it to another's and let go.
- **App popups slide in** from their side and out again, the darkness fading along (or just appear, as you like), and can open **wherever their app is opened**: the taskbar, the start menu, search, the desktop and the Command Center then bring the popup.
- **Every chart answers the pointer**: the flyouts of CPU, memory, battery, disk, GPU, network and power, the Claude Code charts and the charts on the desktop show the reading under the pointer and when it was; the week of Claude Code also where the pace so far leads.
- **Claude Code usage on the desktop** as charts of the week and of the last three 5-hour windows. A window's usage only grows: an older value another session of Claude Code shows no longer pulls the charts down.
- **Wi-Fi radar:** a right click on a network opens its menu (connect or disconnect, details, connect automatically, forget), and **Bluetooth devices** are on it too, as diamonds that connect, disconnect or pair with a click.
- **Kernel updates:** the live fixes of a release now get past fixes the compiler names by a detour or the linker cannot link, as those of Linux 7.3.5 needed; they show how far they are while klp-build builds, a notification says when they cannot be made, and a release whose checksum kernel.org has not listed yet is looked at again in an hour.
- **Settings search** finds the widgets by their names and what they do.
- **Fixes:** the taskbar no longer crashes when the pointer moves over a chart on the desktop.

## 1.8

- **modernCC, an experimental theme of a faster way to work.** The **Command Center**, opened with Super or the capsule of the bar, finds apps, open windows, actions, settings pages, files and sums as you type, ranks them by how well they match and how often you use them, shows the open windows and what you use often before you type, keeps to actions, windows or settings after `>`, `@` or `#`, and offers what else can be done with the chosen row (Tab, Alt+1 to Alt+3). A top bar of floating capsules, quiet window frames and soft wallpapers, light and dark.
- **Wi-Fi radar** widget for the desktop and the taskbar, in every theme: the networks in reach as contacts on a radar whose sweep goes round, the stronger the nearer the middle. The pointer on one shows its signal, security and band; a click connects to it, a saved or open network at once with the link drawn as it is made, a new one through the field for its password. The Starship Console's moving wallpaper uses the same radar.
- **App popups:** any app on a shortcut of its own, without a frame, at the place and size you pick over a darkened screen, for example Thunderbird as high as the screen on the right. The first press starts it; the shortcut again, a click beside it or another app hides it, still running, so it comes back at once. Made on the new **App popups** page of the settings.
- **Claude Code usage charts:** the widget's flyout shows the last three 5-hour windows in one chart along the hours, and the week from its start to its reset like the chart of copying files: the usage up to the line of now and a dashed line on at the pace so far.
- **Kernel updates:** a new release whose checksum kernel.org has not listed yet is no longer taken for a broken download and thrown away; the list of kernel.org itself is asked, it is waited for, and the next run tries again instead of a day later.
- **Settings:** when the tileWin that runs is older than the settings and does not know something yet, the settings say so and offer to restart into the new one.

## 1.7

- **Starship Console, renewed.** The cooler palette of the later consoles: lavender frames, lilac bars, steel blue buttons, peach for the menu and the clock. Its menus are framed as its windows, the start menu with the elbow and a bar to MENU, and its title bars are taller and easier to grab.
- **A moving wallpaper** for the Starship Console: a sensor whose sweep goes round with the Wi-Fi networks in reach as contacts, and a STATUS panel of live readouts (time, processor and its last minute, memory, disk, network, battery, temperature). A row lights up under the pointer and opens its flyout when clicked. It rests while windows cover it and is switched off with **Moving wallpaper** on the Wallpaper page.
- **Moving windows by their title bar** works over the whole bar again in themes with a thick frame on one side, and the right edge of a Starship Console window no longer takes the window's scrollbar.
- **Answers of the port guard in the settings:** every program allowed or blocked, everywhere or on one network, to change or forget on the Network page. The question also offers **Only on** the network you are on and **This time only**, and a click on a port in the Open ports flyout opens a menu: open in the browser, copy the address, allow, block, end the program. When the guard cannot work, a notification and the Network page say why.
- **Wi-Fi flyout:** type to find a network by its name; the list keeps its place when it refreshes and while the pointer is over it.
- **Clipboard** has a page of its own under System, with **Clear** for everything but the pinned entries.
- **What's new** after an update: a notification names what the release brought, and the About page shows all of it.
- **Welcome window:** a step of widgets worth having on the taskbar, each switched on with a click.
- **Fixes:** the password dialog comes back with the screen after a switch to another console and back, instead of leaving it dark; with **Lock on resume**, the lock screen is there the moment the screen saver ends, without a glimpse of the desktop.

## 1.6

- **Open ports widget** for the taskbar and the desktop: every port a program on this computer listens on, and whether it can only be reached from here (**Local**), from a private network (**LAN**) or from every network the computer is on (**Public**). On the taskbar a network socket in the colour of the widest of them and "2 public · 9 local"; a click opens a flyout with each port, its program, protocol and address and a badge of how far it reaches, in the look of every theme.
- **Ask before a program opens a port.** Switched on in Settings → Network (off by default): when one of your programs starts to listen on a port other computers could reach, tileWin asks, as Windows' firewall does. Until you allow it, other computers cannot connect to it, while this computer still can. The answer is kept for the program, so it is not asked about again when it is started again, also on another port; for an interpreter the script or module counts, so allowing one Python server does not allow every one. It uses a small nftables table of its own that only touches programs of users, never the system's services or another firewall.
- **Starship Console theme**, in the style of the touch screens of science fiction starships (an original design): black, windows framed by round bars of colour, the taskbar a row of blocks, its own wallpaper and icons.
- **Atari ST theme**: the GEM desktop, with the menu bar along the top, windows of one black line, dithered black and white icons, and a dark scheme.
- **Lists that scroll:** the Wi-Fi flyout shows as many networks as there is room for and the rest beside a scrollbar; the widget flyouts scroll instead of saying "and 6 more". The scrollbar is dragged, clicked or turned with the wheel, in the look of the theme.
- **Emoji picker:** install.sh installs Noto Color Emoji, the font it draws in.
- **Kernel updates:** after a restart the fixes shown are those of the running kernel, and the check after a livepatch no longer misses a complaint in a full log.

## 1.5

- **Automatic kernel updates: a secure kernel without restarts.** Switched on on the Kernel page, the security fixes kernel.org publishes for the running kernel are made into a livepatch and applied while you work, every program staying open; the kernel's log is watched right after, and the fixes are taken out again if it complains. Each new release of the series is built in the background, at the lowest priority and on mains power, and starts at whatever restart you make next; when a series ends, the newest one is built. A new kernel that does not come up twice is not started again, the boot menu's default stays your distribution's kernel, and old kernels of the updates are taken off by themselves. Nothing is downloaded or built except at the time you set (every day or one day a week) or when you press **Update now**; **Stop** ends a run at once. The builds run as a system user of their own and take only what kernel.org's signing key signed.
- **The Kernel page, made simple.** A card at the top says how your kernel is (up to date, fixed live, building 69%, ready for the restart) with Update now, Stop and Restart now; below it when the updates run, what your kernel has and what was done lately. Building kernels by hand and the CPU scheduler are folded away. Choosing a kernel older than 6.19 with live patching now warns that it cannot get live fixes.
- **Kernel updates widget** for the taskbar and the desktop: a shield in the color of the state and what goes on ("Downloading 7.3.6 · 15%", "Building 7.3.6 · 69%", "Fixed live: 7.3.6", "7.3.6 at restart"), in Card, List, Ring and Bar styles, its bars in blocks in Windows 95 and XP, with a flyout in the look of the theme.
- **Drivers after a system update.** When pacman updates the distribution's kernel, the modules of the running kernel are kept until the next restart, so a driver it had not loaded yet (a USB stick, a VPN) still loads.
- **Lock and login screen:** the password and user name fields have a real cursor, Home and End, words with Ctrl, a selection with Shift and Ctrl+A.
- **Start menu** closes when a window opens from a shortcut; **dialogs** stay on top of their window and have no taskbar button of their own; **Widget settings** of a desktop widget open the settings of that very widget.
