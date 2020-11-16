/* Xscreensaver-systemd, Copyright (c) 2019 Martin Lucina <martin@lucina.net>
 *
 * ISC License
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * This is a small utility providing systemd integration for XScreenSaver.
 *
 * When run from ~/.xsession or equivalent, this will:
 *
 *   - Lock the screen before the system goes to sleep (using
 *     xscreensaver-command -suspend).
 *
 *   - Ensure the XScreenSaver password dialog is shown after the system
 *     is resumed (using xscreensaver-command -deactivate).
 *
 * This is implemented using the recommended way to do these things
 * nowadays, namely inhibitor locks. sd-bus is used for DBUS communication,
 * so the only dependency is libsystemd (which you already have if you
 * want this).
 *
 * https://github.com/mato/xscreensaver-systemd
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
# include <systemd/sd-bus.h>

#else   /* !HAVE_LIBSYSTEMD */

 /* This is a testing shim so that I can make sure this compiles.
    I don't have access to any machine with libsystemd >= 221.
    Only 219 is available on CentOS 7.7...
  */
 typedef struct sd_bus sd_bus;
 typedef struct sd_bus_message sd_bus_message;
 typedef struct sd_bus_slot sd_bus_slot;
 typedef struct { char *message; } sd_bus_error;
 typedef int (*sd_bus_message_handler_t)
   (sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
 #define SD_BUS_ERROR_NULL { 0 }
 static int sd_bus_message_read (sd_bus_message *m, char *types, ...)
   { return -1; }
 static sd_bus_message *sd_bus_message_unref(sd_bus_message *m) { return 0; }
 static void sd_bus_error_free(sd_bus_error *e) { }
 static int sd_bus_call_method(sd_bus *bus, const char *destination,
                               const char *path, const char *interface,
                               const char *member, sd_bus_error *ret_error,
                               sd_bus_message **reply, const char *types, ...)
   { return -1; }
 static int sd_bus_open_user(sd_bus **ret) { return -1; }
 static int sd_bus_request_name(sd_bus *bus, const char *name, uint64_t flags)
   { return -1; }
 static int sd_bus_open_system(sd_bus **ret) { return -1; }
 static int sd_bus_add_match(sd_bus *bus, sd_bus_slot **slot,
                             const char *match,
                             sd_bus_message_handler_t callback, void *userdata)
   { return -1; }
 static int sd_bus_process(sd_bus *bus, sd_bus_message **r) { return -1; }
 static int sd_bus_wait(sd_bus *bus, uint64_t timeout_usec) { return -1; }
 static sd_bus *sd_bus_flush_close_unref(sd_bus *bus) { return 0; }
 static sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *slot) { return 0; }
 static void sd_bus_message_ref(sd_bus_message *r) { }
#endif /* !HAVE_LIBSYSTEMD */


#include "version.h"

static char *progname;
static char *screensaver_version;
static int verbose_p = 0;

#define DBUS_CLIENT_NAME  "org.jwz.XScreenSaver"
#define DBUS_SERVICE_NAME "org.freedesktop.login1"
#define DBUS_OBJECT_PATH  "/org/freedesktop/login1"
#define DBUS_INTERFACE    "org.freedesktop.login1.Manager"
#define DBUS_METHOD       "Inhibit"
#define DBUS_METHOD_ARGS  "ssss"
#define DBUS_METHOD_WHAT  "sleep"
#define DBUS_METHOD_WHO   "xscreensaver"
#define DBUS_METHOD_WHY   "lock screen on suspend"
#define DBUS_METHOD_MODE  "delay"

#define DBUS_MATCH "type='signal'," \
                   "interface='" DBUS_INTERFACE "'," \
                   "member='PrepareForSleep'"

struct handler_ctx {
  sd_bus *bus;
  sd_bus_message *lock_message;
  int lock_fd;
};

static struct handler_ctx global_ctx = { NULL, NULL, -1 };


static void
xscreensaver_command (const char *cmd)
{
  char buf[1024];
  int rc;
  sprintf (buf, "xscreensaver-command %.100s -%.100s",
           (verbose_p ? "-verbose" : "-quiet"),
           cmd);
  if (verbose_p)
    warnx ("exec: %s", buf);
  rc = system (buf);
  if (rc == -1)
    warnx ("exec failed: %s", buf);
  else if (WEXITSTATUS(rc) != 0)
    warnx ("exec: \"%s\" exited with status %d", buf, WEXITSTATUS(rc));
}


static int
xscreensaver_register_sleep_lock (struct handler_ctx *ctx)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int fd = -1;
  int rc = sd_bus_call_method (ctx->bus,
                               DBUS_SERVICE_NAME, DBUS_OBJECT_PATH,
                               DBUS_INTERFACE, DBUS_METHOD, &error, &reply,
                               DBUS_METHOD_ARGS,
                               DBUS_METHOD_WHAT, DBUS_METHOD_WHO,
                               DBUS_METHOD_WHY, DBUS_METHOD_MODE);
  if (rc < 0)
    {
      warnx ("dbus: inhibit sleep failed: %s", error.message);
      goto DONE;
    }

  /* Save the lock fd and explicitly take a ref to the lock message. */
  rc = sd_bus_message_read (reply, "h", &fd);
  if (rc < 0 || fd < 0)
    {
      warnx ("dbus: inhibit sleep failed: no lock fd: %s", strerror(-rc));
      goto DONE;
    }
  sd_bus_message_ref(reply);
  ctx->lock_message = reply;
  ctx->lock_fd = fd;

 DONE:
  sd_bus_error_free (&error);

  return rc;
}


/* Called when DBUS_INTERFACE sends a "PrepareForSleep" signal.
   The event is sent twice: before sleep, and after.
 */
static int
xscreensaver_systemd_handler (sd_bus_message *m, void *arg,
                              sd_bus_error *ret_error)
{
  struct handler_ctx *ctx = arg;
  int before_sleep;
  int rc;

  rc = sd_bus_message_read (m, "b", &before_sleep);
  if (rc < 0)
    {
      warnx ("dbus: message read failed: %s", strerror(-rc));
      return 1;  /* >= 0 means success */
    }

  /* Use the scheme described at
     https://www.freedesktop.org/wiki/Software/systemd/inhibit/
     under "Taking Delay Locks".
   */
  if (before_sleep)
    {
      /* Tell xscreensaver that we are suspending, and to lock if desired. */
      xscreensaver_command ("suspend");

      if (ctx->lock_message)
        {
          /* Release the lock, meaning we are done and it's ok to sleep now.
             Don't rely on unref'ing the message to close the fd, do that
             explicitly here.
           */
          close(ctx->lock_fd);
          sd_bus_message_unref (ctx->lock_message);
          ctx->lock_message = NULL;
          ctx->lock_fd = -1;
        }
      else
        {
          warnx ("dbus: no context lock");
        }
    }
  else
    {
      /* Tell xscreensaver to present the unlock dialog right now. */
      xscreensaver_command ("deactivate");

      /* We woke from sleep, so we need to re-register for the next sleep. */
      rc = xscreensaver_register_sleep_lock (ctx);
      if (rc < 0)
        warnx("could not re-register sleep lock");
    }

  return 1;  /* >= 0 means success */
}


static int
xscreensaver_systemd_loop (void)
{
  sd_bus *bus = NULL, *user_bus = NULL;
  sd_bus_slot *slot = NULL;
  struct handler_ctx *ctx = &global_ctx;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int rc;

  rc = sd_bus_open_user (&user_bus);
  if (rc < 0) {
    warnx ("dbus: connection failed: %s", strerror(-rc));
    goto FAIL;
  }

  rc = sd_bus_request_name (user_bus, DBUS_CLIENT_NAME, 0);
  if (rc < 0)
    {
      warnx ("dbus: failed to connect as %s: %s", 
             DBUS_CLIENT_NAME, strerror(-rc));
      goto FAIL;
    }

  rc = sd_bus_open_system (&bus);
  if (rc < 0)
    {
      warnx ("dbus: open failed: %s", strerror(-rc));
      goto FAIL;
    }

  /* Obtain a lock fd from the "Inhibit" method, so that we can delay
     sleep when a "PrepareForSleep" signal is posted. */

  ctx->bus = bus;
  rc = xscreensaver_register_sleep_lock (ctx);
  if (rc < 0)
    goto FAIL;


  /* This is basically an event mask, saying that we are interested in
     "PrepareForSleep", and to run our callback when that signal is thrown.
   */
  rc = sd_bus_add_match (bus, &slot, DBUS_MATCH,
                         xscreensaver_systemd_handler,
                         &global_ctx);
  if (rc < 0)
    {
      warnx ("dbus: add match failed: %s", strerror(-rc));
      goto FAIL;
    }

  /* Run an event loop forever, and wait for our callback to run.
   */
  while (1)
    {
      rc = sd_bus_process (bus, NULL);
      if (rc < 0)
        {
          warnx ("dbus: process failed: %s", strerror(-rc));
          goto FAIL;
        }

      if (rc > 0)  /* Processed a request, try to do another one right away. */
        continue;

      /* Wait for the next request to process */
      rc = sd_bus_wait (bus, (uint64_t) -1);
      if (rc < 0)
        {
          warnx ("dbus: wait failed: %s", strerror(-rc));
          goto FAIL;
        }
    }

 FAIL:
  if (slot)
    sd_bus_slot_unref (slot);

  if (bus)
    sd_bus_flush_close_unref (bus);

  if (user_bus)
    sd_bus_flush_close_unref (user_bus);

  sd_bus_error_free (&error);

  return EXIT_FAILURE;
}


static char *usage = "\n\
usage: %s [-verbose]\n\
\n\
This program is launched by the xscreensaver daemon to monitor DBus.\n\
It invokes 'xscreensaver-command' to tell the xscreensaver daemon to lock\n\
the screen before the system suspends, e.g., when a laptop's lid is closed.\n\
\n\
From XScreenSaver %s, (c) 1991-%s Jamie Zawinski <jwz@jwz.org>.\n";


#define USAGE() do { \
 fprintf (stderr, usage, progname, screensaver_version, year); exit (1); \
 } while(0)


int
main (int argc, char **argv)
{
  int i;
  char *s;
  char year[5];

  progname = argv[0];
  s = strrchr (progname, '/');
  if (s) progname = s+1;

  screensaver_version = (char *) malloc (5);
  memcpy (screensaver_version, screensaver_id + 17, 4);
  screensaver_version [4] = 0;

  s = strchr (screensaver_id, '-');
  s = strrchr (s, '-');
  s++;
  strncpy (year, s, 4);
  year[4] = 0;

  for (i = 1; i < argc; i++)
    {
      const char *s = argv [i];
      int L;
      if (s[0] == '-' && s[1] == '-') s++;
      L = strlen (s);
      if (L < 2) USAGE ();
      else if (!strncmp (s, "-verbose", L)) verbose_p = 1;
      else if (!strncmp (s, "-quiet",   L)) verbose_p = 0;
      else USAGE ();
    }

  exit (xscreensaver_systemd_loop());
}
