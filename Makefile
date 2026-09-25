 # RavenMPV GUI - Single File Edition

CC = gcc
CFLAGS = -O2 -Wall
GUI_CFLAGS = $(shell pkg-config --cflags gtk4)
GUI_LIBS = $(shell pkg-config --libs gtk4) -lcurl -lpthread

all: raven-gui

raven-gui: raven_gui.c
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -o $@ $< $(GUI_LIBS)
	@echo "✓ Built: raven-gui"

clean:
	rm -f raven-gui
	rm -rf /tmp/raven_thumbs
	@echo "✓ Cleaned"

run: raven-gui
	./raven-gui

install: raven-gui
	install -m 755 raven-gui /usr/local/bin/
	@echo "✓ Installed to /usr/local/bin/raven-gui"

uninstall:
	rm -f /usr/local/bin/raven-gui
	@echo "✓ Uninstalled"

.PHONY: all clean run install uninstall
