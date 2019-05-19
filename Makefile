.PHONY: all clean

all: xscreensaver-systemd

clean:
	$(RM) xscreensaver-systemd

CFLAGS += -O2 -Wall -std=c99
CFLAGS += $(shell pkg-config libsystemd --cflags)
LDLIBS += $(shell pkg-config libsystemd --libs)
