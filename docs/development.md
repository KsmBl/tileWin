# Building and testing

How to install is in the [README](../README.md#installation).

## Tests

```sh
meson test -C build-release --suite unit   # the config parser, the config editor, the disk list, the widget list, the screen savers, moving taskbar widgets and the settings coverage
meson test -C build-release --suite gui    # drives a nested tileWin and looks at what it does
```

All tests run side by side, as many at once as the computer has cores
(`--num-processes` sets fewer): each `gui` test starts a nested tileWin of its
own, with its own config, state, socket and temporary folder, and learns its
Wayland display from a file the nested session writes, never by looking for a
new socket, which another test may have just made. A new test has to keep it
so. The whole run takes about three minutes with 28 at once, the `unit` suite about
a minute; while working on one thing, run only its tests by name
(`meson test -C build-release portal flyout-keyboard`), and everything before a
commit.

The coverage test walks the sources and fails when the taskbar or the
compositor reads a setting that no page of the settings app offers, so nothing
ends up settable only by hand. The `gui` suite starts a nested tileWin and:

- opens every page of the settings app and counts the pixels it drew, so a page
  that lays itself out to nothing cannot pass unnoticed;
- clicks a switch with a pointer of its own and reads the config file back, to
  check that it really saves what it was set to;
- drags a widget of the Taskbar page by its handle to another place and reads
  `taskbar.conf` back, to check that the new order is what was saved;
- copies texts and pictures and reads the clipboard history back over the
  socket of `tilewin-clipboard`, then weighs both processes to check that a
  copied picture is held by that program and not by the taskbar;
- checks that a `$taskmanager` nobody has installed is replaced by one that is,
  and a `$filemanager` by the default app for folders,
  and that `expensive_calculations on` makes a window being maximized pass
  through sizes on the way instead of jumping to the end;
- opens a window, restarts, and checks that the very same window is still
  there and the compositor never went, then replaces the binary on disk and
  checks that a restart does end the session;
- types a program into **Other...** on the Apps page with a keyboard of its
  own and reads `common.conf` back, so a dialog that opens and closes without
  saving cannot pass;
- opens a terminal in a repository of its own and checks the git widget puts
  the branch on the bar, and keeps it there once the window has gone;
- names a desktop and checks the name stays with it when it is moved, while
  the number still says where it sits;
- runs two screens of different sizes and checks that a maximized window moved
  to the other one stays maximized and focused, that `Super+→` carries a
  snapped window on to the next screen, that the edge between the screens does
  not snap while the outer edge does, that unplugging a screen brings its
  windows onto the desktop on view and plugging it back sends them home as they
  were, and that show desktop clears both screens;
- snaps terminals side by side and in quarters and checks that dragging the
  line between them resizes the windows on both sides and keeps them snapped,
  that a window snapped next to them takes the rest of the width, and that an
  outer side still unsnaps only its window;
- opens X11 and Wayland windows nearly as big as the screen and checks they
  open maximized and restore to the usual size, while a smaller one opens as it
  is;
- opens an X11 window with a title bar of its own and checks that it moves
  where it asks to (also partly off the screen), follows the pointer exactly
  when dragged by that title bar, maximizes, restores with its own button (an
  activation right after a click in its top strip, but not without the click)
  and minimizes;
- has an app report progress and a count over D-Bus and checks its taskbar
  button fills (in blocks on Windows 95) and shows the number, and that both go
  when the app leaves the bus;
- has a fake pactl report a stream of a window's process and checks a speaker
  shows on its taskbar button, that clicks on it mute and unmute that stream and
  the wheel changes its volume, and that it goes when the stream is paused;
- gives an app a desktop entry with tasks and recent files and checks the
  right-click menu of its taskbar button lists the files newest first, leaves
  out one that is gone, opens them with the app, and runs the tasks;
- clicks the top edge and the top right corner of the screen over maximized and
  snapped windows, in the Windows 95 and 10 styles, and checks their buttons
  are hit;
- rests the pointer on a window's maximize button and checks the layouts show,
  that a part of one snaps the window there and no menu of the other windows
  follows, that `snap left 0.66` gives two thirds, and that
  `snap_layouts disable` turns them off;
- reads the text in a part of the screen with a fake tesseract and checks it
  lands in the clipboard and a notification, read in the session's language
  too, that a missing tesseract is reported, and that the Text button of the
  snipping toolbar starts it;
- opens the Program Manager of the Windows 3 theme and checks a program group
  opens as a window whose icon starts its app, that a minimized window lies as
  an icon on the desktop which a double click restores, and that
  `minimized_icons no` leaves it on the taskbar only;
- runs on a fake battery and checks the battery saver comes on at the level,
  tells so in a notification, goes off when plugged in, and that
  `battery_saver off` keeps it off;
- turns keep awake on and checks no screen saver starts, the screen stays on
  and a low battery does not turn the battery saver on, that all three come
  back when it is turned off, that the switch in the power flyout shows it and
  a click on it turns it off and on, and that a new session starts without it;
- puts widgets on the desktop and checks their cards take the cells the config
  gives them (and the upper right corner without one), that the icons make room,
  that a card dragged with a pointer lands on whole cells and keeps them while
  one dropped onto another card goes back, that clicking the disk space ring
  opens its flyout without taking the taskbar down, and that a new place or
  style in the config wins over an old drag;
- lets a nested tileWin sit idle and checks the screen saver covers the screen
  only after its time, that moving the mouse ends it and runs the lock command,
  and that `screensaver start` is not ended by the input right after it but by
  input a moment later;
- lets a nested tileWin sit idle until its screen goes off and checks the
  session gets locked with it, that a key turns the screen on again showing the
  lock screen, still locked, and that no second lock screen starts the next
  time;
- locks a nested tileWin with the real lock screen and checks the screen saver
  covers the lock screen after its time, that a key ends it and brings the
  lock screen back with the session still locked, and that the key ending it
  on the sign-in view is not typed into the password field;
- shows a card of every desktop style and switches through all themes, light
  and dark, checking that each card is drawn, that the dark scheme is darker
  where a theme has one, and that no two themes draw the chart alike;
- throws a window with a pointer of its own and checks it slides on past the
  throw, is still going a second later, and does neither when it was put down
  gently or when gravity mode is off.

- types into the start menu and the launcher and checks that settings pages,
  sums and files are found and open, with quotes and marks in a file name, and
  that a word with no app still runs as a command in the launcher;
- reads the bindings back, opens the list of keyboard shortcuts with its keys,
  and checks a switch of the mode tells how to get back three times and no more;
- walks quick settings and the network and power flyouts with the keyboard, and
  changes a slider and connects to a network with stand-ins for pactl and nmcli;
- goes through the welcome of a new user and checks it stays away afterwards, and
  from someone who was here before it came in;
- draws the cards of the Theme page and looks for the title bars of the themes;
- has a helper refuse on the DNS page and checks the banner, Try again and ×;
- compares the sharpness of the taskbar at 150% with and without
  `fractional-scale-v1`, and clicks its start button there;
- runs on a clock of its own and checks dark mode by the schedule, and that a
  change by hand lasts until the next start or end;
- presses the buttons of the Updates group with a stand-in for `tilewin-update`.

- starts the display manager with the real greeter and compositor, and a stand-in
  for PAM in a test program that is not installed: a wrong and a right password, a
  further question, a session that is gone, root, automatic login and what is
  remembered;

`tests/update.sh` in the unit suite runs `tilewin-update` itself with stand-ins
for GitHub, git and the password: the temporary folder goes in every case, there
is a restart only after a good update, and a wrong checksum installs nothing.

Debian has no pacman to check against, so two more tests of the unit suite play
it: `tests/install_debian.sh` runs `install.sh` with stand-ins for apt,
pkg-config and meson, as on Debian 13 (no wlroots 0.20: it is downloaded into
`subprojects/`, linked in statically and not installed) and on a Debian that has
it; `tests/pam_setup.sh` puts the PAM services into a `/etc/pam.d` of Arch and
one of Debian, and replaces one of Arch found on Debian. `tests/kernel_tools.sh`
does the Kernel page on Debian too: initramfs-tools and ukify, apt with the
Debian names of the build tools, and dpkg for the package of a driver.

To try the build of Debian 13 on Arch, download the subprojects and make meson
take them over the system's libraries:

```sh
meson subprojects download
meson setup build-bundled -Dwerror=false --default-library=static \
	--force-fallback-for=wlroots-0.20,wayland-server,wayland-client,wayland-cursor,wayland-protocols,libdrm,xkbcommon,pixman-1
```

Delete the folders of the subprojects again afterwards (the `.wrap` files stay):
while they are there, every build of the source tree takes wlroots from them.

## Making a release

Set the version in `meson.build`, commit and tag, then
`tools/make-release-archive.sh <dir>` makes the ready-made archive and
`SHA256SUMS` for the release, which `tilewin-update release` looks for. An archive
runs where the libraries it was built against are there; elsewhere the update
builds the source of the tag.

The AUR package is in `packaging/aur`. After a release, set `pkgver` there and:

```sh
git clone ssh://aur@aur.archlinux.org/tilewin.git aur-tilewin
cp packaging/aur/PKGBUILD packaging/aur/tilewin.install aur-tilewin/
cd aur-tilewin
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD tilewin.install .SRCINFO
git commit -m "Version x.y"
git push
```

It builds with `-Dself-update=false`, so `tilewin-update` and the About page
leave the updating to pacman rather than write over its files.

It needs `grim`, `wl-copy`, `python3` and `dbus-run-session`, and skips itself
where it cannot run.
