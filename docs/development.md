# Building and testing

How to install is in the [README](../README.md#installation).

## Tests

```sh
meson test -C build-release --suite unit   # the config parser, the config editor, the disk list, the widget list, the screen savers, moving taskbar widgets and the settings coverage
meson test -C build-release --suite gui    # drives a nested tileWin and looks at what it does
```

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
- shows a card of every desktop style and switches through all themes, light
  and dark, checking that each card is drawn, that the dark scheme is darker
  where a theme has one, and that no two themes draw the chart alike;
- throws a window with a pointer of its own and checks it slides on past the
  throw, is still going a second later, and does neither when it was put down
  gently or when gravity mode is off.

It needs `grim`, `wl-copy`, `python3` and `dbus-run-session`, and skips itself
where it cannot run.
