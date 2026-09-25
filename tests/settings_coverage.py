#!/usr/bin/env python3
"""Nothing may be settable only by hand.

Every option the taskbar and the compositor read has to have a control in the
settings app. This walks the sources, collects what is read on one side and
what is offered on the other, and complains about the difference. When a new
setting turns up here, either give it a row in the settings app or put it in
one of the lists below and say why it does not belong in a window.

It also checks that the command tables of the compositor stay in alphabetical
order, because they are searched with bsearch and a misplaced line silently
makes a command unreachable.
"""

import re
import sys
from pathlib import Path

# Widget options that are deliberately not offered in the settings app.
WIDGET_OPTIONS_WITHOUT_A_CONTROL = {
    "type": "names the widget itself, not one of its settings",
}

# Widget types that are not in the widget list.
WIDGET_TYPES_NOT_IN_THE_LIST = {
}

# Keys at the top level of taskbar.conf with no control.
TASKBAR_KEYS_WITHOUT_A_CONTROL = {
    "outputs": "which monitors carry a taskbar; the Screen page owns the monitors",
    "layout": "the sections are edited as a whole on the Taskbar page",
    "menu": "edited in the Right-click menus group of the Taskbar page",
    "startmenu": "edited on the Start menu page",
    "position": "inside a layout block, edited on the Taskbar page",
    "height": "inside a layout block, edited on the Taskbar page",
    "left": "a section of a layout block",
    "center": "a section of a layout block",
    "right": "a section of a layout block",
}

# Config commands of the compositor with no control. Sway's own settings are
# here as a block: tileWin keeps them working for people who write a config by
# hand, but the settings app is about the things tileWin adds.
COMPOSITOR_COMMANDS_WITHOUT_A_CONTROL = {
    "assign": "sway: rules for where a window opens",
    "bindcode": "sway: key bindings, edited on the Keyboard page by symbol",
    "bindgesture": "sway: touchpad gestures",
    "bindswitch": "sway: laptop lid and tablet switches",
    "client.background": "sway: ignored, kept so old configs still load",
    "client.focused": "sway: window colors, the theme decides them",
    "client.focused_inactive": "sway: window colors, the theme decides them",
    "client.focused_tab_title": "sway: window colors, the theme decides them",
    "client.placeholder": "sway: ignored, kept so old configs still load",
    "client.unfocused": "sway: window colors, the theme decides them",
    "client.urgent": "sway: window colors, the theme decides them",
    "default_border": "sway: tile mode borders, the theme decides them",
    "default_floating_border": "sway: tile mode borders, the theme decides them",
    "exec_always": "sway: autostart, run again on every reload",
    "floating_maximum_size": "sway: tile mode",
    "floating_minimum_size": "sway: tile mode",
    "focus_wrapping": "sway: tile mode",
    "for_window": "sway: rules per window",
    "force_display_urgency_hint": "sway: tile mode",
    "force_focus_wrapping": "sway: tile mode",
    "gaps": "sway: tile mode",
    "hide_edge_borders": "sway: tile mode",
    "mouse_warping": "sway: tile mode",
    "new_float": "sway: tile mode borders",
    "new_window": "sway: tile mode borders",
    "no_focus": "sway: rules per window",
    "screensaver": "runtime only: starts or ends the screen saver right away",
    "screensaver_command": "which program draws the screen saver; tilewin-screensaver by default",
    "panel_command": "which program draws the taskbar; turning it off leaves no settings app",
    "popup_during_fullscreen": "sway: tile mode",
    "show_marks": "sway: tile mode",
    "smart_borders": "sway: tile mode",
    "smart_gaps": "sway: tile mode",
    "tiling_drag": "sway: tile mode",
    "tiling_drag_threshold": "sway: tile mode",
    "title_align": "sway: tile mode title bars",
    "titlebar_border_thickness": "sway: tile mode title bars",
    "titlebar_padding": "sway: tile mode title bars",
    "unbindcode": "sway: undoes a binding",
    "unbindgesture": "sway: undoes a binding",
    "unbindswitch": "sway: undoes a binding",
    "unbindsym": "sway: undoes a binding",
    "workspace": "sway: tile mode workspaces",
    "workspace_auto_back_and_forth": "sway: tile mode workspaces",
    "xdg_autostart": "runs the autostart entries of the desktop; on by default",
}

problems = []


def complain(message):
    problems.append(message)


def read(root, *parts):
    return (root.joinpath(*parts)).read_text(encoding="utf-8")


def sources(root, folder, pattern="*.c"):
    return sorted(Path(root, folder).rglob(pattern))


def widget_options_read_by_the_panel(root):
    """Keys the taskbar looks up in a widget's block."""
    call = re.compile(r'widget_conf(?:_int|_bool|_color)?\s*\([^,]+,\s*"([a-z_0-9]+)"')
    found = {}
    for path in sources(root, "panel"):
        for key in call.findall(path.read_text(encoding="utf-8")):
            found.setdefault(key, path)
    return found


def widget_options_offered_by_the_settings(root):
    """Keys of the option tables of the widget list, which the settings app shows."""
    text = read(root, "common", "tw_widgets.c")
    tables = re.findall(r"const struct tw_widget_option \w+\[\] = \{(.*?)\n\};", text, re.S)
    keys = set()
    for table in tables:
        keys.update(re.findall(r'^\t\{\s*"([a-z_0-9]+)"', table, re.M))
    return keys


def widget_types_of_the_panel(root):
    types = {}
    for path in sources(root, "panel"):
        for name in re.findall(r'\.type\s*=\s*"([a-z_0-9]+)"', path.read_text(encoding="utf-8")):
            types.setdefault(name, path)
    return types


def widget_types_offered_by_the_settings(root):
    """Types of the widget list (common/tw_widgets.c), which the settings app offers."""
    text = read(root, "common", "tw_widgets.c")
    table = re.search(r"const struct tw_widget_info tw_widgets\[\] = \{(.*?)\n\};", text, re.S)
    if not table:
        complain("common/tw_widgets.c: the tw_widgets table is gone")
        return set()
    return set(re.findall(r'^\t\{\s*"([a-z_0-9]+)"', table.group(1), re.M))


def taskbar_keys_read_by_the_panel(root):
    """Keys read straight off the root of taskbar.conf."""
    call = re.compile(r'twconf_(?:value|child)\s*\(\s*(?:[\w>.-]*\b)?root\s*,\s*"([a-z_0-9]+)"')
    found = {}
    for path in sources(root, "panel"):
        for key in call.findall(path.read_text(encoding="utf-8")):
            found.setdefault(key, path)
    # the sizes of the desktop grid are read through a table, not one by one
    config = read(root, "panel", "config.c")
    table = re.search(r"\} sizes\[\] = \{(.*?)\n\t\};", config, re.S)
    if table:
        for key in re.findall(r'\{\s*"([a-z_0-9]+)"', table.group(1)):
            found.setdefault(key, Path(root, "panel", "config.c"))
    return found


def command_tables(root):
    text = read(root, "sway", "commands.c")
    tables = {}
    for name in ("handlers", "config_handlers", "command_handlers"):
        table = re.search(
            r"static const struct cmd_handler %s\[\] = \{(.*?)\n\};" % name, text, re.S)
        if not table:
            complain("sway/commands.c: the %s table is gone" % name)
            continue
        tables[name] = re.findall(r'\{\s*"([a-z_.0-9]+)"', table.group(1))
    return tables


def strings_in_the_settings(root):
    """Every string literal of the settings app, to look a key up in."""
    paths = sources(root, "settings") + [Path(root, "common", "tw_widgets.c")]
    text = "".join(path.read_text(encoding="utf-8") for path in paths)
    return set(re.findall(r'"([^"\\\n]+)"', text))


def check_widget_options(root, offered):
    for key, path in sorted(widget_options_read_by_the_panel(root).items()):
        if key in offered or key in WIDGET_OPTIONS_WITHOUT_A_CONTROL:
            continue
        complain('%s reads the widget option "%s", which no settings page offers'
                 % (path.relative_to(root), key))


def check_widget_types(root):
    offered = widget_types_offered_by_the_settings(root)
    for name, path in sorted(widget_types_of_the_panel(root).items()):
        if name in offered or name in WIDGET_TYPES_NOT_IN_THE_LIST:
            continue
        complain('%s has the widget "%s", which cannot be added in the settings'
                 % (path.relative_to(root), name))


def check_taskbar_keys(root, settings_strings):
    for key, path in sorted(taskbar_keys_read_by_the_panel(root).items()):
        if key in settings_strings or key in TASKBAR_KEYS_WITHOUT_A_CONTROL:
            continue
        complain('%s reads "%s" from taskbar.conf, which no settings page offers'
                 % (path.relative_to(root), key))


def check_compositor_commands(tables, settings_strings):
    for command in tables.get("handlers", []):
        if command in settings_strings or command in COMPOSITOR_COMMANDS_WITHOUT_A_CONTROL:
            continue
        complain('sway/commands.c: "%s" can be set in the config but not in the settings'
                 % command)


def check_tables_are_sorted(tables):
    for name, commands in tables.items():
        for earlier, later in zip(commands, commands[1:]):
            if earlier.lower() >= later.lower():
                complain('sway/commands.c: %s is searched with bsearch but "%s" comes '
                         'before "%s"' % (name, earlier, later))


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    settings_strings = strings_in_the_settings(root)
    check_widget_options(root, widget_options_offered_by_the_settings(root))
    check_widget_types(root)
    check_taskbar_keys(root, settings_strings)
    tables = command_tables(root)
    check_compositor_commands(tables, settings_strings)
    check_tables_are_sorted(tables)

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print("%d setting(s) cannot be reached from the settings app" % len(problems),
              file=sys.stderr)
        return 1
    print("settings coverage: every setting has a control")
    return 0


if __name__ == "__main__":
    sys.exit(main())
