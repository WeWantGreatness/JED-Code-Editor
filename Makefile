# For packaging we prefer /usr; allow overriding but default to /usr for deb packages
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2
TARGET = editor_min
SRC = editor_min.c highlight.c include_classifier.c keyword_highlight/generated_index.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/jed"
	$(MAKE) install-man PREFIX=$(PREFIX) DESTDIR="$(DESTDIR)"

install-man:
	install -d "$(DESTDIR)$(MANDIR)"
	install -m 0644 man/jed.1 "$(DESTDIR)$(MANDIR)/jed.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/jed"
	rm -f "$(DESTDIR)$(MANDIR)/jed.1.gz"

clean:
	rm -f $(TARGET)

package:
	@echo "Building .deb using dpkg-buildpackage (requires debhelper/debian tools)"
	@dpkg-buildpackage -b -us -uc || (echo "dpkg-buildpackage failed; ensure build deps are installed"; exit 1)

simulate-purge:
	@tmpdir=$$(mktemp -d); \
	echo "Simulating purge in $$tmpdir"; \
	mkdir -p $$tmpdir/var/lib/jed $$tmpdir/var/log/jed; \
	touch $$tmpdir/var/lib/jed/foo $$tmpdir/var/log/jed/log; \
	ls -la $$tmpdir/var || true; \
	JED_TEST_ROOT=$$tmpdir debian/postrm purge; \
	echo "After purge:"; ls -la $$tmpdir/var || true; \
	rm -rf $$tmpdir; \


.PHONY: all install install-man uninstall clean
