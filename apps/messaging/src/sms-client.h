/* The app's side of the SMS service (sms-service.c, de.yesman.Messaging.Sms): what it asks the service to do, and
 * bugle_db opened read-only, which the service tells it to read again with Changed.
 *
 * Asking goes over D-Bus, which starts the service when it does not run; reading goes to the store directly, as
 * Messaging's UI reads bugle_db through its own cursors rather than asking the actions for rows. */
#pragma once

#include <gio/gio.h>

#include "sms-store.h"

G_BEGIN_DECLS

#define SMS_TYPE_CLIENT (sms_client_get_type ())
G_DECLARE_FINAL_TYPE (SmsClient, sms_client, SMS, CLIENT, GObject)

SmsClient *sms_client_new (void);

/* the store, or NULL until the service has made it (its first start) */
SmsStore  *sms_client_get_store (SmsClient *self);
/* a modem that can send (FALSE also in gsm2sip's gateway mode) */
gboolean   sms_client_get_available (SmsClient *self);
/* the network's country for formatting numbers ("DE"), or NULL */
const char *sms_client_get_country (SmsClient *self);

/* the conversation with exactly these numbers (as typed), made if new: its id, or 0 (the service did not answer) */
gint64     sms_client_get_conversation (SmsClient *self, const char * const *numbers);
void       sms_client_send (SmsClient *self, gint64 conversation, const char *text);
void       sms_client_resend (SmsClient *self, gint64 message);
void       sms_client_mark_read (SmsClient *self, gint64 conversation);
void       sms_client_set_archived (SmsClient *self, gint64 conversation, gboolean archived);
void       sms_client_set_draft (SmsClient *self, gint64 conversation, const char *text);
void       sms_client_delete_conversation (SmsClient *self, gint64 conversation);
void       sms_client_delete_message (SmsClient *self, gint64 message);
/* the media of a provider's message, which is not on the phone, is asked for: Changed tells when it is */
void       sms_client_fetch_media (SmsClient *self, gint64 message);
/* what is in front: -1 nothing, 0 the conversation list at its top, else that conversation */
void       sms_client_set_visible (SmsClient *self, gint64 conversation);

/* Signals: "changed" (gint64 conversation: 0 for more than one), as the service's Changed.
 * Properties: "available" (gboolean, read-only), "country" (string, read-only). */

G_END_DECLS
