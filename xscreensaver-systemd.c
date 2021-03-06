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
 *
 * This utility provides systemd integration for XScreenSaver.
 * It does two things:
 *
 *   - When the system is about to go to sleep (e.g., laptop lid closing)
 *     it locks the screen *before* the system goes to sleep, by running
 *     "xscreensaver-command -suspend".  And then when the system wakes
 *     up again, it runs "xscreensaver-command -deactivate" to force the
 *     unlock dialog to appear immediately.
 *
 *   - When another process on the system makes asks for the screen saver
 *     to be inhibited (e.g. because a video is playing) this program
 *     periodically runs "xscreensaver-command -deactivate" to keep the
 *     display un-blanked.  It does this until the other program asks for
 *     it to stop.
 *
 *
 * BACKGROUND:
 *
 *   For decades, the traditional way for a video player to temporarily
 *   inhibit the screen saver was to have a heartbeat command that ran
 *   "xscreensaver-command -deactivate" once a minute while the video
 *   was playing, and ceased when the video was paused or stopped.  The
 *   reason to do it as a heartbeat rather than a toggle is so that the
 *   player fails SAFE -- if the player exits abnormally, the heart
 *   stops beating, and screen saving and locking resumes.
 *
 *
 *   - MPlayer and MPV:
 *
 *     Some time in 2018 or 2019, the authors of MPlayer and MPV decided to
 *     remove this functionality because... they're idiots?  They don't run
 *     screen savers?  They don't understand how any of this works?  I can
 *     only speculate.
 *
 *     Currently (late 2020) MPlayer and MPV call only XResetScreenSaver()
 *     as their heartbeat.  But that only affects the X11 server's built-in
 *     screen saver, not a userspace screen locker like xscreensaver.
 *
 *     They also call XScreenSaverSuspend() which is part of the MIT
 *     SCREEN-SAVER server extension.  XScreenSaver does make use of that
 *     extension because it is worse than useless.  See commentary atop
 *     xscreensaver.c for details.
 *
 *
 *   - VLC:
 *
 *     In some circumstances, VLC will send "inhibit" messages to one of
 *     these DBUS targets: "org.freedesktop.ScreenSaver",
 *     "org.freedesktop.PowerManagement.Inhibit", "org.mate.SessionManager",
 *     and/or "org.gnome.SessionManager".
 *
 *     In some other circumstances, it will run "xdg-screensaver reset" as
 *     a heartbeat.  That is a shell script that tries to figure out which
 *     desktop environment is being used, and will eventually run
 *     "xscreensaver-command -deactivate".
 *
 *     I can't tell how VLC decides which of these methods to use.
 *
 *
 *   - Firefox:
 *
 *     When playing media, Firefox will send "inhibit" to one of these
 *     targets: "org.freedesktop.ScreenSaver", or "org.gnome.SessionManager".
 *
 *     However, Firefox decides which, if any, of those to use at launch time,
 *     and does not revisit that decision.  So if xscreensaver-systemd has not
 *     been launched before Firefox, it won't work.  Fortunately, in most use
 *     cases, xscreensaver will have been launched earlier in the startup
 *     sequence than the web browser.
 *
 *     Also, firefox sends an "inhibit" message when it is merely playing
 *     audio.  That's horrible.  We should ignore those messages and only
 *     inhibit when video is playing.
 *
 *
 *   - Chrome:
 *
 *     As for Firefox, but Chrome calls the "org.freedesktop.ScreenSaver"
 *     target with an object path of "/org/freedesktop/ScreenSaver". Unlike
 *     Firefox, Chrome does not send an "inhibit" message when only audio is
 *     playing.
 *
 *
 * TO DO:
 *
 *   - Currently this code is only listening to "org.freedesktop.ScreenSaver".
 *     It should listen to all the others too, because why not.
 *
 *   - What happens if Firefox is playing a video, and has requested to
 *     inhibit the saver, and then is killed with -9?  Do we get a signal to
 *     uninhibit, or is it back to not failing safe?
 *
 *     If it's doing the shitty thing, can we get the pid of the process on
 *     the other end of the "inhibit" request and notice when it goes away?
 *
 *   - xscreensaver_get_cookie() can theoretically return duplicates, we
 *     should handle that?
 *
 *   - xscreensaver_method_uninhibit() does not actually send a reply, are
 *     we doing the right thing when registering it?
 *
 *   - run under valgrind, etc. to check for any memory leaks.
 *
 *   - call sd_bus_release_name() explicitly on exit?
 *
 * TESTING:
 *
 *   To call the D-BUS methods manually, you can use "busctl":
 *
 *   busctl --user call org.freedesktop.ScreenSaver \
 *     /ScreenSaver org.freedesktop.ScreenSaver \
 *     Inhibit ss test-application test-reason
 *
 *   This will hand out a cookie, which you can pass back to UnInhibit:
 *
 *   u 1792821391
 *
 *   busctl --user call org.freedesktop.ScreenSaver \
 *     /ScreenSaver org.freedesktop.ScreenSaver \
 *     UnInhibit u 1792821391
 *
 * https://github.com/mato/xscreensaver-systemd
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

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
 static sd_bus *sd_bus_flush_close_unref(sd_bus *bus) { return 0; }
 static sd_bus_slot *sd_bus_slot_unref(sd_bus_slot *slot) { return 0; }
 static void sd_bus_message_ref(sd_bus_message *r) { }
 static int sd_bus_reply_method_return (sd_bus_message *call,
                                        const char *types, ...) { return -1; }
 struct sd_bus_vtable { int x; };
 typedef struct sd_bus_vtable sd_bus_vtable;
# define SD_BUS_VTABLE_START(_flags) { 0 }
# define SD_BUS_VTABLE_END /**/
# define SD_BUS_METHOD(_member, _signature, _result, _handler, _flags) { 0 }
 static int sd_bus_add_object_vtable(sd_bus *bus, sd_bus_slot **slot,
                                     const char *path, const char *interface,
                                     const sd_bus_vtable *vtable,
                                     void *userdata) { return -1; }
 static void *sd_bus_slot_set_userdata(sd_bus_slot *s, void *d) { return 0; }
 static int sd_bus_get_fd(sd_bus *bus) { return -1; }
 static int sd_bus_get_events(sd_bus *bus) { return -1; }
 static int sd_bus_get_timeout (sd_bus *bus, uint64_t *u) { return -1; }

#endif /* !HAVE_LIBSYSTEMD */

#include "queue.h"
#include "version.h"

static char *progname;
static char *screensaver_version;
static int verbose_p = 0;

#define DBUS_CLIENT_NAME     "org.jwz.XScreenSaver"
#define DBUS_SD_SERVICE_NAME "org.freedesktop.login1"
#define DBUS_SD_OBJECT_PATH  "/org/freedesktop/login1"
#define DBUS_SD_INTERFACE    "org.freedesktop.login1.Manager"
#define DBUS_SD_METHOD       "Inhibit"
#define DBUS_SD_METHOD_ARGS  "ssss"
#define DBUS_SD_METHOD_WHAT  "sleep"
#define DBUS_SD_METHOD_WHO   "xscreensaver"
#define DBUS_SD_METHOD_WHY   "lock screen on suspend"
#define DBUS_SD_METHOD_MODE  "delay"

#define DBUS_SD_MATCH "type='signal'," \
                      "interface='" DBUS_SD_INTERFACE "'," \
                      "member='PrepareForSleep'"

#define DBUS_FDO_NAME          "org.freedesktop.ScreenSaver"
#define DBUS_FDO_OBJECT_PATH   "/ScreenSaver"
#define DBUS_FDO_OBJECT_PATH_2 "/org/freedesktop/ScreenSaver"
#define DBUS_FDO_INTERFACE     "org.freedesktop.ScreenSaver"

struct handler_ctx {
  sd_bus *system_bus;
  sd_bus_message *lock_message;
  int lock_fd;
  int is_inhibited;
};

static struct handler_ctx global_ctx = { NULL, NULL, -1 };

SLIST_HEAD(inhibit_head, inhibit_entry) inhibit_head =
  SLIST_HEAD_INITIALIZER(inhibit_head);

struct inhibit_entry {
  uint32_t cookie;
  SLIST_ENTRY(inhibit_entry) entries;
};

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
  int rc = sd_bus_call_method (ctx->system_bus,
                               DBUS_SD_SERVICE_NAME, DBUS_SD_OBJECT_PATH,
                               DBUS_SD_INTERFACE, DBUS_SD_METHOD,
                               &error, &reply,
                               DBUS_SD_METHOD_ARGS,
                               DBUS_SD_METHOD_WHAT, DBUS_SD_METHOD_WHO,
                               DBUS_SD_METHOD_WHY, DBUS_SD_METHOD_MODE);
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


/* Called when DBUS_SD_INTERFACE sends a "PrepareForSleep" signal.
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

static uint32_t
xscreensaver_get_cookie(void)
{
    uint32_t cookie = 0;
    static int use_rand48 = 0;
    int rc;

    if (use_rand48)
      {
        cookie = lrand48();
      }
    else
      {
        /* According to the manpage may fail with ENOSYS on some kernels,
           so just fall back to lrand48() if that (or any other failure)
           happens. */
        rc = getentropy(&cookie, sizeof cookie);
        if (rc != 0)
          {
            warn("getentropy() failed, falling back to lrand48()");
            srand48(time(NULL));
            use_rand48 = 1;
            cookie = lrand48();
          }
      }
    return cookie;
}

static int
xscreensaver_method_inhibit(sd_bus_message *m, void *arg,
                            sd_bus_error *ret_error)
{
    struct handler_ctx *ctx = arg;
    char *application_name, *inhibit_reason;
    struct inhibit_entry *entry;

    int rc = sd_bus_message_read(m, "ss", &application_name, &inhibit_reason);
    if (rc < 0) {
        warnx("Failed to parse method call: %s", strerror(-rc));
        return rc;
    }

    entry = malloc(sizeof (struct inhibit_entry));
    entry->cookie = xscreensaver_get_cookie();
    SLIST_INSERT_HEAD(&inhibit_head, entry, entries);
    ctx->is_inhibited++;
    if (verbose_p)
      warnx("Inhibit() called: Application: '%s': Reason: '%s' -> returning %u",
          application_name,
          inhibit_reason,
          entry->cookie);

    return sd_bus_reply_method_return(m, "u", entry->cookie);
}

static int
xscreensaver_method_uninhibit(sd_bus_message *m, void *arg,
                              sd_bus_error *ret_error)
{
    struct handler_ctx *ctx = arg;
    uint32_t cookie;
    struct inhibit_entry *entry;
    int found = 0;

    int rc = sd_bus_message_read(m, "u", &cookie);
    if (rc < 0) {
        warnx("Failed to parse method call: %s", strerror(-rc));
        return rc;
    }

    SLIST_FOREACH(entry, &inhibit_head, entries)
      {
        if (entry->cookie == cookie)
          {
            SLIST_REMOVE(&inhibit_head, entry, inhibit_entry, entries);
            free(entry);
            ctx->is_inhibited--;
            if (ctx->is_inhibited < 0)
              ctx->is_inhibited = 0;
            found = 1;
            break;
          }
      }
    if (verbose_p)
      warnx("UnInhibit() called: Cookie: %u%s",
          cookie,
          found ? ": Removed" : ": Not found, ignored");

    return sd_bus_reply_method_return(m, "");
}

/*
 * This vtable defines the service interface we implement.
 */
static const sd_bus_vtable
xscreensaver_dbus_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ss", "u", xscreensaver_method_inhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnInhibit", "u", "", xscreensaver_method_uninhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};


static int
xscreensaver_systemd_loop (void)
{
  sd_bus *system_bus = NULL, *user_bus = NULL;
  struct handler_ctx *ctx = &global_ctx;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int rc;
  time_t last_deactivate_time = 0, now;

  /* 'user_bus' is where we receive messages from other programs sending
     inhibit/uninhibit to org.freedesktop.ScreenSaver, etc.
   */

  rc = sd_bus_open_user (&user_bus);
  if (rc < 0) {
    warnx ("dbus: connection failed: %s", strerror(-rc));
    goto FAIL;
  }

  rc = sd_bus_add_object_vtable(user_bus,
                                NULL,
                                DBUS_FDO_OBJECT_PATH,
                                DBUS_FDO_INTERFACE,
                                xscreensaver_dbus_vtable,
                                &global_ctx);
  if (rc < 0) {
    warnx("dbus: vtable registration failed: %s", strerror(-rc));
    goto FAIL;
  }

  rc = sd_bus_add_object_vtable(user_bus,
                                NULL,
                                DBUS_FDO_OBJECT_PATH_2,
                                DBUS_FDO_INTERFACE,
                                xscreensaver_dbus_vtable,
                                &global_ctx);
  if (rc < 0) {
    warnx("dbus: vtable registration failed: %s", strerror(-rc));
    goto FAIL;
  }

  rc = sd_bus_request_name(user_bus, DBUS_FDO_NAME, 0);
  if (rc < 0)
    {
      warnx ("dbus: failed to connect as %s: %s",
             DBUS_FDO_NAME, strerror(-rc));
      goto FAIL;
    }

  rc = sd_bus_request_name (user_bus, DBUS_CLIENT_NAME, 0);
  if (rc < 0)
    {
      warnx ("dbus: failed to connect as %s: %s",
             DBUS_CLIENT_NAME, strerror(-rc));
      goto FAIL;
    }


  /* 'system_bus' is where we hold a lock on org.freedesktop.login1, meaning
     that the system will send us a PrepareForSleep message when the system is
     about to suspend.
   */

  rc = sd_bus_open_system (&system_bus);
  if (rc < 0)
    {
      warnx ("dbus: open failed: %s", strerror(-rc));
      goto FAIL;
    }

  /* Obtain a lock fd from the "Inhibit" method, so that we can delay
     sleep when a "PrepareForSleep" signal is posted. */

  ctx->system_bus = system_bus;
  rc = xscreensaver_register_sleep_lock (ctx);
  if (rc < 0)
    goto FAIL;


  /* This is basically an event mask, saying that we are interested in
     "PrepareForSleep", and to run our callback when that signal is thrown.
   */
  rc = sd_bus_add_match (system_bus, NULL, DBUS_SD_MATCH,
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
      struct pollfd fds[2];
      uint64_t poll_timeout, timeout, user_timeout;

      /*
       * We MUST call sd_bus_process() on each bus at least once before calling
       * sd_bus_get_events(), so just always start the event loop by processing
       * all outstanding requests on both busses.
       */
      do
        {
          rc = sd_bus_process(system_bus, NULL);
          if (rc < 0)
            {
               warnx("Failed to process bus: %s", strerror(-rc));
               goto FAIL;
            }
        }
      while (rc > 0);

      do
        {
          rc = sd_bus_process(user_bus, NULL);
          if (rc < 0)
            {
               warnx("Failed to process bus: %s", strerror(-rc));
               goto FAIL;
            }
        }
      while (rc > 0);

      fds[0].fd = sd_bus_get_fd(system_bus);
      fds[0].events = sd_bus_get_events(system_bus);
      fds[0].revents = 0;
      fds[1].fd = sd_bus_get_fd(user_bus);
      fds[1].events = sd_bus_get_events(user_bus);
      fds[1].revents = 0;

      sd_bus_get_timeout(system_bus, &timeout);
      sd_bus_get_timeout(user_bus, &user_timeout);
      if (timeout == 0 && user_timeout == 0)
        poll_timeout = 0;
      else if (timeout == UINT64_MAX && user_timeout == UINT64_MAX)
        poll_timeout = -1;
      else
        {
          poll_timeout = (timeout < user_timeout) ? timeout : user_timeout;
          poll_timeout /= 1000000;
        }

      /*
         We want to wake up at least once every 50 seconds, to de-activate
         the screensaver if we have been inhibited.
       */
      if (poll_timeout > 50000)
        poll_timeout = 50000;

      rc = poll(fds, 2, poll_timeout);
      if (rc < 0)
        err(EXIT_FAILURE, "poll()");

      if (ctx->is_inhibited)
        {
          now = time(NULL);
          if (now - last_deactivate_time >= 50)
            {
              if (verbose_p)
                warnx("%d active inhibitors, deactivating screensaver",
                    ctx->is_inhibited);
              xscreensaver_command("deactivate");
              last_deactivate_time = now;
            }
        }
    }

 FAIL:
  if (system_bus)
    sd_bus_flush_close_unref (system_bus);

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
It also responds to certain messages sent by media players allowing them to\n\
request that the screen not be blanked during playback.\n\
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
