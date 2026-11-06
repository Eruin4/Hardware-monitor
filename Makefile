CC ?= cc
PKGS = gtk+-3.0 libsoup-3.0 json-glib-1.0
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

.PHONY: all check check-syntax check-shell preview clean
all: hardware-dashboard
hardware-dashboard: hardware-dashboard.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
check-syntax:
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only hardware-dashboard.c
check-shell:
	@set -e; for script in scripts/*.sh; do bash -n "$$script"; done
	python3 tests/test_launchers.py
check: check-syntax check-shell hardware-dashboard
	G_DEBUG=fatal-warnings ./hardware-dashboard --self-test
preview: hardware-dashboard
	./hardware-dashboard --preview preview-native.png
clean:
	$(RM) hardware-dashboard
