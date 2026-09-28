#!/usr/bin/env python3
"""An X11 window that draws its own title bar, as Bambu Studio (wxWidgets on
GTK, forced to X11) does:
 - no frame from the window manager (_MOTIF_WM_HINTS without a title);
 - dragged by moving itself: on a press in its top 30 pixels it notes where
   the pointer is (in root coordinates) against the window, and on each
   motion with the button held it moves itself (a configure request) so the
   pointer keeps that place;
 - its buttons ask for maximize (_NET_WM_STATE), for minimize
   (WM_CHANGE_STATE) and, for restore, only to be activated
   (_NET_ACTIVE_WINDOW), which is what wxWidgets' Restore() does on GTK.

Commands are read from stdin, one per line:
  move X Y      move the window there (root coordinates)
  maximize      ask to be maximized
  minimize      ask to be minimized (iconified)
  present       ask to be activated (the restore button)
  state         print "x y width height" and the _NET_WM_STATE atoms
  quit
"""
import os
import select
import sys
from Xlib import X, Xatom, display

d = display.Display()
screen = d.screen()
root = screen.root
atom = lambda name: d.intern_atom(name)

win = root.create_window(100, 100, 400, 300, 0, screen.root_depth, X.InputOutput,
	X.CopyFromParent, background_pixel=screen.white_pixel,
	event_mask=X.StructureNotifyMask | X.PropertyChangeMask | X.ButtonPressMask |
	X.ButtonReleaseMask | X.Button1MotionMask)
win.set_wm_name("x11 titlebar")
win.set_wm_class("x11titlebar", "X11titlebar")
# flags: functions and decorations; decorations: border, resize, minimize,
# maximize, but no title (what Bambu Studio sets)
win.change_property(atom("_MOTIF_WM_HINTS"), atom("_MOTIF_WM_HINTS"), 32,
	[3, 0x3e, 0x66, 0, 0])
win.map()
d.sync()


def client_message(type_name, data):
	ev = display.event.ClientMessage(window=win, client_type=atom(type_name),
		data=(32, (data + [0] * 5)[:5]))
	root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
	d.sync()


def position():
	pos = win.translate_coords(root, 0, 0)
	return -pos.x, -pos.y


def state():
	geo = win.get_geometry()
	x, y = position()
	prop = win.get_full_property(atom("_NET_WM_STATE"), Xatom.ATOM)
	names = [d.get_atom_name(a) for a in prop.value] if prop else []
	return "%d %d %d %d %s" % (x, y, geo.width, geo.height,
		",".join(n.replace("_NET_WM_STATE_", "") for n in names) or "-")


drag = None  # where the pointer is against the window, while dragging


def handle_event(ev):
	global drag
	if ev.type == X.ButtonPress and ev.detail == 1 and ev.event_y < 30:
		x, y = position()
		drag = (ev.root_x - x, ev.root_y - y)
	elif ev.type == X.ButtonRelease and ev.detail == 1:
		drag = None
	elif ev.type == X.MotionNotify and drag:
		pointer = root.query_pointer()  # as wxGetMousePosition()
		win.configure(x=pointer.root_x - drag[0], y=pointer.root_y - drag[1])
		d.sync()


def handle_command(words):
	if words[0] == "move":
		win.configure(x=int(words[1]), y=int(words[2]))
		d.sync()
	elif words[0] == "maximize":
		client_message("_NET_WM_STATE", [1, atom("_NET_WM_STATE_MAXIMIZED_VERT"),
			atom("_NET_WM_STATE_MAXIMIZED_HORZ"), 1])
	elif words[0] == "minimize":
		client_message("WM_CHANGE_STATE", [3])  # IconicState
	elif words[0] == "present":
		client_message("_NET_ACTIVE_WINDOW", [1, X.CurrentTime, 0])
	elif words[0] == "state":
		print(state(), flush=True)
	elif words[0] == "quit":
		sys.exit(0)


print("ready", flush=True)
stdin = sys.stdin.fileno()
pending = b""
while True:
	while d.pending_events():
		handle_event(d.next_event())
	readable, _, _ = select.select([stdin, d.fileno()], [], [])
	if stdin in readable:
		chunk = os.read(stdin, 4096)
		if not chunk:
			break
		pending += chunk
		while b"\n" in pending:
			line, pending = pending.split(b"\n", 1)
			words = line.decode().split()
			if words:
				handle_command(words)
