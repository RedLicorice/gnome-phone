/* Minerva > Messaggi: the messaging providers of Messages (WhatsApp, Telegram, the agent), through
 * `minerva-config messaging`. Each provider's page holds its status, its pairing code, the read and write scopes,
 * the two flags, the whitelist and Disconnect; everything is data (messaging.conf, messaging_ui.json). */
#pragma once

#include <adwaita.h>

/* the index: one row per provider of `messaging list --json` */
void ns_page_messaging (GtkWidget *row, gpointer data);
/* one provider: `data` is its id (Messages' provider id, a string) */
void ns_page_provider (GtkWidget *row, gpointer data);
/* Accounts: the Telegram bot token, stored through `minerva-config key set telegram` (on stdin) */
void ns_messaging_token_add (GtkWidget *content, AdwNavigationPage *page);
