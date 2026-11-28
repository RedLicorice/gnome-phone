/* The first run setup on the phone (`neo-settings --setup`, Minerva's minerva-setup-app user unit): the owner's
 * contact, the PIN, the Wi-Fi, then the address to open on the PC. Portrait, one step a page, each page replacing the
 * one before (no way back into a step that is done). The steps' work is Minerva's programs': `minerva-contacts
 * create` makes the owner's card with the id `minerva-setup state` names, `minerva-setup pin` sets the PIN (the
 * service refuses a second one), the Wi-Fi is Settings' own page. Minerva's design:
 * docs/superpowers/specs/2026-11-28-first-run-setup-design.md. */
#include "ns-setup.h"
#include "ns-look.h"
#include "ns-mc.h"
#include "ns-net.h"
#include "ns-pages.h"
#include "ns-ui.h"

static AdwNavigationView *nav;
static char *owner_id;      /* [setup] owner_contact, from `minerva-setup state` */
static char *setup_path;    /* [setup] setup_path */

static void show_contact (void);
static void show_pin (void);
static void show_wifi (void);
static void show_address (void);

/* the step's page replaces every page before it */
static void
replace (AdwNavigationPage *page)
{
  AdwNavigationPage *pages[] = { page };
  adw_navigation_view_replace (nav, pages, 1);
}

/* a step's page: its number, the title, the column to add to; the texts of its dialogs in Italian */
static AdwNavigationPage *
step_page (const char *title, const char *step, GtkWidget **content)
{
  AdwNavigationPage *page = ns_page_new (title, content);
  ns_texts_set (*content, "Non impostato", "Salva", "Annulla");
  ns_note (*content, step);
  return page;
}

/* the line under the cards that says what went wrong, made once per page */
static void
say (GtkWidget *content, const char *text)
{
  GtkWidget *note = g_object_get_data (G_OBJECT (content), "ns-setup-error");
  if (!note) {
    note = ns_note (content, "");
    gtk_widget_add_css_class (note, "error");
    g_object_set_data (G_OBJECT (content), "ns-setup-error", note);
  }
  gtk_label_set_text (GTK_LABEL (note), text);
}

/* ---- 1. The owner's contact ---- */

typedef struct {
  GtkWidget *content, *name, *surname, *phone, *email, *next;
} Contact;

static void
contact_made (JsonObject *root, const char *err, gpointer data)
{
  Contact *c = data;
  gtk_widget_set_sensitive (c->next, TRUE);
  if (err) {
    say (c->content, err);
    return;
  }
  show_pin ();
}

static void
contact_next (GtkWidget *row, gpointer data)
{
  Contact *c = data;
  const char *name = ns_row_value_get (c->name), *surname = ns_row_value_get (c->surname);
  const char *phone = ns_row_value_get (c->phone), *email = ns_row_value_get (c->email);
  if (!name || !*name) {
    say (c->content, "Scrivi almeno il tuo nome.");
    return;
  }
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_strv_builder_add_many (b, "create", name, NULL);
  if (surname && *surname)
    g_strv_builder_add (b, surname);
  g_strv_builder_add_many (b, "--id", owner_id, NULL);
  if (phone && *phone)
    g_strv_builder_add_many (b, "--phone", phone, NULL);
  if (email && *email)
    g_strv_builder_add_many (b, "--email", email, NULL);
  g_auto (GStrv) args = g_strv_builder_end (b);
  gtk_widget_set_sensitive (c->next, FALSE);
  ns_mc_run_bin (G_OBJECT (c->content), "minerva-contacts", (const char *const *) args, NULL, contact_made, c);
}

static void
show_contact (void)
{
  GtkWidget *content;
  AdwNavigationPage *page = step_page ("Chi sei?", "Passo 1 di 4 · Questa è la tua scheda: Minerva saprà che il "
                                       "telefono è tuo.", &content);
  Contact *c = g_new0 (Contact, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-setup", c, g_free);
  c->content = content;
  GtkWidget *g = ns_group (content, NULL);
  c->name = ns_row_value (g, "Nome", NULL, FALSE, NULL, NULL);
  c->surname = ns_row_value (g, "Cognome", NULL, FALSE, NULL, NULL);
  c->phone = ns_row_value (g, "Telefono", NULL, FALSE, NULL, NULL);
  c->email = ns_row_value (g, "Email", NULL, FALSE, NULL, NULL);
  c->next = ns_row_button (ns_group (content, NULL), "Avanti", FALSE, contact_next, c);
  replace (page);
}

/* ---- 2. The PIN ---- */

typedef struct {
  GtkWidget *content, *pin, *again, *next;
} Pin;

static void
pin_set (JsonObject *root, const char *err, gpointer data)
{
  Pin *p = data;
  gtk_widget_set_sensitive (p->next, TRUE);
  if (err) {
    say (p->content, err);
    return;
  }
  show_wifi ();
}

static void
pin_next (GtkWidget *row, gpointer data)
{
  Pin *p = data;
  const char *pin = ns_row_value_get (p->pin), *again = ns_row_value_get (p->again);
  if (!pin || !*pin || g_strcmp0 (pin, again) != 0) {
    say (p->content, "I due PIN sono diversi: scrivili di nuovo.");
    return;
  }
  g_autofree char *line = g_strconcat (pin, "\n", NULL);
  const char *args[] = { "pin", NULL };
  gtk_widget_set_sensitive (p->next, FALSE);
  ns_mc_run_bin (G_OBJECT (p->content), "minerva-setup", args, line, pin_set, p);
}

static void
show_pin (void)
{
  GtkWidget *content;
  AdwNavigationPage *page = step_page ("Scegli il PIN", "Passo 2 di 4 · Sblocca il telefono e approva le azioni "
                                       "importanti. Solo cifre.", &content);
  Pin *p = g_new0 (Pin, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-setup", p, g_free);
  p->content = content;
  GtkWidget *g = ns_group (content, NULL);
  p->pin = ns_row_value (g, "PIN", NULL, TRUE, NULL, NULL);
  p->again = ns_row_value (g, "Di nuovo", NULL, TRUE, NULL, NULL);
  p->next = ns_row_button (ns_group (content, NULL), "Avanti", FALSE, pin_next, p);
  replace (page);
}

/* ---- 3. The Wi-Fi: Settings' own page, then on ---- */

typedef struct {
  GtkWidget *state, *next;
} Wifi;

static void
wifi_fill (AdwNavigationPage *page)
{
  Wifi *w = g_object_get_data (G_OBJECT (page), "ns-setup");
  g_autofree char *ip = ns_net_primary_ipv4 ();
  ns_row_set_subtitle (w->state, ip ? ip : "Non collegato");
  gtk_widget_set_sensitive (w->next, ip != NULL);
}

static void
wifi_next (GtkWidget *row, gpointer data)
{
  show_address ();
}

static void
show_wifi (void)
{
  GtkWidget *content;
  AdwNavigationPage *page = step_page ("Wi-Fi", "Passo 3 di 4 · Collega il telefono alla rete del tuo PC.", &content);
  Wifi *w = g_new0 (Wifi, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-setup", w, g_free);
  GtkWidget *g = ns_group (content, NULL);
  ns_row_on_activate (ns_row (g, "network-wireless-symbolic", "blue", "Scegli la rete", "Le reti a portata"),
                      ns_page_wifi, NULL);
  w->state = ns_row (g, NULL, NULL, "Indirizzo del telefono", "Non collegato");
  w->next = ns_row_button (ns_group (content, NULL), "Avanti", FALSE, wifi_next, NULL);
  ns_later_init (page, wifi_fill);
  ns_net_follow (page);
  replace (page);
}

/* ---- 4. The address to open on the PC ---- */

typedef struct {
  GtkWidget *url;
} Address;

static void
address_fill (AdwNavigationPage *page)
{
  Address *a = g_object_get_data (G_OBJECT (page), "ns-setup");
  g_autofree char *ip = ns_net_primary_ipv4 ();
  g_autofree char *url = ip ? g_strdup_printf ("http://%s%s", ip, setup_path) : NULL;
  gtk_label_set_text (GTK_LABEL (a->url), url ? url : "Il telefono non è collegato: torna al Wi-Fi.");
}

static void
show_address (void)
{
  GtkWidget *content;
  AdwNavigationPage *page = step_page ("Continua dal PC", "Passo 4 di 4 · Sul PC, nella stessa rete, apri:", &content);
  Address *a = g_new0 (Address, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-setup", a, g_free);
  a->url = gtk_label_new (NULL);
  gtk_label_set_selectable (GTK_LABEL (a->url), TRUE);
  gtk_label_set_wrap (GTK_LABEL (a->url), TRUE);
  gtk_widget_add_css_class (a->url, "title-2");
  gtk_widget_add_css_class (a->url, "ns-setup-url");
  ns_row_widget (ns_group (content, NULL), NULL, a->url);
  ns_note (content, "Quando lo apri, il telefono ti chiede il PIN o l'impronta.");
  ns_row_on_activate (ns_row (ns_group (content, NULL), "network-wireless-symbolic", "blue", "Wi-Fi",
                              "Cambia rete"), ns_page_wifi, NULL);
  ns_later_init (page, address_fill);
  ns_net_follow (page);
  replace (page);
}

/* ---- Where to start: the state the phone is in ---- */

static void
contact_known (JsonObject *root, const char *err, gpointer data)
{
  gboolean pin = GPOINTER_TO_INT (data);
  if (err)        /* no owner's card yet */
    show_contact ();
  else if (!pin)
    show_pin ();
  else if (ns_net_primary_ipv4 () == NULL)
    show_wifi ();
  else
    show_address ();
}

static void
state_known (JsonObject *root, const char *err, gpointer data)
{
  GtkWindow *win = data;
  if (err) {
    GtkWidget *content;
    AdwNavigationPage *page = step_page ("Setup", "Il servizio di setup non risponde.", &content);
    say (content, err);
    replace (page);
    return;
  }
  if (json_object_get_boolean_member_with_default (root, "done", FALSE)) {
    gtk_window_close (win);
    return;
  }
  g_free (owner_id);
  owner_id = g_strdup (ns_json_str (root, "owner_contact"));
  g_free (setup_path);
  setup_path = g_strdup (ns_json_str (root, "path"));
  gboolean pin = json_object_get_boolean_member_with_default (root, "pin", FALSE);
  const char *args[] = { "get", owner_id, NULL };
  ns_mc_run_bin (G_OBJECT (win), "minerva-contacts", args, NULL, contact_known, GINT_TO_POINTER (pin));
}

void
ns_setup_open (GtkApplication *app)
{
  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "Setup");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  gtk_widget_add_css_class (win, "ns-window");
  ns_look_init (win);
  nav = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  ns_ui_init (nav);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), GTK_WIDGET (nav));
  GtkWidget *content;
  replace (step_page ("Setup", "Un momento…", &content));
  gtk_window_present (GTK_WINDOW (win));
  const char *args[] = { "state", NULL };
  ns_mc_run_bin (G_OBJECT (win), "minerva-setup", args, NULL, state_known, win);
}
