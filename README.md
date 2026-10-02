# tileWin

tileWin is a Wayland compositor with two modes you can switch between at any time:

- **Tile mode:** behaves like [sway](https://swaywm.org) and is configured with the same syntax. Your sway config works here.
- **Window mode:** behaves like Windows:
  - floating windows with themed title bars and minimize/maximize/close buttons
  - drag-to-edge snapping, Alt+Tab and Windows keyboard shortcuts

It comes with **tilewin-panel**, a lightweight taskbar with widgets, custom script widgets, right-click menus and a start menu. There are seven built-in themes: **Windows 95, XP, 7, 8, 10, 11** and **sway**. They style window decorations, taskbar, start menu, menus and wallpaper, and switch live from the command line.

tileWin is built on sway 1.12 and wlroots 0.20. The compositor and taskbar are written in C without GTK/Qt and use no CPU when idle; only the optional settings app uses GTK4 and runs only while it is open.

## Screenshots

Window mode with the Windows 10 theme: floating windows with title bars, the taskbar with tray and clock, desktop icons, and the settings app.

![Window mode](docs/screenshots/window-mode.png)

| Start menu | Task view (Super+Tab) |
|---|---|
| ![Start menu](docs/screenshots/start-menu.png) | ![Task view](docs/screenshots/task-view.png) |

Tile mode is sway: the same layouts, the same config, with the theme's tile colors and a status bar.

![Tile mode](docs/screenshots/tile-mode.png)

Every theme brings its own title bars, taskbar, start menu and wallpaper:

| Windows 1 | Windows 3 |
|---|---|
| ![Windows 1](docs/screenshots/theme-win1.png) | ![Windows 3](docs/screenshots/theme-win3.png) |
| **Windows 95** | **Windows 2000** |
| ![Windows 95](docs/screenshots/theme-win95.png) | ![Windows 2000](docs/screenshots/theme-win2000.png) |
| **Windows XP** | **Windows XP (dark)** |
| ![Windows XP](docs/screenshots/theme-winxp.png) | ![Windows XP (dark)](docs/screenshots/theme-winxp-dark.png) |
| **Windows Vista** | **Windows 7** |
| ![Windows Vista](docs/screenshots/theme-vista.png) | ![Windows 7](docs/screenshots/theme-win7.png) |
| **Windows 8**, its start screen of colored tiles over the whole screen | **Windows 11** |
| ![Windows 8 start screen](docs/screenshots/theme-win8.png) | ![Windows 11](docs/screenshots/theme-win11.png) |

Windows 10 is the window mode screenshot at the top.

Windows can be closed with an explosion (`animation close explode`), one of the styles on the Animations page:

![A window closing with the explosion animation](docs/screenshots/explosion.png)

## Features

- **Two live-switchable modes.** `Super+Shift+W` or `tilewinmsg mode toggle`.
  - Windows keep running.
  - Tiled windows become floating windows and get their previous window-mode geometry back when you return.
- **Window mode:**
  - Themed server-side decorations with hover and pressed states.
  - Double-click the title bar to maximize; double-click the icon to close. `double_click_time` sets how fast the two clicks have to follow each other.
  - Resize from the borders (with invisible grab margins on thin-border themes).
  - Drag to a screen edge to snap left/right, drag to a corner for quarters, drag to the top edge to maximize. A preview shows first.
  - **Snap Layouts**, as on Windows 11: rest the pointer on a window's maximize button and pick a part of a layout (halves, two thirds and one third either way, quarters, a half and two quarters); the window snaps there; a click on one of the window's buttons closes the layouts at once. The layouts can be your own: the Window behavior page of the settings lists them with a picture each, to reorder, remove or change, and an editor makes new ones: pick the kind (two side by side, quarters, one beside two quarters on either side) and drag the lines between the parts where you want them; they stop at quarters, thirds and the middle, and each part shows its size. `snap <slot> <fraction> [<fraction>]` snaps into a part of such a layout (`snap left 0.66`); `snap_layouts disable` (or the switch on the Windows page) turns it off.
  - The buttons of a window against the top of the screen (maximized, or snapped to a half or a quarter there) reach up to the edge, and the last one into the corner beside it: push the pointer to the top or into the top right corner and click, without aiming at the button.
  - Snapped windows side by side resize together, as on Windows: drag the line between a left and a right half (or between two quarters of a column) and the windows on both sides follow, staying snapped. The line is easy to grab (6 pixels on each side), a window snapped next to them later takes the space that is left, and an outer side still unsnaps the window when dragged.
  - `Super+Arrow` snapping.
  - A new window about as big as the screen (90% of it both ways) opens maximized instead of a few pixels short of it; restoring it gives a window of the usual size. Dialogs and windows of a fixed size open as they are.
  - X11 apps that draw their own title bar (Bambu Studio and other slicers built on wxWidgets) can be dragged by it and their minimize, maximize and restore buttons work. They move themselves, ask for maximize over X11, and "restore" by asking to be activated, which a maximized window of that kind gets after a click in its top strip.
  - Moved and resized windows stick to the edges of other windows and of the screen. Hold Shift (`window_group_modifier`) while dragging to move the windows touching it along. Windows snapped to an edge or a corner belong to such a group as well: they come along in the size they have, instead of going back to the size they had before they were snapped.
  - Double-click the left or right side of a window to stretch it to the next window or the screen edge, the top or bottom side to do the same with its height. Double-click again for the old size.
  - Keep a window over the others ("Always on top" in its menu, `always_on_top`), and make it see-through from its menu ("Set transparency").
  - Minimize to the taskbar; show desktop; Alt+Tab switcher, as a grid of icons or as a 3D stack of the windows themselves like Flip 3D (`alttab { style flip3d }`, used by the Windows 7 theme).
  - Closing a window gives the focus to the window used before it that is open on the same desktop and not minimized, so a minimized window is not opened again.
  - Task view (Win+Tab) like on Windows 10: thumbnails of the windows, a strip with all desktops (workspaces) to switch to, drag windows onto a desktop or "New desktop", close windows and desktops. Drag a desktop along the strip to put it in another place, and click its name to rename it (Return keeps the name, Escape the old one). Hovering a desktop shows its windows; arrow keys and Enter pick a window. Desktops created there stay when they are empty.
  - Arrange windows: cascade, stacked, side by side, or the optimal grid, whose windows come closest to 1.3:1 (four windows make 2x2).
  - **Gravity mode** (off by default): let go of a window while it is still moving and it carries on sliding, bouncing off the edges of the screen until it comes to rest. There is no pull downwards — a window behaves like a flat thing pushed across a table. The **drag** says how quickly it stops and the **bounce** how much speed an edge gives back; both are on the Window behavior page. A window put down without moving stays where it is put.
- **Several screens** (window mode), handled the way Windows handles them:
  - A window taken to another screen (`Super+Shift+←/→` or dragging it over) stays maximized or snapped there, keeps the focus, and the size it goes back to comes along to the same place on the new screen. A window too big for the new screen is made to fit.
  - `Super+←` on a window snapped to the left half carries it on to the right half of the screen to the left, and `Super+→` the other way round.
  - The edge between two screens does not snap a window carried quickly over it; hold the pointer at it for a moment and it snaps there too (the half of that screen, or a quarter when you slide on to a corner). The outer corners snap along either edge that meets there, and the window snaps on the screen the pointer is on, even when most of it is still on the other one.
  - Unplugging a screen puts its windows on the desktop on view on another screen, still maximized or snapped. Plugging it back in returns them to it, where they were and as they were, unless you moved them in the meantime. Screens are recognized by make, model and serial, so another port does not matter.
  - Show desktop (`Super+D`) clears every screen at once.
  - A program started with a shortcut, from the Start menu or the Run dialog opens on the screen the pointer is on and gets the focus, also when the keyboard focus was on another screen.
  - The Super key opens the Start menu on the main display, as on Windows; the Start button of another screen's taskbar opens it there.
  - A **main display** (`main_output`, or "Make this my main display" on the Screen page; by default the screen at the top left) gets the desktop icons, and the taskbar there can show the windows of every screen (`outputs main`).
  - Every screen can have a wallpaper of its own (`output_wallpaper`, or the Screen chooser on the Wallpaper page); the lock screen shows it too.
  - The taskbar can be on every screen, only on the main display, or on the screens you pick ("Show the taskbar on" on the Taskbar page; `outputs * | main | <names>` at the top of `taskbar.conf`). With `main` it moves along when another screen becomes the main display.
  - The Screen page arranges the screens by dragging them; a dragged screen snaps to the edges of the others, side by side or above and below, and lines up with their tops, bottoms, sides or middles. **Align with a grid…** covers every screen with one grid drawn across the whole layout, with colored bands and diagonals: drag the grid of a screen (or use the arrow keys, Shift for 10 pixels) until its lines run on straight into the next screen across the bezel, then Done (Enter) or Cancel (Escape). **Align the screens** puts them in one row or column without gaps, lined up at their tops, middles or bottoms (left edges, middles or right edges when stacked). **Identify** shows each screen's number on it. Turning a screen, or changing its scale or resolution, shows at once in the arrangement, and the screens right of it and below it move along so none overlaps it.
  - Selecting icons with a rectangle works on every screen, and the rectangle is drawn on the screen it is dragged on. Screen savers that use the picture of the desktop show it the right way round on turned screens.
- **Tile mode:** everything sway does.
- **Taskbar:**
  - Separate layouts for window mode and tile mode.
  - Widgets:
    - Apps and windows: start button, taskbar, quick launch, workspaces, window title.
    - Status: system tray (StatusNotifierItem, with the menus the icons publish over DBusMenu), clock with the clock and calendar flyout, the notification bell, volume, network, battery, CPU, memory, disk activity, disk space, GPU, network usage, power draw, brightness, keyboard layout, git.
    - Controls: mode switch, show desktop, search box.
    - Layout and scripts: separator, spacer, **custom script widgets**.
  - Every widget can run commands on click or scroll and have its own right-click menu.
  - **Progress and counts on the buttons:** apps that report them (Firefox and Chromium downloads, file managers copying, mail and chat apps with unread counts; the `com.canonical.Unity.LauncherEntry` signal) fill their taskbar button green as it goes, as on Windows 7, and show the number on the corner of their icon. The colors come from the theme (`taskbar.progress`, `taskbar.badge_bg`, `taskbar.badge_fg`), and the classic look draws navy blocks.
  - **Sound per app:** the button of a window playing sound shows a speaker (struck through while muted). A click on it mutes or unmutes that app, the wheel on it changes that app's volume. Sound from a child process (a browser's) counts for the window of its parent.
  - **Jump lists:** the right-click menu of a taskbar button starts with the files the app opened last (from `recently-used.xbel`, newest first, only those still there), each opened with that app, and the tasks its desktop entry offers (such as "New private window"), as on Windows 7.
- **Start menu** in the style of the active theme:
  - Windows 95: cascading menu with banner.
  - XP / 7: two-column menu.
  - Windows 10: list.
  - Windows 11: centered grid.
  - Windows 8: a full-screen start screen with colored tiles, all apps in columns and search.
  - All include app search, pinned apps, places and a power menu.
- **Run dialog**, tooltips, calendar flyout.
- **Desktop** like on Windows: icons of the files in `~/Desktop` (double-click opens them) and a right-click menu to create folders, text documents and shortcuts to programs, files or web addresses, rename or delete (to the trash) items, open a terminal there, and change the wallpaper or theme. Drag icons to any cell of the grid; where they sit is kept in `~/.local/state/tileWin/desktop-icons`, and "Sort icons" puts them back in order. Dragging one of several selected icons moves them all and keeps the places they have next to each other. Drag on the empty desktop to draw a selection rectangle, and Ctrl+click to add or remove single icons. Clicking the desktop gives it the keyboard: **Delete** moves the selected icons to the trash (where there is none, it asks whether to delete it for good), **F2** renames the one selected icon, **F5** rereads the folder and **Escape** drops the selection. Shortcuts of other desktops work too, including KDE's `Type=Link` files such as the trash can. The Desktop page of the settings turns the icons off and sizes the grid: the picture (`desktop_icon_size`, default 48), the cell an icon sits in (`desktop_icon_width` and `desktop_icon_height`, default 100) and the room around the whole grid (`desktop_margin`, default 10), all in `taskbar.conf`.
- **Desktop widgets.** Every taskbar widget can sit on the desktop as well: the clock, the meters of the CPU, memory, GPU, network, disk space and power draw, the disk lamp, volume, battery, git, script widgets and the rest. They sit in the grid of the desktop icons and take whole cells of it, and the icons flow around them. Each has styles of its own: the taskbar look and a tile for every widget, a chart of the last measurements, a gauge with a needle, a ring and a bar for the meters, and a digital, an analog and a binary clock. Every style is drawn in the look of the theme (grey and raised for Windows 95, soft blue for XP, glass with a chrome gauge and a white clock for 7, colored tiles for 8, flat for 10, rounded for 11) and in its light or dark colors. The `widget <name>` block of `taskbar.conf` sets a widget up for both places, and what its entry in `desktop_widgets` says (a style, a format) is only for the desktop. Clicks, scrolling, tooltips and flyouts work there as on the taskbar. Drag a card to move it from cell to cell; a card dropped onto another one goes back. Where it was put is kept in `~/.local/state/tileWin/desktop-widget-cells` until the config gives it another place. `output` puts a widget on the main display (default), on every screen or on one by name. The Desktop page of the settings adds and removes them and picks the style, cells, screen and card of each.
- **Named desktops.** A click on the name of a desktop in the task view, or `tilewinmsg desktop rename Work`, gives a virtual desktop a name; the task view and the workspaces widget then show it instead of *Desktop 2*. The name is kept beside the number (`2:Work`), so the desktop keeps its place in the order, `Win+Ctrl+←/→` still walks through them in order, and moving a desktop takes its name along. `desktop rename` with nothing after it gives the number back. A workspace named by hand in a tile-mode config keeps the name it was given.
- **Flyouts** like on Windows: click the network icon for Wi-Fi networks (connect with password, disconnect, Wi-Fi on/off; each network shows its band, 2.4, 5 or 6 GHz, and the most this PC and that router reach together, the older Wi-Fi standard, the fewer antenna streams and the narrower channel of the two, which an expanded network spells out, e.g. Wi-Fi 5 · 2×2 · 160 MHz; for the connection in use the frequency, channel and width and the rates the link runs at right now), the volume icon for the volume, output device and per-app volumes, and the battery or brightness icon for charge, remaining time, brightness and power mode.
- **Shut down dialog** in the look of the theme, opened from the start menu or with Ctrl+Alt+Delete: Windows 95 asks "Shut Down Windows" with radio buttons over the dithered screen, Windows XP shows "Turn off computer" with Stand By, Hibernate, Hybrid Sleep, Turn Off and Restart while the screen fades to gray (and "Log Off Windows" from Log Off), Windows 7, 10 and 11 show the Ctrl+Alt+Delete screen over the blurred desktop, and the Sway theme big buttons like wlogout. The commands are the `power` entries of the `startmenu` block. Sleep (stand by in RAM), hibernate (to disk) and hybrid sleep (both, so the work survives an empty battery) are left out of the dialog and the power menu where logind says the computer cannot do them, e.g. hibernate without a swap big enough.
- **Text fields** (start menu search, launcher, Run dialog, desktop dialogs, Wi-Fi password) edit like on Windows: ←/→ move the cursor (Ctrl: by word), Home/End, Shift with any of them selects, Ctrl+A selects everything, typing replaces the selection and Backspace/Delete remove it (Ctrl: a word). Themes can color the selection with `text { selection_bg; selection_fg }`.
- **Lock screen** (Win+L): `tilewin-lock` shows the wallpaper with the time and date like Windows; any key shows the sign-in view with your picture (`~/.face`), name and password. The password is checked with PAM (`/etc/pam.d/tilewin-lock`, installed by install.sh). If the lock screen crashes, the session stays locked. `tilewin-lock -f` returns once the session is locked (for scripts and `lock_command`).
- **Battery saver**, like the one of Windows: on battery at or below 20% (`battery_saver`, "Battery saver" on the Screen page) the animations, the trail of the pointer and the charts the taskbar widgets keep in the background stop until the computer is plugged in, and the taskbar says so in a notification.
- **Screen saver** (`tilewin-screensaver`), started after a time without input like on Windows, and ended by the mouse or a key; with "On resume, display the lock screen" coming back asks for the password. The screen savers of Windows 7 are there: **Blank**, **Bubbles** (soap bubbles with rainbow rims drifting over the desktop, which stays visible, and bouncing off each other), **Mystify** (two shapes of lines leaving echoes of color), **Ribbons** (glowing ribbons that twist in space), **3D Text** (your words or the time in solid chrome letters turning slowly) and **Photos** (a slideshow of a folder, zooming and fading from picture to picture). From older Windows come **Starfield**, **3D Pipes** (as Windows 95 to XP drew them: shiny pipes of one color each growing through a box in perspective, passing properly in front of and behind each other, turning in smooth elbows and now and then a ball joint, and very rarely in the teapot), **3D Maze** (a walk through a brick maze over a carpet and under a wooden ceiling, with rats, grey stones that turn the world upside down, and a smiley at the way out) and **Flying Windows**. And four of tileWin's own: **Aurora**, northern lights over dark hills whose curtains sway faster, burn brighter and turn from green to purple and red the harder the computer works, so a glance at the screen says whether that build is still running; **Word Clock**, the time spelled out in a grid of letters ("it is twenty past ten"), with the minutes in between as dots and the grid wandering a little so nothing burns in; and **Tiling**, windows opening, closing and making room for each other the way tile mode does it, with a shell that keeps typing, an editor, a monitor, an equalizer and a picture; and **Diggers**, little miners in yellow helmets who drop onto the windows that are really open on the screen and dig into them with their picks, looking for what is inside: they walk on the title bars, fall off the edges and dig like a real mine, level drifts one above the other joined by straight shafts, and now and then hold up a find (a page, a gem, a folder, a letter of the window's title) while a counter at the top keeps score. Each has a spot in a window he wants to get to and heads for it: along the ground, level through the window, straight down a shaft with the pick, or up by stairs. A miner done digging deep in a window hammers a wooden staircase together plank by plank to climb back up to its top, in a narrow stairwell cut straight up through the window with the flights turning back and forth in it, and one on the floor under a window builds his stairs all the way up to it and through it. Where a wall rises beside them with open air above, they throw a grappling hook over its edge and climb the rope hand over hand instead. Nobody builds a staircase where one close by already climbs that way: he walks to its foot and takes it. Stairs nobody has walked on for a while fall apart, the planks tumbling down. They walk up and down stairs and slopes, take the next flight of a switchback on the way up, and a miner who falls further than his own height opens a parachute and floats down, as do the ones dropping in from the top. One of them, in a red helmet, always carries dynamite: he sets a bundle against a window, runs while the fuse sparks, and the blast tears a sooty crater into it, throws the others off their feet and sometimes turns up a find or two. The taskbar is solid ground: they walk along it and build their stairs up from it, but no pick, staircase or dynamite gets through it, and with the taskbar at the top they drop in below it. When the windows fill the whole screen there is nowhere to walk, so they climb in at the edges of the screen and dig their way in from there. When a third of the windows is dug out, or after two minutes, a huge drilling machine pushes in from one side of the screen with its teeth whirring, grinds the windows away, knocks the staircases down and throws the miners off the screen; then helicopters fly the windows back in, slab by slab from the bottom up, and lower each into its place; the slabs are the real windows, cut from a picture of the screen the saver takes when it starts (with wlr-screencopy, after the windows lost the focus to it, so they look just as they do under it), and a window opened later comes as a drawn stand-in, and the miners parachute in again one at a time at spots all over the screen. **Matrix** is the green code of the film raining down the screen, the katakana mirrored as they were there, each column led by a white character and some characters of the trails changing as they fade. **Doomsday** ends the desktop, in one of three ways picked in its settings, each with settings of its own (`doomsday_kind` and `doomsday_<kind>_<setting>`; the names they had as savers of their own, `name hellfire` or `hellfire_flash`, still work). In **Hellfire** the windows burn, after the nuclear flash of Terminator 2: a white flash from a blast beyond the top of the screen, then the heat browns everything like paper held to a flame, the windows that are really open catch fire from a few spots each and burn through with thin white-hot edges and red-hot char behind them, the writing still showing in the charred sheet, while flames lick up and sparks and smoke rise; the shock wave follows with a hot rim and dark dust, shakes the screen and tears the charred windows away as ash. Pieces of the burning title bars and borders break off, the real ones cut from the screen, browned and charring, tumble down glowing and trailing sparks, and pile up in a low heap of shards along the bottom of the screen; the smouldering scraps keep shedding bits into it. At the end only torn scraps of the title bars, borders and the taskbar are left on the burnt ground, smouldering at their edges for as long as the saver runs and slowly turning brown and red under falling ash. It burns a picture of the screen taken when it starts, and brings windows of its own in the preview. In **Thunderstorm** the desktop is under a storm: its light goes grey and cold under heavy clouds rolling along the top, and the rain comes down in three depths, a far veil behind the windows, rain that drums on the tops of the windows and the taskbar and splashes there, and heavy streaks in front of it all. Water gathers in drops on the glass of the windows that are really open, grows, runs down in wandering wet trails and drips off their lower edges, runoff pours off their corners, and the taskbar is wet and shines. Lightning forks down out of the clouds, into the distance, into the taskbar or into a window, whose light goes out for a moment and which keeps a smoking scorch mark; each flash shows the desktop in a cold light, the thunder follows, the nearer the sooner and the harder it shakes the screen, and gusts slant the rain and drive it in sheets while now and then a big drop runs down the screen itself. In **Blizzard** it is snowed in. The light turns cold and flat, and snow comes in on a wind that rises to a blizzard and falls off again: a fine haze of flakes far off behind the windows, flakes that drive past and settle, big soft ones out of focus close in front, and now and then a snow crystal drifting by close enough to see its six arms. The snow piles up on the tops of the windows that are really open and on the taskbar, soft and lumpy, heaped no steeper than snow holds and never where a window in front hides the edge or higher than the room under it; what is pushed over an end bulges out as a cornice until a lump of it breaks off, falls, and bursts on whatever is below, adding to the heap there. Gusts blow powder off the crests and turn the air white, streams of blowing snow run along the bottom, and a drift of dunes rises there in front of the taskbar until it is buried. Icicles grow down from the lower edges of the windows and from the snow over the title bars; a strong gust breaks one off now and then, and it shatters below in glittering bits. Frost creeps over the glass of the windows from their edges and corners: a haze first, which blurs what is behind it, then ice ferns branching into it, feathered like real ones, which on a light window show steel blue, leaving a clearer hollow in the middle. Later ferns grow in from the corners of the screen itself, and a flake that lands on it shows its crystal for a few seconds before melting into a drop. And it does not stop: the snow comes harder and harder, the heaps grow into domes as high as there is room, the drift rises half way up the screen over the lower parts of the windows, and the wind plasters snow over the windows and the taskbar from below and from its side, until after some minutes everything is buried. It is a doomsday: the light is cold and dark under a darker sky and fails further as it goes on, squalls drive walls of snow across and rattle the screen, thundersnow flashes in the murk and the thunder shakes it, and now and then something hits the frozen glass of the screen, which cracks in a web of jagged lines. The frost and the plastered snow are worked out on a thread of its own while the snow already falls, so it starts at once; what changes slowly (the desktop frosting over, the heaps, the drift, the icicles, the light) is drawn a part a frame into a picture of its own, so a frame only adds what moves. Its settings are how hard it snows (heavy, light, or whiteout), whether frost grows, and whether it snows everything in. In **Decay** the desktop is eaten by time: days flicker past faster and faster while the years count up in a corner, and each thing ages as what it is made of. The desktop behind is a plastered wall: it yellows unevenly, water stains spread with brown tide lines ring in ring, dirt washed off the windows runs down the wall below them in streaks and settles low and in the corners, cracks run through the plaster, mould creeps up from below, and the plaster flakes off in patches that show the brickwork under it, the broken edges pale. Cobwebs are spun into the corners of the screen and of the windows' glass, and dust gathers on the tops of the windows and the taskbar. The insides of the windows are glass: a film of grime, dirt running down in streaks, rings where water dried, a milky haze; then something hits it and it cracks in a web from that point, straight lines out and rings between, and later the shards fall out of the frame, nearest the break first. The frames (title bars and borders) are wood or stone: wood fades, its paint peels to the grain, it greys, rots in patches and splits along the grain into long planks; stone loses its paint, pits, grows lichen, cracks jaggedly and breaks into blocks. The taskbar is stone. Whatever comes off falls as a rigid body, a real polygon with its mass and spin: it collides with the other pieces, with the windows still standing and with the rubble, bounces and slides as its material does (glass skitters, wood knocks and bounces, stone thuds and stays), lies on what is below it, and breaks when it lands hard: glass shatters into shards from where it hit in a spray of glittering splinters, wood splits along its grain and throws fibres, stone breaks into chunks in a puff of dust, and heavy blocks jolt the screen. Pieces show their thickness as they turn (glass green at the edge), and dust breaks off in soft clouds. Pieces fall at different depths, so they lie in front of each other like real debris, pieces that land on a window's ledge lie there until that window goes, the falling ones cast shadows, glass flashes as it turns and the shards in the heap glint. Where a piece was, what lay behind it shows, ageing too. At the end the rubble crumbles and dust drifts over it in dunes, until nothing is left but dust, with motes of it drifting through shafts of light that move across the room as the days pass. The physics runs in small steps whatever the speed, so it stays right at the fastest one too. Its settings are how long it takes (six, twelve or two and a half minutes) and whether the years are counted. In **Jungle** the desktop is overgrown, growing smoothly a little every frame: the light turns green and close, moss creeps up the windows from below and in from their corners, over the taskbar and up the desktop; vines sprout from the bottom and from the windows, climb their edges, creep along the title bars, branch, curl tendrils and put out leaves of four kinds, and more hang down from the top; flowers open on them, stay a while, wilt, drop their petals and open again, now and then in another colour; giant fronds (a monstera, a palm, a banana leaf) unfurl in from the edges and sway in the breeze, butterflies flit about and settle on the flowers, leaves and petals drift down, and days pass, with shafts of light through the canopy by day and fireflies at night, until all is jungle. Its settings are how long it takes, the butterflies and fireflies, and the nights. **Gravity** is one for privacy: a moment after it starts, the windows come loose one after another from the top of the stack, each hanging for a breath from one corner of its title bar and swinging, then dropping and tumbling behind the taskbar and off the screen, until only the empty desktop is left with nothing on it to read. The windows are cut from the picture of the screen, what another window hid of one filled with its own colour so none carries a piece of another away; where they stood the wallpaper shows, the one tileWin reports for the screen (`tw_wallpaper` of `get_outputs`), or when another program draws its own, the colour of the desktop around. Its settings are how fast they fall and whether the taskbar goes last, so its buttons show no window titles either. **Microslop Ad** is a two-minute parody commercial for Copilot− PCs, only $16.05 per 6 days, in twelve scenes: the Microslop logo melting as it drips ("Where do you want to be sent today?"); Brenda, 34, a real customer* ("I used to think for myself. Now Copilot− does it for me. Slightly worse.", *AI-generated); the product ("All the AI you didn't ask for. Now with less."); a laptop whose Start menu is all ads next to what's new; Recall− playing back your day ("Never forget. Never be allowed to."); Slippy the paperclip ("It looks like you're trying to leave. Would you like help staying?" Yes / Yes, but later); your PC against a Copilot− PC ("Comparison performed by Copilot−. Copilot− won."); a smiling blue screen with the stop code CUSTOMER_TOO_SATISFIED and a progress that runs backwards; the price rolling up past a crossed-out $16.04, with Copilot−− PCs at $32.10 per 12 days (best value, same value); your privacy, which is worth a lot, with a telemetry switch that springs back on; an Upgrade button beside a Not now that runs from the pointer; and the end card. The Skip ad button counts down and never lets you, and twice a round updates are ready, with a choice of Restart now or Restart now. **Random** picks another one each time. For any of them, "Show frames per second and CPU use" (above the choice of the saver on the Screen saver page, `stats yes` in the `screensaver` block) puts a small plate in the top left corner of each screen with how many frames a second it draws, how long a frame takes, how busy the processor is, and how much of that is the screen saver; the preview in the settings shows it too. An app that keeps the screen on, like a video, keeps the screen saver away too. It draws at most 30 times a second and only while it is shown.
- **Emoji picker** (Win+. or Win+;, `tilewinmsg panel emoji`) like Windows': all emoji of Unicode in their groups and the ones you used lately, searched by name as soon as you type. A click or Enter types the emoji into the window you were writing in (Shift keeps the picker open); the clipboard gets back what it held.
- **Workspaces** on the taskbar like KDE Plasma's pager: every virtual desktop as a small screen with its windows where they are, as outlines with their app icons, the desktop shown outlined. Rest the pointer on one for a preview of that desktop with its windows as they look, click it (or the preview) to go there, scroll to walk through them; the right button moves, makes and closes desktops (`labels no` hides the desktop numbers, `icons no` the icons).
- **Sticky Notes** like Windows 7's: New > Sticky note on the desktop (or `tilewinmsg panel note new`) puts a note of yellow paper on the desktop, under the windows, that you type into right there. Its strip moves it, "+" makes another, "x" throws it away, the corner resizes it and the right button picks yellow, blue, green, pink, purple or white. Each note is a text file in `~/.local/share/tileWin/notes`, saved as you type.
- **Accent color from the wallpaper:** with the switch on the Theme page, the Windows 10 and 11 themes take the most striking color of the wallpaper for the taskbar, the window frames and the highlights, as Windows does, and follow it when the wallpaper changes.
- **Theme icons in other apps:** Thunar, Dolphin, Nautilus, file dialogs and other GTK, KDE and Qt apps show the icons of the theme too, e.g. Windows XP folders and files (see [Icons](#icons)).
- **Themes:** switch with `tilewin-theme set <name>`. Create your own themes, inheriting from the built-in ones.
- **Dark mode:** `tilewin-theme scheme dark` (or the switch in tileWin Settings) gives every theme dark title bars, menus, flyouts and start menu, and switches GTK, GNOME (and Qt apps through the desktop portal) and KDE apps to dark as well.
- **Apps behind the windows** (the App windows page of the settings):
  - **Not responding, like on Windows:** an app that gives no answer within five seconds after you click, type into, focus or close its window gets "(Not Responding)" in its title and on the taskbar, and its window turns pale. Closing it then brings up "… is not responding" with **Close the program** (ends it at once, `end_task`) and **Wait**. It all goes away as soon as the app answers. Apps are only asked when you do something with their window, so an idle desktop wakes nothing.
  - **Minimized apps stop** (`pause_minimized 5m`, off by default): an app all of whose windows are minimized is stopped (SIGSTOP, together with the programs it started) after that long, so it takes no processor time or battery, and goes on the moment one of its windows is shown, focused or closed. Terminals and apps running a shell, apps with a window of another process of theirs shown, apps keeping the screen on, apps playing sound (`pause_minimized_sound keep`) and apps in `pause_minimized_except` keep running. If tileWin itself ends without letting them go on, the next tileWin does.
  - **Apps open where they were closed** (`remember_windows`, on by default): the first window of an app opens at the place and in the size its window had when it was closed last (on the screen it is started on), maximized or snapped if it was. Dialogs, windows of a fixed size and apps in `remember_windows_except` open as new windows do.
  - **Window rules:** for one app, keep it on top, open it maximized, minimized, full screen or on a given desktop, let it float in tile mode or make it see-through; the settings write them as `for_window` lines.
- **Media buttons** under the preview of a player's window above the taskbar, as on Windows: previous, play/pause and next for any player on MPRIS (Firefox, Chromium, Spotify, mpv, VLC, ...). Play/pause shows what the player is doing, and a button it cannot use now is greyed out.
- **Peek** like Aero Peek: resting the pointer on a window's preview above the taskbar shows only that window, the others as outlines of glass (a minimized one shows where it would open); resting on "Show desktop" shows the desktop. `peek no` on the `taskbar` or `showdesktop` widget turns it off.
- **Magnifier:** hold Alt and turn the scroll wheel to zoom the screen in and out around the pointer, up to 32 times. The pointer stays in the middle of what is shown, so moving the mouse moves the enlarged picture, and it moves more finely the more it is zoomed; past the screen's edge it is dark. `magnifier Super` takes another key, `magnifier off` turns it off, `magnify in|out|off|<factor>` does it from a binding.
- **Session restore:** the apps open at shutdown or logout start again at the next login, on the same workspace and position. Terminals (xfce4-terminal, GNOME Terminal, Konsole, kitty, Alacritty, foot, …) reopen in the directory their shell was in, and file managers (Thunar, Dolphin, Nautilus, Nemo, Caja, PCManFM) reopen the folder that was shown; tileWin switches Thunar, Dolphin and Nemo to show the full path in the window title for this. Apps that save their own state (browsers, editors) bring back their content. Turn it off with `session_restore no`.
- **Reload without logging out:**
  - `reload`: config.
  - `restart panel`: taskbar only.
  - `restart`: everything else. It keeps the session and every window open, unless the compositor binary itself has been replaced, which is the one thing that cannot be done without ending the session. `restart session` ends it anyway, and `restart relaunch-apps` starts your apps again afterwards.
- **Settings app** (`tilewin-settings`, GTK4, Super+I): theme and mode, wallpaper per theme or for all themes, taskbar layout and widgets, right-click and start menus, application launcher and default programs.
- **Fish completions** for `tilewinmsg` (including all tileWin commands and theme names), `tilewin`, `tilewin-theme`, `tilewin-panel` and `tilewin-settings`.
- **Low resource use:**
  - Decorations are drawn once and cached.
  - The panel is event-driven, redraws only on change and polls system information only for widgets that are visible.

## Installation

```sh
git clone <this repository> tileWin
cd tileWin
./install.sh              # installs dependencies (pacman/apt/dnf), builds and installs to /usr/local
```

Options:

| Option | Effect |
|---|---|
| `--prefix <dir>` | Installation prefix (default `/usr/local`) |
| `--no-deps` | Skip installing dependencies |
| `--debug` | Debug build instead of the optimized LTO release build |
| `--destdir <dir>` | Stage the install into a directory (packaging) |
| `--no-user-config` | Don't create `~/.config/tileWin` |
| `--uninstall` | Remove the installation |

The installer:
- creates `~/.config/tileWin/` with the default config files if they don't exist yet
- registers the **tileWin** session for display managers

Start tileWin:
- choose "tileWin" in your display manager (GDM, SDDM, ly, greetd, ...), or
- run `tilewin-session` from a text console.

**Dependencies:**
- **Required:** wlroots 0.20, wayland, wayland-protocols, libinput, libxkbcommon, libevdev, pixman, libdrm, cairo, pango, gdk-pixbuf2, librsvg, json-c, pcre2, xcb-util-wm, Xwayland, systemd-libs (sd-bus, for the tray), meson and ninja.
- **Optional:** pam (the lock screen is skipped without it), gtk4 (the settings app is skipped without it), grim, slurp and wl-clipboard (screenshots and the snipping tool), tesseract with its language data (copying text from the screen), pavucontrol/pactl (volume widget), xfce4-terminal and thunar (the default terminal and file manager in the configs), swaylock.

### Tests

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

### Updating

```sh
git pull && ./install.sh --no-deps && tilewinmsg restart
```

`restart` picks the cheapest way to apply what was installed. When the compositor binary is the same one that is running, it keeps the session: the config, the theme and the taskbar are all started afresh while the windows stay open and untouched. When the compositor itself has been replaced it has to be executed, and that ends the session — a Wayland app has no way to survive the compositor it is talking to, because the display socket goes with it and no toolkit knows how to reconnect. For that case `tilewinmsg restart relaunch-apps` starts your apps again and puts them back where they were, and `tilewinmsg restart session` forces the same thing when nothing was replaced. If only the taskbar changed, `tilewinmsg restart panel` is enough.

## Configuration

All configuration lives in `~/.config/tileWin/` as plain text; the settings app writes it for you. Every file and setting, the commands tileWin adds, the taskbar and how to make a theme of your own are described in [docs/configuration.md](docs/configuration.md).

### Default window mode shortcuts

| Keys | Action |
|---|---|
| Super (tap) / Ctrl+Esc | Start menu |
| Super+R | Run dialog |
| Super+S | Application launcher |
| Super+I | tileWin Settings |
| Super+E | File manager |
| Super+Return | Terminal |
| Super+D, Super+M | Show desktop |
| Super+L | Lock |
| Super+← / → | Snap the window to the left or right half; pressing the other way unsnaps it again |
| Super+↑ / ↓ | Up maximizes, or lifts a half to the quarter above it; down takes that back, restores a snapped window and minimizes one that is not snapped |
| Alt+Tab, Alt+Shift+Tab | Switch windows (release Alt to confirm, Esc to cancel) |
| Super+Tab | Task view: windows and desktops; drag windows onto another or a new desktop |
| Super+Ctrl+D, Super+Ctrl+F4 | New desktop, close the current desktop |
| Super+Alt+← / → | Move the current desktop left / right in the order |
| Alt+F4 | Close window |
| Alt+Space | Window menu |
| Ctrl+Shift+Esc | Task manager |
| Super+1..9 | Activate the n-th taskbar entry |
| Super+Ctrl+← / → | Previous / next virtual desktop |
| Super+Ctrl+Shift+← / → | Move window to previous / next desktop |
| Super+Shift+← / → | Move window to another monitor |
| Print | Screenshot of all screens to the clipboard and ~/Pictures/Screenshots |
| Win+Shift+S | Snipping tool: rectangle, window or full screen to the clipboard, or **Text**: the text in a rectangle, read with tesseract (in English and the session's language when its data is installed) |
| Super+Shift+W | Switch to tile mode |
| Super+Shift+C | Reload config |
| Super+Shift+Ctrl+R | Restart tileWin |
| Super+Shift+Ctrl+P | Restart the taskbar |
| Ctrl+Alt+Del | Shut down dialog: shut down, restart, sleep, hibernate, hybrid sleep, lock or log out |
| Win+Page Up / Win+Page Down | Maximize / minimize the window |
| Win+A / Win+N | Quick settings / notifications (Action Center) |
| Win+V | Clipboard history |
| Win+. or Win+; | Emoji picker |
| Three fingers up / down on the touchpad | Task view / show the desktop |
| Three fingers left / right on the touchpad | Next / previous desktop |

Tile mode uses sway's default bindings (`$mod` = Super, `$mod+d` opens the launcher) plus `Super+Shift+W` to switch mode, `Super+Shift+Ctrl+R` to restart and the three finger swipes (up: task view, left / right: workspaces).

Configs from before the gestures get them added on start unless they already bind a three finger swipe (`bindgesture swipe:3:…`); an `unbindgesture swipe:3:up` line removes one again.

### Volume, brightness and media keys

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

### Notifications and the Action Center

The taskbar is the notification service of the desktop (`org.freedesktop.Notifications`), so `notify-send` and apps show their notifications on it. They pop up above the notification area as toasts (Windows 10 and 11) or balloons (95, XP and 7) and then go to the Action Center: the bell button next to the clock, `Win+N` or `tilewinmsg panel notifications`.

- Click a notification to open the app, use its buttons, or close it with ×. Hovering keeps it on screen.
- The Action Center groups the history by app (kept across restarts, up to 50), with *Clear all* and a *Do not disturb* switch. Do not disturb (`panel dnd on|off|toggle`) only lets urgent notifications pop up.
- If another notification daemon (mako, dunst, …) is running, tileWin waits and takes over when it quits.
- Theme keys: `notifications.style toast|balloon`, `notifications.center side|flyout`, `notifications.bg`, `.fg`, `.border`, `.badge`.
- Configs from older versions get the bell button after the clock; `notifications_button no` in taskbar.conf keeps it away (the Settings app writes this when you remove it).

### Quick settings

`Win+A` (or `tilewinmsg panel quicksettings`) opens quick settings like on Windows 11: buttons for Wi-Fi, Bluetooth, airplane mode, do not disturb, night light and tile mode, sliders for volume and brightness, the battery and a link to the settings. The arrows next to Wi-Fi, Bluetooth and the volume open their own flyouts. In the Windows 11 theme the network, volume and battery icons of the taskbar open it too (theme key `panel.quick_settings`, or `quick_settings yes|no` on those widgets).

Wi-Fi uses NetworkManager (`nmcli`), airplane mode `rfkill`, the volume `pactl`, the brightness `brightnessctl`.

### Clipboard history

`Win+V` (or `tilewinmsg panel clipboard`) lists the last 25 copied texts and pictures. Click one (or pick it with the arrow keys and Enter) to paste it into the window you were using; tileWin types Ctrl+V for you (Ctrl+Shift+V in terminals). Pin entries to keep them after a restart, × removes one, *Clear all* removes everything that isn't pinned.

Copies that password managers mark as secret are not recorded. The Clipboard group on the Taskbar page of the settings turns the history off (`clipboard_history no` in taskbar.conf) and can have a picked entry only copied instead of pasted (`clipboard_paste no`).

The history is kept by **tilewin-clipboard**, a program of its own that the taskbar starts. A single copied picture can be sixteen megabytes, and keeping that inside the taskbar made the taskbar look like it was growing for no reason; in its own process a look at `htop` says plainly what the memory is for. The taskbar only holds a thumbnail and the first few hundred bytes of each text. It keeps running when the taskbar restarts, so the history survives `tilewinmsg restart panel`.

### Git

The **git** widget shows the branch, the newest tag reachable from HEAD and the lines added and removed of the repository the focused window is working in:

```
main  v1.0.7  ↑2  +42 -7
```

The directory comes from the kernel rather than from the shell: the focused window has a process, and the foreground process of its terminal has a `/proc/<pid>/cwd`. Nothing is sourced into a shell, so bash, zsh, fish and the rest behave identically. Terminals that serve every window from one process (`xfce4-terminal`, `gnome-terminal` and `konsole` in their default mode) cannot be told apart this way; start them with `--disable-server` (`--disable-factory` for gnome-terminal) to give each window its own process.

The reading **stays on the last repository it saw**, so moving to a browser or an editor leaves the outstanding work on screen instead of blanking it. Clicking opens a terminal in the repository. The tooltip adds the path, what is staged, changed, untracked or conflicted, and how far the branch is from its upstream. Theme keys `git.clean`, `git.dirty` and `git.conflict` color it; `interval` (default 5 s), `show_tag`, `icon` and `max_width` are widget options.

git is asked in the background — one shell running three git commands, whose answers the taskbar reads when they are ready — so the bar never waits for a slow repository, and nothing runs at all while the widget is off screen.

### Bluetooth

The Bluetooth flyout (`panel bluetooth`, or the arrow of the Bluetooth button) turns Bluetooth on and off, lists paired and nearby devices and connects, disconnects, pairs and forgets them. It talks to BlueZ directly, so `bluetoothctl` is not needed, but `bluetoothd` must run (`sudo systemctl enable --now bluetooth`). While a device pairs, codes to type on it are shown in the flyout.

### Night light

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
| Taskbar | Font, layouts of both modes (position, height, widgets in the left/center/right sections, put in order by dragging each one by its handle, also from one section into another), settings of each widget, custom script widgets, quick launch apps and the icon of each of them, and the right-click menus of the taskbar, of the taskbar buttons and of the start button (with submenus) |
| Start menu | The style of the start menu, its pinned apps, its places and its power entries |
| Launcher & apps | Built-in launcher, rofi, wofi, fuzzel, tofi, bemenu or any command; terminal, file manager, task manager, locker and screenshot programs |
| Keyboard | Keyboard layouts and variants, layout switch shortcut, Caps Lock and Compose key, Num Lock, key repeat, and the shortcuts of window mode and tile mode (with a key recorder) |
| Mouse & touchpad | Pointer speed and acceleration, scrolling speed and direction, left-handed buttons, tap to click, tap and drag, disable while typing, touchpad scroll and click methods, cursor theme and size, showing the pointer when Ctrl is tapped, growing it when the mouse is shaken, pointer trail lifetime, double-click speed with a folder to try it on |
| Apps | Default apps (web browser, email, file manager, terminal, task manager, text editor, pictures, music, videos, PDF) and startup apps: turn them on or off, add or remove them. Each one can be picked from the installed apps or, with **Other...**, given any program at all: a command to type or an executable to pick off the disk, for the things that have no entry of their own. The menus, shortcuts and flyouts all use these, so a program is named in one place only. A task manager that is not installed opens whichever one the machine does have, or the terminal running btop, htop or top, instead of nothing at all |
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

Your own themes, and every key a theme can set: [docs/configuration.md](docs/configuration.md#your-own-themes-themeconf).

### Icons

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

### Wallpapers

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

## Limitations

- Session restore starts apps again from their command line: terminals come back in their last directory but not with their running programs or scrollback, and documents or tabs only come back if the app restores them itself. Tile mode restores workspaces but not the split layout.
- Microsoft fonts, icons and logos are not included; themes use font fallback lists, drawn glyphs and original look-alike icons.
- There is no background blur, so the Windows 7 glass is translucent only.
- Windows 11 rounds only the frame and title bar; window contents keep square corners.
- Apps that draw their own title bars (GTK4/libadwaita) keep them in window mode.
- A compositor restart closes running Wayland apps (`restart relaunch-apps` starts them again).

## License

MIT. tileWin is based on [sway](https://github.com/swaywm/sway) (MIT, Copyright © 2016-2024 Drew DeVault and contributors).
