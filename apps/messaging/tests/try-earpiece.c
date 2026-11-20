/* On the phone, by tests/try-earpiece.py: msg-earpiece as a playing note uses it, on the bus the script gives it as
 * the system bus (its fake sensor and ModemManager), with the real callaudiod. Prints "stopped" when a call stops
 * the note; ends on SIGTERM, letting the earpiece go. */
#include <glib-unix.h>
#include <stdio.h>

#include "../src/msg-earpiece.h"

static void
on_stop (gpointer data)
{
  printf ("stopped\n");
  fflush (stdout);
}

static gboolean
on_term (gpointer loop)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

int
main (void)
{
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  msg_earpiece_start (G_CALLBACK (on_stop), NULL);
  printf ("playing\n");
  fflush (stdout);
  g_unix_signal_add (SIGTERM, on_term, loop);
  g_main_loop_run (loop);
  msg_earpiece_stop ();
  return 0;
}
