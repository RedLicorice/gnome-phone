/* The conversation list (spec §1): ConversationListActivity, and with @archived ArchivedConversationListActivity.
 *
 * A row for each conversation, newest first: its avatar, its people, the newest message (or the draft) and when it
 * was. A tap opens it; a long press selects it, and the action bar then archives or deletes the selected ones. A
 * swipe to either side archives one, with Undo. The FAB starts a new conversation; the overflow opens the
 * archived ones. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_CONVERSATION_LIST (conversation_list_get_type ())
G_DECLARE_FINAL_TYPE (ConversationList, conversation_list, MSG, CONVERSATION_LIST, GtkWidget)

GtkWidget *conversation_list_new (SmsClient *client, Contacts *contacts, gboolean archived);
/* ForwardMessageActivity's list (§2.5 Forward): the conversations that are not archived, without the action bar, the
 * FAB, swiping or select mode; a tap or a long press picks one ("open-conversation") */
GtkWidget *conversation_list_new_forward (SmsClient *client, Contacts *contacts);
/* scrolled to its top, with nothing selected: what comes is then seen (SetVisible 0) */
gboolean   conversation_list_is_at_top (ConversationList *self);
/* the group the conversation is listed under (SMS, a provider's chats) is the one shown: a notification's tap and the
 * Back from it land where the conversation is */
void       conversation_list_show_group_of (ConversationList *self, gint64 id);
/* leaves select mode; FALSE when it was not in it (Back then closes the screen) */
gboolean   conversation_list_back (ConversationList *self);

/* Signals: "open-conversation" (gint64), "start-chat" (), "show-archived" (), "show-blocked" () (the overflow's
 * "Blocked contacts"), "show-settings" (), "closed" () (the up arrow), "at-top-changed" () */

G_END_DECLS
