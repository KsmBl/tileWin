# Screen savers of your own

Every screen saver of tileWin is a directory. Make one, drop it in the right
folder, and it appears on the Screen saver page of the settings, with its
picture, its settings and the preview, and `tilewin-screensaver` can show it.

## Where they are

A screen saver directory holds:

| File | What it is |
|---|---|
| `saver.so` | the saver: a module that draws it with cairo |
| `screenshot.png` | a picture of it for the list in the settings (optional) |

tileWin looks for such directories in these folders, and of two savers with
the same name takes the first found:

1. the folders in `$TILEWIN_SAVERS`, separated by colons (for trying things out);
2. `~/.local/share/tileWin/screensavers` (`$XDG_DATA_HOME/tileWin/screensavers`): your own;
3. `screensavers` in tileWin's data folder (`/usr/local/share/tileWin/screensavers`):
   the ones that come with tileWin.

So a saver of your own goes into
`~/.local/share/tileWin/screensavers/<name>/`. A saver there with the name of
one of tileWin's replaces it. The settings find the savers when their Screen
saver page opens; `tilewin-screensaver --list` shows all it finds and where.

A directory that holds no working `saver.so` is left out, with the reason on
the standard error of the program that looked.

## What a saver is

A saver is a `struct saver` (in `savers.h`) with its name, title, description,
and three functions:

```c
void *create(int width, int height, const struct saver_options *options);
void draw(void *state, cairo_t *cr, int width, int height, double dt);
void destroy(void *state);
```

- `create` sets it up for an area of `width` by `height` pixels: the whole
  screen, or the little monitor of the preview. Make sizes relative to the area
  (`saver_unit(width, height)` is a thousandth of the shorter side), so the same
  code serves both.
- `draw` draws the next picture. `dt` is the seconds since the last one,
  already multiplied by the Speed setting. What was drawn before is cleared to
  black first (or to see-through for `.transparent` savers, which are drawn
  over the desktop); a saver that paints every pixel itself can say
  `.covers = true` and save that clearing.
- `destroy` frees what `create` made.

Other fields: `.transparent` (drawn over the desktop, which shows through),
`.resolution` (drawn at a part of the size and scaled up, for soft pictures),
`.wants_desktop` (gets a picture of the screen from before it started, in
`options->desktop`; `saver_desktop()` gives it, with where the windows and the
taskbars are, or stand-ins for the preview).

The module says which saver it has with one line:

```c
TILEWIN_SAVER(.saver = &my_saver);
```

with, if you like, `.order` (the place in the list, lower first; savers without
one come after tileWin's, by title) and `.shot_seconds` (how long it runs before
its screenshot is taken).

## Settings of its own

A saver can have settings, which the Screen saver page shows while it is
picked and which are kept in the `screensaver` block of `taskbar.conf` as
`<name>_<key> <value>`:

```c
static const char *const colour_values[] = { "red", "purple", "green", NULL };
static const char *const colour_labels[] = { "Red", "Purple", "Green", NULL };
static const struct saver_option my_options[] = {
	{ "colour", "Wax", "The colour of the wax", SAVER_CHOICE, colour_values, colour_labels, false,
		NULL },
	{ "bubbles", "Bubbles", NULL, SAVER_TOGGLE, NULL, NULL, true, NULL },
	{ 0 },
};
```

Read them in `create` with `saver_choice(options, &my_saver, "colour")` (the
index into the values, 0 when not set) and `saver_toggle(options, &my_saver,
"bubbles")`. The last field of a setting, `when`, shows it only while another
choice of the saver has a value, as `"kind=blizzard"`.

## Helpers

`tilewin-saver.h` brings all a saver can use, lent to it by the program that
loads it:

- `saver_random()`, `saver_between(a, b)`, `saver_clamp()`, `saver_unit()`,
  `saver_hsv()`, `saver_set_hsva()`;
- `saver_desktop()`, `saver_bare_desktop()`, `saver_wallpaper()`,
  `saver_tilewin_windows()`: the screen, the windows and the taskbars as they
  were, and the wallpaper;
- `saver_run_new()`, `saver_run_draw()`, `saver_run_free()`: to run another
  saver inside yours (Doomsday runs its kinds so);
- in `saver_noise.h`: value noise, fractal noise, a noise grid and a fast mix
  of two pixels.

Use nothing else of tileWin: a module links only cairo (and whatever else it
needs itself), and the program lends it these functions when it loads it.

## An example

`example/lavalamp` is a whole saver of this kind: `lavalamp.c`, about a hundred
lines, with a setting of its own, and a `Makefile`:

```sh
cd screensaver/example/lavalamp
make                # builds saver.so against the installed headers
make install        # into ~/.local/share/tileWin/screensavers/lavalamp, with its screenshot
```

The headers are installed with tileWin in `/usr/local/include/tileWin`
(`make TILEWIN_INCLUDE=...` when they are elsewhere). By hand:

```sh
cc -O2 -shared -fPIC -I/usr/local/include/tileWin $(pkg-config --cflags cairo) \
	-o saver.so mysaver.c $(pkg-config --libs cairo) -lm
```

## Its screenshot

```sh
tilewin-screensaver --screenshot <directory> <directory>/screenshot.png [--seconds <s>]
```

runs the saver of a directory for a few seconds on stand-in windows, as the
preview does, and writes the picture. tileWin's own savers get theirs so when
they are built.

## Trying it out

```sh
TILEWIN_SAVERS=$PWD/.. tilewin-screensaver --saver lavalamp
```

shows it on the screen at once, until the mouse moves; the folder given holds
the saver's directory.

## The savers that come with tileWin

They are in `savers/`, a directory each, built the same way (except that they
link the helpers in instead of borrowing them). Doomsday is one saver with five
kinds (Hellfire, Thunderstorm, Blizzard, Decay, Jungle) in one directory; its
kinds are found by their own names too, as `.hidden` of its module.
