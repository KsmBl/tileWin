# The display manager

`tilewin-display-manager` is the login screen of tileWin: in the look of your theme, quick to start, with a setting for most things. It is installed with tileWin and stays off until you turn it on.

## Turning it on

The **Login screen** page of the settings (under Personalize, or the button on the Theme page) has the switch, or:

```sh
sudo /usr/local/libexec/tilewin-dm-apply enable    # takes over at the next start of the computer
sudo /usr/local/libexec/tilewin-dm-apply disable   # puts back the one used before
```

That path is for `install.sh`. In the Arch package the helper is `/usr/lib/tilewin/tilewin-dm-apply`.

### Try it first

Once tileWin is installed, it can run beside the display manager you use, on a terminal of its own, without changing anything for the next start:

```sh
sudo systemd-run --unit=tilewin-dm-trial --collect tilewin-display-manager --vt 9 --once
```

Log in, or press Ctrl+Alt+F<n> to get back to where you were. `--once` ends it after the first session. If the screen stays dark, Ctrl+Alt+F3 gets you to a text console, and `journalctl -u tilewin-dm-trial` says why. It runs as a unit because logind gives no session to a program started inside one of yours.

The page has a picture of the login screen that follows every change. **Try it on the whole screen**, or `tilewin-dm-greeter --preview`, shows the real thing in your running session; it checks nothing, and Esc ends it. Changes only count after **Save**, in a bar that shows when there is something to save.

## How it works

- **`tilewin-display-manager`** runs as root and is small: no toolkit, no drawing. Each login is a child process holding PAM from the first prompt to the end of the session, so a stuck PAM module cannot take the daemon with it.
- **The greeter** (`tilewin-dm-greeter`) runs as the user `tilewin-dm`, on the tileWin compositor without any of the desktop, in a logind session of class greeter. It can only draw and talk to the daemon over a socket only that user may connect to.
- **The daemon trusts nothing the greeter says:** a user name goes to PAM, a session is looked up among the session files and never taken as a command, and a password is only passed on.
- **The look follows you:** with `theme follow` the login screen wears the theme and the color scheme of whoever logged in last, read when they log in and out. A theme of your own in `~/.config/tileWin/themes` cannot be read by the greeter: then it wears Windows 10, unless you copy the theme to `/usr/share/tileWin/themes`.
- **The first frame comes before the wallpaper,** which is drawn right after it, so a large picture does not hold up the screen.
- **Sessions** are the files in `/usr/share/wayland-sessions` and `/usr/local/share/wayland-sessions`. X11 sessions are not offered.
- **What a session prints** goes to `~/.cache/tilewin-dm-session.log`.

The PAM services are `tilewin-dm` (logging in), `tilewin-dm-autologin` and `tilewin-dm-greeter`, in `/etc/pam.d`.

## Keys

| Keys | Action |
|---|---|
| Enter | Log in; from the name on to the password |
| Tab, Shift+Tab | The name, the password, the session, the layout, the button |
| Up, Down | The user before or after |
| Left, Right | The session or the layout, when it has the focus |
| Ctrl+U | Empty the password |
| Esc | Empty the password; stop a further question; end a preview |

## Settings

`/etc/tileWin/display-manager.conf` holds `key value` lines, `#` for comments, and quotes around a value with spaces at its ends. The Login screen page writes it, which drops your comments. `tilewin-display-manager --check-config <file>` says what is wrong with a file. A line that makes no sense is left out and the default counts, so a mistake in here cannot keep anyone from logging in.

`env KEY=value` lines, as many as you like, set the environment of the sessions.

### Logging in

| Setting | Default | What it does |
|---|---|---|
| `default_user` | empty | User picked at first. When no one logged in before, or remembering is off |
| `remember_user` | `yes` | Remember the last user (yes, no) |
| `default_session` | `tilewin` | Session picked at first. The name of its file in wayland-sessions, without .desktop |
| `remember_session` | `yes` | Remember the last session (yes, no) |
| `autologin_user` | empty | Log this user in without a password. Anyone at this computer is then that user. Empty for nobody. |
| `autologin_session` | empty | Session of the automatic login. Empty for the session picked at first |
| `autologin_once` | `yes` | Automatic login only once per start of the computer. Turned off, logging out logs in again right away (yes, no) |
| `allow_root` | `no` | Allow logging in as root (yes, no) |
| `fail_delay` | `2` | Seconds to wait after a wrong password (0 to 60) |
| `clear_password_after` | `60` | Forget a half typed password after. Seconds without a key, 0 for never (0 to 3600) |
| `session_path` | `/usr/local/sbin:/usr/local/bin:/usr/bin` | PATH of a session |
| `vt` | `1` | Virtual terminal. Where the login screen and the sessions run (1 to 63) |

### Look

| Setting | Default | What it does |
|---|---|---|
| `theme` | `follow` | Theme. The name of a tileWin theme, or follow for the theme of whoever logged in last |
| `color_scheme` | `follow` | Color scheme. Follow takes it from whoever logged in last (follow, light, dark) |
| `wallpaper` | `theme` | Wallpaper. theme, a picture every user can read, or a color like #204070 |
| `dim` | `30` | Darken the wallpaper. In percent (0 to 90) |
| `accent` | `theme` | Accent color. theme or a color like #0078d4 |
| `font` | `theme` | Font. theme or a font like Noto Sans 11 |
| `card_style` | `window` | Login card. window has the title bar of the theme (window, plain) |
| `card_position` | `center` | Place of the login card (center, left, right) |
| `card_width` | `420` | Width of the login card. In pixels (280 to 900) |
| `card_title` | `Log in to %h` | Title of the login card. %h is the name of the computer |
| `card_on_all_screens` | `yes` | Login card on every screen. Turned off, only on the first (yes, no) |

### Shown

| Setting | Default | What it does |
|---|---|---|
| `clock` | `top` | Clock (top, bottom-left, bottom-right, none) |
| `clock_format` | `%H:%M` | Format of the clock. As strftime |
| `date_format` | `%A, %e %B` | Format of the date. As strftime; empty for no date |
| `greeting` | empty | Message. A line under the title, e.g. who to call for help |
| `show_users` | `yes` | List the users. Turned off, the name is typed (yes, no) |
| `min_uid` | `1000` | Lowest user ID listed (0 to 65535) |
| `max_uid` | `60000` | Highest user ID listed (0 to 65535) |
| `hide_users` | empty | Users not listed. Names with spaces between them |
| `show_full_names` | `yes` | Show full names (yes, no) |
| `show_avatars` | `yes` | Show pictures of users. /var/lib/AccountsService/icons/<user>, or .face if it can be read (yes, no) |
| `show_sessions` | `yes` | Show the session to pick (yes, no) |
| `show_caps_lock` | `yes` | Say that Caps Lock is on (yes, no) |
| `show_layout` | `yes` | Show the keyboard layout to pick. With more than one layout (yes, no) |
| `password_style` | `dots` | Password shown as (dots, stars, hidden) |
| `password_reveal` | `yes` | A button to show the password (yes, no) |

### Power

| Setting | Default | What it does |
|---|---|---|
| `show_power` | `yes` | Show the power buttons (yes, no) |
| `allow_poweroff` | `yes` | Allow shutting down (yes, no) |
| `allow_reboot` | `yes` | Allow restarting (yes, no) |
| `allow_suspend` | `yes` | Allow sleeping (yes, no) |
| `allow_hibernate` | `no` | Allow hibernating (yes, no) |
| `screen_off_after` | `300` | Turn the screens off after. Seconds without input, 0 for never (0 to 86400) |

### Input

| Setting | Default | What it does |
|---|---|---|
| `keyboard_layout` | empty | Keyboard layouts. As xkb, like de,us. Empty for the default. |
| `keyboard_variant` | empty | Keyboard variants. As xkb, like nodeadkeys, |
| `keyboard_options` | empty | Keyboard options. As xkb, like ctrl:nocaps |
| `numlock` | `yes` | Num Lock on (yes, no) |
| `repeat_delay` | `400` | Delay before a key repeats. In milliseconds (100 to 2000) |
| `repeat_rate` | `30` | Key repeats per second (1 to 100) |
| `tap_to_click` | `yes` | Tap the touchpad to click (yes, no) |
| `cursor_theme` | empty | Pointer theme. Empty for the default |
| `cursor_size` | `24` | Pointer size (8 to 128) |
| `hide_cursor` | `no` | Hide the pointer until it moves (yes, no) |
| `screen_scale` | `auto` | Scale of the screens. auto, or like 1.5 |
