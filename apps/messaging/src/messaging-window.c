/* Messaging's window (messaging-window.h). */
#include "messaging-window.h"
#include "blocked-contacts.h"
#include "contact-picker.h"
#include "contacts.h"
#include "conversation-list.h"
#include "conversation-view.h"
#include "msg-navigator.h"
#include "msg-settings.h"
#include "people-options.h"
#include "sms-client.h"

#include <glib/gi18n.h>
#include <neo.h>

typedef struct
{
  GtkWidget *window;
  SmsClient *client;
  Contacts *contacts;
  GtkWidget *navigator, *list;
  GtkWidget *shown;             /* the conversation view last on top, whose draft is kept when it goes */
} Shell;

static void open_conversation (Shell *shell, gint64 id, const char *draft, gboolean replace);

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), "messaging-shell");
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  /* nothing is on screen any more */
  sms_client_set_visible (shell->client, -1);
  g_signal_handlers_disconnect_by_data (shell->client, shell);
  g_clear_object (&shell->contacts);
  g_clear_object (&shell->client);
  g_clear_object (&shell->shown);
  g_free (shell);
}

/* --- what is on screen (SetVisible) */

static void
update_visible (Shell *shell)
{
  GtkWidget *top = msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator));
  gint64 visible = -1;

  /* only while the window is in front and seen (onResume): a message that comes to a window behind another, or under
   * the lock screen (the compositor suspends it), is still news */
  if (!gtk_window_is_active (GTK_WINDOW (shell->window)) || !gtk_widget_get_mapped (shell->window)
      || gtk_window_is_suspended (GTK_WINDOW (shell->window)))
    visible = -1;
  else if (MSG_IS_CONVERSATION_VIEW (top))
    visible = conversation_view_get_id (MSG_CONVERSATION_VIEW (top));
  else if (top == shell->list && conversation_list_is_at_top (MSG_CONVERSATION_LIST (top)))
    visible = 0;
  sms_client_set_visible (shell->client, visible);
}

static void
keep_draft (Shell *shell)
{
  if (shell->shown)
    conversation_view_save_draft (MSG_CONVERSATION_VIEW (shell->shown));
}

static void
on_top_changed (MsgNavigator *navigator, gpointer data)
{
  Shell *shell = data;
  GtkWidget *top = msg_navigator_get_top (navigator);

  /* the conversation going (or covered) keeps what was typed */
  if (shell->shown != top)
    {
      keep_draft (shell);
      g_clear_object (&shell->shown);
      if (MSG_IS_CONVERSATION_VIEW (top))
        shell->shown = g_object_ref (top);
    }
  gtk_widget_grab_focus (top);
  update_visible (shell);
}

static void
on_active (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  Shell *shell = data;

  /* onPause: the draft is saved when the window goes behind another */
  if (!gtk_window_is_active (window))
    keep_draft (shell);
  update_visible (shell);
}

/* --- going back */

static void
go_back (Shell *shell)
{
  GtkWidget *top = msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator));

  if (MSG_IS_CONVERSATION_VIEW (top) && conversation_view_back (MSG_CONVERSATION_VIEW (top)))
    return;
  if (MSG_IS_CONVERSATION_LIST (top) && conversation_list_back (MSG_CONVERSATION_LIST (top)))
    return;
  if (!msg_navigator_pop (MSG_NAVIGATOR (shell->navigator)))
    gtk_window_close (GTK_WINDOW (shell->window));
}

static void
on_page_closed (GtkWidget *page, gpointer data)
{
  Shell *shell = data;

  /* a page closing itself goes only if it is the one on top (a conversation deleted under the picker stays) */
  if (msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator)) == page)
    msg_navigator_pop (MSG_NAVIGATOR (shell->navigator));
}

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  go_back (data);
  return TRUE;
}

/* --- the pages */

static void
on_open_conversation (GtkWidget *list, gint64 id, gpointer data)
{
  open_conversation (data, id, NULL, FALSE);
}

static void
on_picked (ContactPicker *picker, const char *number, gpointer data)
{
  Shell *shell = data;
  const char *numbers[] = { number, NULL };
  gint64 id = sms_client_get_conversation (shell->client, numbers);

  /* the picker becomes the conversation (ConversationActivity swaps its fragment) */
  if (id > 0)
    open_conversation (shell, id, NULL, TRUE);
}

static void
on_start_chat (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *picker = contact_picker_new (shell->client, shell->contacts);

  g_signal_connect (picker, "picked", G_CALLBACK (on_picked), shell);
  g_signal_connect (picker, "closed", G_CALLBACK (on_page_closed), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), picker);
}

static void
on_list_at_top (GtkWidget *list, gpointer data)
{
  update_visible (data);
}

static GtkWidget *
make_list (Shell *shell, gboolean archived)
{
  GtkWidget *list = conversation_list_new (shell->client, shell->contacts, archived);

  g_signal_connect (list, "open-conversation", G_CALLBACK (on_open_conversation), shell);
  g_signal_connect (list, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (list, "at-top-changed", G_CALLBACK (on_list_at_top), shell);
  return list;
}

static void
on_show_archived (GtkWidget *list, gpointer data)
{
  Shell *shell = data;

  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), make_list (shell, TRUE));
}

static void
on_show_blocked (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = blocked_contacts_new (shell->client, shell->contacts);

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), page);
}

static void
on_show_advanced_settings (GtkWidget *settings, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = msg_settings_new_advanced ();

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), page);
}

static void
on_show_settings (GtkWidget *list, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = msg_settings_new ();

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (page, "show-advanced", G_CALLBACK (on_show_advanced_settings), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), page);
}

/* the person blocked from People & options: the page and its conversation both close */
static void
on_options_blocked (GtkWidget *page, gpointer data)
{
  Shell *shell = data;

  if (msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator)) != page)
    return;
  msg_navigator_pop (MSG_NAVIGATOR (shell->navigator));
  if (MSG_IS_CONVERSATION_VIEW (msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator))))
    msg_navigator_pop (MSG_NAVIGATOR (shell->navigator));
}

static void
on_show_options (ConversationView *view, gpointer data)
{
  Shell *shell = data;
  GtkWidget *page = people_options_new (shell->client, shell->contacts, conversation_view_get_id (view));

  g_signal_connect (page, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (page, "blocked", G_CALLBACK (on_options_blocked), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), page);
}

/* --- Forward (§2.5): ForwardMessageActivity, a dialog over the conversation (BugleTheme.DialogActivity: the window's
 * #181c1d under 28 dp corners, titled "Forward message"; no still of it, so the card is the app's alert dialog's) */

typedef struct
{
  Shell *shell;
  NeoDialog *dialog;
  char *text;
  gboolean picked;
} Forward;

static void
forward_free (gpointer data)
{
  Forward *forward = data;

  g_free (forward->text);
  g_free (forward);
}

static void
on_forward_picked (GtkWidget *list, gint64 id, gpointer data)
{
  Forward *forward = data;
  Shell *shell = forward->shell;
  g_autofree char *text = g_strdup (forward->text);
  GtkWidget *top = msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator));

  /* a second tap while the dialog fades */
  if (forward->picked)
    return;
  forward->picked = TRUE;
  neo_dialog_respond (forward->dialog, NEO_RESPONSE_OK);
  /* launchConversationActivity with FLAG_ACTIVITY_CLEAR_TOP: the conversation picked, with the text as its draft,
   * takes the place of the one it came from, so Back goes to the list */
  open_conversation (shell, id, text, MSG_IS_CONVERSATION_VIEW (top));
}

static void
on_forward (ConversationView *view, const char *text, gpointer data)
{
  Shell *shell = data;
  Forward *forward = g_new0 (Forward, 1);
  GtkWidget *list = conversation_list_new_forward (shell->client, shell->contacts);

  forward->shell = shell;
  forward->text = g_strdup (text);
  forward->dialog = neo_dialog_new (_("Forward message"));
  gtk_widget_add_css_class (GTK_WIDGET (forward->dialog), "msg-forward-dialog");
  g_object_set_data_full (G_OBJECT (forward->dialog), "msg-forward", forward, forward_free);
  g_signal_connect (list, "open-conversation", G_CALLBACK (on_forward_picked), forward);
  neo_dialog_set_content (forward->dialog, list);
  neo_dialog_present (forward->dialog, shell->navigator);
}

static void
open_conversation (Shell *shell, gint64 id, const char *draft, gboolean replace)
{
  GtkWidget *top = msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator));
  GtkWidget *view;

  conversation_list_show_group_of (MSG_CONVERSATION_LIST (shell->list), id);
  if (MSG_IS_CONVERSATION_VIEW (top) && conversation_view_get_id (MSG_CONVERSATION_VIEW (top)) == id)
    {
      if (draft)
        conversation_view_set_text (MSG_CONVERSATION_VIEW (top), draft);
      return;
    }
  view = conversation_view_new (shell->client, shell->contacts, id);
  if (draft)
    conversation_view_set_text (MSG_CONVERSATION_VIEW (view), draft);
  g_signal_connect (view, "closed", G_CALLBACK (on_page_closed), shell);
  g_signal_connect (view, "show-options", G_CALLBACK (on_show_options), shell);
  g_signal_connect (view, "forward", G_CALLBACK (on_forward), shell);
  if (replace)
    msg_navigator_replace (MSG_NAVIGATOR (shell->navigator), view);
  else
    msg_navigator_push (MSG_NAVIGATOR (shell->navigator), view);
}

/* --- from outside */

void
messaging_window_open_conversation (GtkWidget *window, gint64 conversation, const char *draft)
{
  Shell *shell = shell_of (window);

  /* a notification's tap: the conversation, over the list (the back stack Bugle builds for it) */
  if (msg_navigator_get_depth (MSG_NAVIGATOR (shell->navigator)) > 1)
    {
      GtkWidget *top = msg_navigator_get_top (MSG_NAVIGATOR (shell->navigator));

      if (!(MSG_IS_CONVERSATION_VIEW (top) && conversation_view_get_id (MSG_CONVERSATION_VIEW (top)) == conversation))
        msg_navigator_pop_to_root (MSG_NAVIGATOR (shell->navigator));
    }
  open_conversation (shell, conversation, draft, FALSE);
}

void
messaging_window_open_numbers (GtkWidget *window, const char * const *numbers, const char *draft)
{
  Shell *shell = shell_of (window);
  gint64 id = sms_client_get_conversation (shell->client, numbers);

  if (id > 0)
    messaging_window_open_conversation (window, id, draft);
  else
    on_start_chat (shell->list, shell);
}

void
messaging_window_show_conversations (GtkWidget *window)
{
  msg_navigator_pop_to_root (MSG_NAVIGATOR (shell_of (window)->navigator));
}

GtkWidget *
messaging_window_new (GtkApplication *app)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();

  shell->window = window;
  shell->client = sms_client_new ();
  shell->contacts = contacts_new ();
  g_object_set_data_full (G_OBJECT (window), "messaging-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Messaging"));
  gtk_widget_add_css_class (window, "msg-window");

  shell->navigator = msg_navigator_new ();
  shell->list = make_list (shell, FALSE);
  g_signal_connect (shell->list, "start-chat", G_CALLBACK (on_start_chat), shell);
  g_signal_connect (shell->list, "show-archived", G_CALLBACK (on_show_archived), shell);
  g_signal_connect (shell->list, "show-blocked", G_CALLBACK (on_show_blocked), shell);
  g_signal_connect (shell->list, "show-settings", G_CALLBACK (on_show_settings), shell);
  msg_navigator_push (MSG_NAVIGATOR (shell->navigator), shell->list);
  g_signal_connect (shell->navigator, "top-changed", G_CALLBACK (on_top_changed), shell);
  neo_window_set_content (GTK_WINDOW (window), shell->navigator);

  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), shell);
  g_signal_connect_swapped (window, "notify::suspended", G_CALLBACK (update_visible), shell);
  g_signal_connect_swapped (window, "map", G_CALLBACK (update_visible), shell);
  /* Escape is Back (a keyboard's; the shell's gesture presses the page's "Back" button) */
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  return window;
}
