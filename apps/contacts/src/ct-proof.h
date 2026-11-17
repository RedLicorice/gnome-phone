/* The human proof of an action on a whole contact (deleting it, merging it into another): who approved exactly this
 * action, and when. Minerva's memory service asks for one with every such call (`HumanProof`: approver, time, the
 * action's sentence) and refuses a stale or different one.
 *
 * The app gets the proof from a small provider, so that where it comes from can change without touching the
 * callers: today the shell's own sheets over D-Bus (org.minerva.Pin1: AskFinger, AskPin; a finger on the sensor or the
 * PIN), later the token service's human approval. A development provider approves at once, for the tests and for the
 * test rig; it is only chosen from the environment when the test rig started the app (CT_RIG_DIR). The action's
 * sentence is built in one place (ct_proof_action), the one the service compares with. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  CT_PROOF_DELETE,              /* "delete contact <id>" */
  CT_PROOF_MERGE,               /* "merge contact <from> into <to>" */
} CtProofVerb;

typedef enum {
  CT_PROOF_FINGER,
  CT_PROOF_PIN,
} CtProofMethod;

/* The sentence the memory service checks the proof against. @second only for a merge (the contact that stays). */
char *ct_proof_action (CtProofVerb verb, const char *first, const char *second);

/* The proof as the service's JSON takes it ({"approver", "at" (RFC 3339, UTC, now), "action"}) */
char *ct_proof_json (const char *approver, const char *action);

typedef struct {
  CtProofMethod method;
  const char *reason;           /* what the sheet says, in the user's language */
  int seconds;                  /* how long the sheet waits */
  int tries;                    /* PIN: wrong tries before it gives up */
  const char *wrong_text;       /* PIN: what the pad says after a wrong one */
} CtProofRequest;

/* @ok: the person approved; else @why says why not (no, timed out, no shell) */
typedef void (*CtProofDone) (gboolean ok, const char *why, gpointer data);

typedef struct {
  void (*ask) (GObject *owner, const CtProofRequest *request, CtProofDone done, gpointer data);
} CtProofProvider;

const CtProofProvider *ct_proof_shell_provider (void);
/* approves at once (or refuses, with ct_proof_dev_refuse); keeps the last request for the tests to look at */
const CtProofProvider *ct_proof_dev_provider (void);
void                   ct_proof_dev_refuse (gboolean refuse);
const CtProofRequest  *ct_proof_dev_last (void);

/* NULL goes back to the default: the shell's, or the development one when the test rig asks for it */
void ct_proof_set_provider (const CtProofProvider *provider);
void ct_proof_ask (GObject *owner, const CtProofRequest *request, CtProofDone done, gpointer data);

G_END_DECLS
