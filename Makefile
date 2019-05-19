.PHONY: all clean

all: xscreensaver-systemd

clean:
	$(RM) xscreensaver-systemd

CFLAGS += -O2 -Wall -std=c99
CFLAGS += $(shell pkg-config glib-2.0 dbus-glib-1 --cflags)
LDLIBS += $(shell pkg-config glib-2.0 dbus-glib-1 --libs)
