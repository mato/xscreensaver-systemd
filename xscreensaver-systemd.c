#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <stdio.h>
#include <stdlib.h>

GMainLoop *loop = NULL;

void handle_signal_prepare_for_sleep(DBusMessage *msg)
{
    DBusError error;
    dbus_error_init(&error);
 
    int arg = 0;
 
    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_BOOLEAN, &arg,
            DBUS_TYPE_INVALID)) {
        g_error("Cannot read PrepareForSleep message, cause: %s", error.message);
        dbus_error_free(&error);
        return;
    }
    g_message("Signal received. Arg: %d", arg);
    if (arg) { /* true: Before sleep, false: After sleep */
        GError *error = NULL;
        gchar *argv[] = { "/usr/bin/xscreensaver-command", "-lock", NULL };
        if (!g_spawn_sync(NULL, argv, NULL,
                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, NULL, NULL, &error)) {
            g_error("Spawn failed: %s", error->message);
            g_error_free(error);
        }
        g_main_loop_quit(loop);
    }
}

DBusHandlerResult signal_filter(DBusConnection *connection, DBusMessage *msg,
        void *user_data)
{
    if (dbus_message_is_signal(msg, "org.freedesktop.login1.Manager",
            "PrepareForSleep")) {
        handle_signal_prepare_for_sleep(msg);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
 
int main()
{
    loop = g_main_loop_new(NULL, FALSE);
    DBusError error;
 
    dbus_error_init(&error);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
 
    if (dbus_error_is_set(&error)) {
        g_error("Cannot get System BUS connection: %s", error.message);
        dbus_error_free(&error);
        return EXIT_FAILURE;
    }
    dbus_connection_setup_with_g_main(conn, NULL);
 
    char *rule = "type='signal',interface='org.freedesktop.login1.Manager'";
    g_message("Signal match rule: %s", rule);
    dbus_bus_add_match(conn, rule, &error);
 
    if (dbus_error_is_set(&error)) {
        g_error("Cannot add D-BUS match rule, cause: %s", error.message);
        dbus_error_free(&error);
        return EXIT_FAILURE;
    }
 
    g_message("Listening to D-BUS signals using a connection filter");
    dbus_connection_add_filter(conn, signal_filter, NULL, NULL);
 
    g_main_loop_run(loop);
 
    return EXIT_SUCCESS; 
}
