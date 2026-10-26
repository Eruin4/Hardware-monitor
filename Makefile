CC ?= cc
PKGS = gtk+-3.0 libsoup-3.0 json-glib-1.0
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

.PHONY: all check preview clean
all: hardware-dashboard
hardware-dashboard: hardware-dashboard.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
check: hardware-dashboard
	G_DEBUG=fatal-warnings ./hardware-dashboard --self-test
preview: hardware-dashboard
	./hardware-dashboard --preview preview-native.png
clean:
	$(RM) hardware-dashboard
