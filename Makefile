# RavenMPV Makefile
# Simple build system for CLI and GUI versions

CC = gcc
CFLAGS = -O2 -Wall

# Libraries
CLI_LIBS = -lncurses -lm
GUI_CFLAGS = $(shell pkg-config --cflags gtk4)
GUI_LIBS = $(shell pkg-config --libs gtk4) -lcurl -lpthread

# Default target: build both
all: ravenmpv raven-gui

# CLI binary
ravenmpv: ravenmpv.c
	$(CC) $(CFLAGS) -o $@ $< $(CLI_LIBS)
	@echo "✓ CLI built: ravenmpv"

# GUI binary
raven-gui: raven_gui.c
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -o $@ $< $(GUI_LIBS)
	@echo "✓ GUI built: raven-gui"

# Clean build artifacts
clean:
	rm -f ravenmpv raven-gui
	rm -rf /tmp/raven_thumbs
	@echo "✓ Cleaned"

# Run CLI
run-cli: ravenmpv
	./ravenmpv

# Run GUI
run-gui: raven-gui
	./raven-gui

# Install to /usr/local
install: all
	install -m 755 ravenmpv /usr/local/bin/
	install -m 755 raven-gui /usr/local/bin/
	@echo "✓ Installed to /usr/local/bin/"

# Uninstall
uninstall:
	rm -f /usr/local/bin/ravenmpv
	rm -f /usr/local/bin/raven-gui
	@echo "✓ Uninstalled"

# Show help
help:
	@echo "RavenMPV Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make              Build CLI + GUI"
	@echo "  make ravenmpv     Build CLI only"
	@echo "  make raven-gui    Build GUI only"
	@echo "  make clean        Remove binaries"
	@echo "  make run-cli      Build and run CLI"
	@echo "  make run-gui      Build and run GUI"
	@echo "  make install      Install to /usr/local"
	@echo "  make uninstall    Remove from /usr/local"
	@echo "  make help         Show this message"

.PHONY: all clean run-cli run-gui install uninstall help
