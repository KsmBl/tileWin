#include <string.h>
#include "tw_widgets.h"

static const char *const choice_yes_no[] = { "yes", "no", NULL };
static const char *const choice_theme_yes_no[] = { "theme", "yes", "no", NULL };
static const char *const choice_current_all[] = { "current", "all", NULL };
static const char *const choice_current_all_main[] = { "current", "all", "main", NULL };
static const char *const choice_close_new[] = { "close", "new", NULL };
static const char *const choice_meter[] = { "text", "graph", "bar", NULL };

static const struct tw_widget_option opts_none[] = {
	{ 0 },
};
static const struct tw_widget_option opts_clock[] = {
	{ "format", "Format", "strftime format, \\n starts a second line. The theme decides by default.", NULL },
	{ "tooltip_format", "Tooltip format", "strftime format of the tooltip", NULL },
	{ "settings", "Settings link", "Opened by the link in the calendar, default exec tilewin-settings --page datetime", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_git[] = {
	{ "show_tag", "Show the newest tag", "The tag git describe reaches from HEAD, e.g. v1.0.7",
		choice_yes_no },
	{ "show_untracked", "Count untracked files", "Adds “?3” for files git does not follow yet",
		choice_yes_no },
	{ "interval", "Update interval", "Seconds between checks; 5 by default, and it also "
		"looks whenever another window is focused", NULL },
	{ "icon", "Icon", "Drawn before the branch, e.g. a Nerd Font glyph. Empty by default, "
		"because not every font has one", NULL },
	{ "max_width", "Maximum width", "Pixels; empty or 0 lets it take the room it needs", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default, and the added and removed "
		"counts keep their own colors", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_cpu[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{usage} is the load in percent", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ "task_manager", "Task manager", "Opened by the link in the flyout, e.g. exec btop", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_memory[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "format", "Format", "{used_percent}, {used} and {total} in GiB", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ "task_manager", "Task manager", "Opened by the link in the flyout, e.g. exec btop", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_gpu[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{usage} is the load in percent", NULL },
	{ "device", "Card", "e.g. card0; the first one that reports anything by default", NULL },
	{ "command", "Command", "For cards that report nothing in /sys, e.g. nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_net[] = {
	{ "interval", "Update interval", "Seconds, default 2", NULL },
	{ "format", "Format", "{down}, {up}, {total} and {device}", NULL },
	{ "device", "Interface", "e.g. wlan0; the busiest one by default", NULL },
	{ "max_rate", "Full scale", "KiB per second the chart and the bar are drawn against, default 12500", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_storage[] = {
	{ "interval", "Update interval", "Seconds, default 30", NULL },
	{ "path", "Folder", "Any folder of the file system to watch, default /", NULL },
	{ "format", "Format", "{used_percent}, {used}, {free}, {total} in GiB and {path}", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_power[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "format", "Format", "{watts} is what is being drawn", NULL },
	{ "device", "Battery", "Name in /sys/class/power_supply, e.g. BAT0", NULL },
	{ "max_watts", "Full scale", "Watts the chart and the bar are drawn against, default 60", NULL },
	{ "style", "Style", "Text, a chart of the last measurements or a bar", choice_meter },
	{ "width", "Width", "Pixels, for the chart and the bar", NULL },
	{ "warning", "Warning above", "Percent; the text turns to the warning color", NULL },
	{ "critical", "Critical above", "Percent; the text turns to the critical color", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ "warning_fg", "Warning color", "e.g. #fbbf24", NULL },
	{ "critical_fg", "Critical color", "e.g. #f87171", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_disk[] = {
	{ "devices", "Disk", "The disk whose lamp this is; every whole disk by default", NULL },
	{ "threshold", "Threshold", "KiB per second before the lamp lights up, default 50", NULL },
	{ "interval", "Update interval", "Seconds, default 1", NULL },
	{ "format", "Format", "{rate} is e.g. 1.2 MB/s, {kbps} the plain number; empty shows only the lamp", NULL },
	{ "fg", "Color", "e.g. #7eb8f7; the theme decides by default", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_battery[] = {
	{ "interval", "Update interval", "Seconds, default 30", NULL },
	{ "format", "Format", "{capacity} is the charge in percent", NULL },
	{ "format_charging", "Format while charging", "Used instead of Format while the battery charges", NULL },
	{ "format_full", "Format when full", "Used instead of Format once the battery is full", NULL },
	{ "format_plugged", "Format on mains", "Used instead of Format while the charger is plugged in", NULL },
	{ "device", "Device", "Name in /sys/class/power_supply, e.g. BAT0", NULL },
	{ "icons", "Icons", "Characters for {icon}, lowest charge first, separated by spaces", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the battery flyout", choice_yes_no },
	{ "settings", "Settings link", "Opened by the link in the flyout, e.g. exec tilewin-settings --page screen", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_network[] = {
	{ "interval", "Update interval", "Seconds, default 5", NULL },
	{ "interface", "Interface", "e.g. wlan0; detected automatically if empty", NULL },
	{ "format", "Format", "{essid}, {quality} and {ifname}", NULL },
	{ "format_ethernet", "Format on a cable", "Used instead of Format for a wired connection", NULL },
	{ "format_disconnected", "Format when offline", "Used instead of Format while nothing is connected", NULL },
	{ "icons", "Icons", "Characters for {icon}, weakest signal first", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the network flyout", choice_yes_no },
	{ "settings", "Settings link", "Opened by the link in the flyout, default exec nm-connection-editor", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_volume[] = {
	{ "mixer", "Mixer command", "Runs on click, default exec pavucontrol", NULL },
	{ "step", "Scroll step", "Percent, default 5", NULL },
	{ "format", "Format", "{volume} is the level in percent", NULL },
	{ "format_muted", "Format when muted", "Used instead of Format while the sound is off", NULL },
	{ "icons", "Icons", "Characters for {icon}, quietest first", NULL },
	{ "quick_settings", "Click opens quick settings", "The Windows 11 flyout instead of the volume flyout", choice_yes_no },
	{ 0 },
};
static const struct tw_widget_option opts_brightness[] = {
	{ "format", "Format", "{percent} is the brightness", NULL },
	{ "icons", "Icons", "Characters for {icon}, darkest first", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_notifications[] = {
	{ "always", "Always show the button", "Otherwise it appears only when something is waiting", choice_yes_no },
	{ 0 },
};
static const struct tw_widget_option opts_taskbar[] = {
	{ "icons_only", "Icons only", "\"theme\" follows the theme (Windows 7 and 11 show icons only)", choice_theme_yes_no },
	{ "group", "Combine windows of the same app", NULL, choice_yes_no },
	{ "workspaces", "Show windows of", "The current workspace or all workspaces", choice_current_all },
	{ "outputs", "Show windows on", "The taskbar of their screen, every taskbar, or also the "
		"taskbar of the main display (\"main\", as on Windows)", choice_current_all_main },
	{ "middle_click", "Middle click", "Close the window or start a new one", choice_close_new },
	{ "max_width", "Maximum button width", "Pixels", NULL },
	{ "button_width", "Button width", "Pixels", NULL },
	{ "thumbnails", "Preview on hover", "A live picture of the window above the button", choice_yes_no },
	{ 0 },
};
static const struct tw_widget_option opts_search[] = {
	{ "label", "Placeholder text", NULL, NULL },
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_start[] = {
	{ "label", "Label", "The theme decides by default", NULL },
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_title[] = {
	{ "max_width", "Maximum width", "Pixels, default 480", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_width[] = {
	{ "width", "Width", "Pixels", NULL },
	{ 0 },
};
static const struct tw_widget_option opts_custom[] = {
	{ "exec", "Command", "Shell command whose output is shown", NULL },
	{ "interval", "Interval", "Seconds between runs; 0 runs it once", NULL },
	{ "exec_listen", "Streaming command", "Instead of Command: keeps running and prints one line (or JSON) per update", NULL },
	{ "format", "Format", "{} is replaced with the output", NULL },
	{ "icon", "Icon", "Icon name or path", NULL },
	{ 0 },
};

#define BOTH (TW_WIDGET_TASKBAR | TW_WIDGET_DESKTOP)
#define STATUS (BOTH | TW_WIDGET_STATUS)

const struct tw_widget_info tw_widgets[] = {
	{ "start", "Start button", "Opens the start menu", BOTH, opts_start },
	{ "search", "Search box", "Opens the start menu to search apps", BOTH, opts_search },
	{ "taskbar", "Window buttons", "A button for each open window", BOTH, opts_taskbar },
	{ "quicklaunch", "Quick launch", "Icons of pinned apps", BOTH, opts_none },
	{ "workspaces", "Workspaces", "Buttons for the virtual desktops", BOTH, opts_none },
	{ "title", "Window title", "Title of the focused window, click for the window", BOTH,
		opts_title },
	{ "tray", "System tray", "Icons of background apps", STATUS, opts_none },
	{ "keyboard", "Keyboard layout", "Current layout, click to pick another", STATUS, opts_none },
	{ "volume", "Volume", "Speaker volume", STATUS, opts_volume },
	{ "network", "Network", "Connection status", STATUS, opts_network },
	{ "battery", "Battery", "Charge level", STATUS, opts_battery },
	{ "brightness", "Brightness", "Screen brightness", STATUS, opts_brightness },
	{ "cpu", "CPU usage", "Processor load", STATUS, opts_cpu },
	{ "memory", "Memory usage", "RAM in use, click for a flyout", STATUS, opts_memory },
	{ "disk", "Disk activity", "A lamp that lights up while the disks are busy", STATUS, opts_disk },
	{ "gpu", "GPU usage", "Load of a graphics card", STATUS, opts_gpu },
	{ "net", "Network usage", "What goes through an interface", STATUS, opts_net },
	{ "storage", "Disk space", "How full a file system is", STATUS, opts_storage },
	{ "power", "Power draw", "Watts the computer is drawing", STATUS, opts_power },
	{ "git", "Git", "Branch and changed lines of the repository the focused window works in",
		STATUS, opts_git },
	{ "clock", "Clock", "Time and date with a calendar", STATUS, opts_clock },
	{ "notifications", "Notifications", "Opens the Action Center with the notification history",
		BOTH, opts_notifications },
	{ "modeswitch", "Mode switch", "Switches between tile and window mode", BOTH, opts_none },
	{ "showdesktop", "Show desktop", "Minimizes all windows", BOTH, opts_width },
	// the layout helpers only make sense between other widgets on the taskbar
	{ "separator", "Separator", "A thin line", TW_WIDGET_TASKBAR, opts_width },
	{ "spacer", "Spacer", "Empty space", TW_WIDGET_TASKBAR, opts_width },
	{ "custom", "Script", "Shows the output of a command", STATUS | TW_WIDGET_UNLISTED,
		opts_custom },
};

const size_t tw_widget_count = sizeof(tw_widgets) / sizeof(tw_widgets[0]);

const struct tw_widget_option tw_widget_events[] = {
	{ "on_click", "On click", "Command, e.g. exec pavucontrol", NULL },
	{ "on_middle_click", "On middle click", NULL, NULL },
	{ "on_right_click", "On right click", "Replaces the right-click menu", NULL },
	{ "on_scroll_up", "On scroll up", NULL, NULL },
	{ "on_scroll_down", "On scroll down", NULL, NULL },
	{ 0 },
};

static const char *const choice_desktop_output[] = { "main", "all", NULL };

const struct tw_widget_option tw_widget_desktop_options[] = {
	{ "x", "From the left", "Pixels from the left edge of the screen; a negative number counts "
		"from the right edge. A widget dragged somewhere stays there until this changes.", NULL },
	{ "y", "From the top", "Pixels from the top edge; a negative number counts from the bottom",
		NULL },
	{ "scale", "Size", "How much bigger than on the taskbar, e.g. 1.5; default 2", NULL },
	{ "output", "Screen", "The main display, every screen, or a screen by its name, e.g. DP-1",
		choice_desktop_output },
	{ "background", "Background", "A translucent card behind the widget", choice_yes_no },
	{ 0 },
};

const struct tw_widget_info *tw_widget_find(const char *type_or_name) {
	if (!type_or_name) {
		return NULL;
	}
	const char *colon = strchr(type_or_name, ':');
	size_t len = colon ? (size_t)(colon - type_or_name) : strlen(type_or_name);
	for (size_t i = 0; i < tw_widget_count; i++) {
		if (strlen(tw_widgets[i].type) == len &&
				strncmp(tw_widgets[i].type, type_or_name, len) == 0) {
			return &tw_widgets[i];
		}
	}
	return NULL;
}
