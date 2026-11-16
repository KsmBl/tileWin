# What's new in tileWin

The newest release first. The About page of the settings shows the section of
the version that is installed, and the taskbar points to it once after an
update. Each section starts with "## " and the version, each change is a line
of its own starting with "- ", its name in **bold**.

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
