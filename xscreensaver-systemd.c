#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

struct handler_ctx {
    sd_bus *bus;
    sd_bus_message *lock;
};
static struct handler_ctx global_ctx = { .bus = NULL, .lock = NULL };

static int handler(sd_bus_message *m, void *arg,
        sd_bus_error *ret_error)
{
    struct handler_ctx *ctx = arg;
    int before_sleep;
    int rc;

    rc = sd_bus_message_read(m, "b", &before_sleep);
    if (rc < 0) {
        warnx("Failed to read message: %s", strerror(-rc));
        return 0;
    }

    /* Use the scheme described at
     * https://www.freedesktop.org/wiki/Software/systemd/inhibit/
     * under "Taking Delay Locks".
     */
    if (before_sleep) {
        rc = system("xscreensaver-command -lock");
        if (rc == -1) {
            warnx("Failed to run xscreensaver-command");
        }
        else if (WEXITSTATUS(rc) != 0) {
            warnx("xscreensaver-command failed with %d", WEXITSTATUS(rc));
        }
        else {
            sleep(1);
        }

        if (ctx->lock) {
            sd_bus_message_unref(ctx->lock);
            ctx->lock = NULL;
        }
        else {
            warnx("Warning: ctx->lock is NULL, this should not happen?");
        }
    }
    else {
        rc = system("xset dpms force on");
        if (rc == -1) {
            warnx("Failed to run xset");
        }
        else if (WEXITSTATUS(rc) != 0) {
            warnx("xset failed with %d", WEXITSTATUS(rc));
        }

        rc = system("/usr/bin/xscreensaver-command -deactivate");
        if (rc == -1) {
            warnx("Failed to run xscreensaver-command");
        }
        else if (WEXITSTATUS(rc) != 0) {
            warnx("xscreensaver-command exited with %d", WEXITSTATUS(rc));
        }

        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        rc = sd_bus_call_method(ctx->bus,
                "org.freedesktop.login1",
                "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager",
                "Inhibit",
                &error,
                &reply,
                "ssss",
                "sleep",
                "xscreensaver",
                "lock screen on suspend",
                "delay");
        if (rc < 0) {
            warnx("Failed to call Inhibit(): %s", error.message);
            goto out;
        }
        int fd;
        rc = sd_bus_message_read(reply, "h", &fd);
        if (rc < 0) {
            warnx("Failed to read message: %s", strerror(-rc));
            goto out;
        }
        assert(fd >= 0);
        ctx->lock = reply;

out:
        sd_bus_error_free(&error);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    struct handler_ctx *ctx = &global_ctx;

    int rc = sd_bus_open_system(&bus);
    if (rc < 0) {
        warnx("Failed to connect to system bus: %s", strerror(-rc));
        goto out;
    }
    ctx->bus = bus;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    rc = sd_bus_call_method(bus,
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "Inhibit",
            &error,
            &reply,
            "ssss",
            "sleep",
            "xscreensaver",
            "lock screen on suspend",
            "delay");
    if (rc < 0) {
        warnx("Failed to call Inhibit(): %s", error.message);
        goto out;
    }
    int fd;
    rc = sd_bus_message_read(reply, "h", &fd);
    if (rc < 0) {
        warnx("Failed to read message: %s", strerror(-rc));
        goto out;
    }
    assert(fd >= 0);
    ctx->lock = reply;

    const char *match =
        "type='signal',interface='org.freedesktop.login1.Manager'"
        ",member='PrepareForSleep'";
    rc = sd_bus_add_match(bus, &slot, match, handler, &global_ctx);
    if (rc < 0) {
        warnx("Failed to add match: %s", strerror(-rc));
        goto out;
    }

    for (;;) {
        rc = sd_bus_process(bus, NULL);
        if (rc < 0) {
            warnx("Failed to process bus: %s", strerror(-rc));
            goto out;
        }
        if (rc > 0)
            /* we processed a request, try to process another one, right-away */
            continue;

        /* Wait for the next request to process */
        rc = sd_bus_wait(bus, (uint64_t) -1);
        if (rc < 0) {
            warnx("Failed to wait on bus: %s", strerror(-rc));
            goto out;
        }
    }

out:
    sd_bus_message_unref(reply);
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    sd_bus_error_free(&error);

    return (rc < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
