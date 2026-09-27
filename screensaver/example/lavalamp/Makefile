# Builds the Lava Lamp screen saver: make, then make install puts it where
# tileWin finds it (~/.local/share/tileWin/screensavers/lavalamp) with a
# screenshot for the settings.
#
# The headers come with tileWin (in /usr/local/include/tileWin, or where it was
# installed); what the saver uses of tileWin is lent by the program that loads
# it, so nothing else of tileWin is linked in.

TILEWIN_INCLUDE ?= /usr/local/include/tileWin
CFLAGS ?= -O2 -Wall
DEST ?= $(HOME)/.local/share/tileWin/screensavers/lavalamp

saver.so: lavalamp.c
	$(CC) $(CFLAGS) -shared -fPIC -I$(TILEWIN_INCLUDE) $$(pkg-config --cflags cairo) \
		-o $@ lavalamp.c $$(pkg-config --libs cairo) -lm

install: saver.so
	mkdir -p $(DEST)
	cp saver.so $(DEST)/
	tilewin-screensaver --screenshot $(DEST) $(DEST)/screenshot.png

clean:
	rm -f saver.so screenshot.png
