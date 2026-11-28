/* NetworkManager's client, made in the background at start (see ns-net.c) */
#pragma once

void ns_net_init (void);

#include <adwaita.h>

/* the first IPv4 address of NetworkManager's primary connection, NULL when there is none (free it) */
char *ns_net_primary_ipv4 (void);
/* `page` (made with ns_later_init) is filled again whenever the primary connection or the connectivity changes, and
 * once as soon as the client is there */
void  ns_net_follow (AdwNavigationPage *page);
