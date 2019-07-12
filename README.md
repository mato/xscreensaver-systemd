**xscreensaver-systemd is now integrated in [XcreenSaver 5.43](https://www.jwz.org/blog/2019/07/xscreensaver-5-43/)**. You probably want to use the version there, and get the latest XScreenSaver goodness while you're at it.

If you're desperate for a version that works (sort of) with an older XScreenSaver, see the [old](tree/old) branch.

----

This is a small utility providing systemd integration for XScreenSaver.

When run from `~/.xsession` or equivalent, this will:

1. Lock the screen _before_ the system goes to sleep (using `xscreensaver-command -lock`).
2. Ensure the XScreenSaver password dialog is shown _after_ the system is resumed (using `xset` to force the screen to power on followed by `xscreensaver-command -deactivate`).

This is implemented using the recommended way to do these things nowadays, namely [inhibitor locks](https://www.freedesktop.org/wiki/Software/systemd/inhibit/). [sd-bus](http://0pointer.net/blog/the-new-sd-bus-api-of-systemd.html) is used for DBUS communication, so the only dependency is `libsystemd` (which you already have if you want this).
