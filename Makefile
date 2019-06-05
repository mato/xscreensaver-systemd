.PHONY: all clean

all: xscreensaver-systemd

clean:
	$(RM) xscreensaver-systemd

CFLAGS += -O2 -g -Wall -std=c89 -pedantic
CFLAGS += $(shell pkg-config libsystemd --cflags)
LDLIBS += $(shell pkg-config libsystemd --libs)
