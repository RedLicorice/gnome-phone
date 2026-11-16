/* gp-auth: checks the lock screen's PIN or pattern for the user who runs it, through PAM (service "gnome-phone").
 * The secret comes on stdin, one line. A pattern is the digits 1-9 of its dots in the order they were joined ("1478"),
 * as Phosh's pattern lock had it, so either one is simply the user's password.
 *
 * Exit status: 0 right, 1 wrong (or the account refused), 2 PAM could not run. PAM's own delay after a failure is
 * turned off: the lock screen shows a wrong attempt at once and throttles repeated ones itself, as Android does.
 *
 * pam_unix checks the password of the user it runs as through its setgid helper (unix_chkpwd), so this needs no
 * privileges of its own.
 *
 * gp-auth --voice: the owner's voice instead, through PAM service "gnome-phone-voice" (pam_exec runs gp-voice-check,
 * which asks the Minerva voice service for a live check). Nothing is read from stdin. Exit status: 0 the voice matched
 * and the rules allowed it, 1 it did not (wrong voice, window closed, misses used up), 3 the voice service is not
 * there to ask, 2 PAM could not run. The lock screen only offers this once the PIN or a finger unlocked since boot,
 * within the window the service enforces, and never while a lock-down or a PIN timeout is on.
 *
 * gp-auth --fingerprint: a finger on the sensor instead, through PAM service "gnome-phone-fingerprint" (pam_fprintd,
 * one try). Nothing is read from stdin. Exit status: 0 the finger matched, 1 it did not, 3 no finger came (pam_fprintd's
 * timeout, or SIGINT: the lock screen stops it so) or there is nothing to match with (no fprintd, no sensor, no prints),
 * 2 PAM could not run. The lock screen counts the misses and asks for the PIN after a few. */
#include <pwd.h>
#include <stdbool.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SECRET 256

static char secret[MAX_SECRET];
static bool fingerprint;
static bool voice;

static int
converse (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  struct pam_response *r = calloc (n, sizeof *r);

  if (!r)
    return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    switch (msg[i]->msg_style) {
    case PAM_PROMPT_ECHO_OFF:
      /* a finger or a voice only: a module that wants a password has nothing to get */
      if (fingerprint || voice) {
        for (int j = 0; j < i; j++)
          free (r[j].resp);
        free (r);
        return PAM_CONV_ERR;
      }
      r[i].resp = strdup (secret);
      break;
    case PAM_PROMPT_ECHO_ON:   /* only ever the user name */
      r[i].resp = strdup ((const char *) data);
      break;
    default:                   /* PAM_ERROR_MSG, PAM_TEXT_INFO */
      continue;
    }
    if (!r[i].resp) {
      for (int j = 0; j < i; j++)
        free (r[j].resp);
      free (r);
      return PAM_BUF_ERR;
    }
  }
  *resp = r;
  return PAM_SUCCESS;
}

static void
no_delay (int status, unsigned int usec, void *data)
{
}

int
main (int argc, char **argv)
{
  struct passwd *pw = getpwuid (getuid ());
  struct pam_conv conv = { converse, NULL };
  pam_handle_t *pamh = NULL;
  size_t len;
  int ret;

  fingerprint = argc > 1 && strcmp (argv[1], "--fingerprint") == 0;
  voice = argc > 1 && strcmp (argv[1], "--voice") == 0;
  if (!pw)
    return 2;
  if (!fingerprint && !voice) {
    if (!fgets (secret, sizeof secret, stdin))
      return 2;
    len = strcspn (secret, "\n");
    secret[len] = '\0';
    if (len == 0)
      return 1;
  }

  /* pam_fprintd takes SIGINT through a signalfd, so only while it is blocked, as sudo has it: then SIGINT ends the
   * verify and lets the sensor go (exit 3) instead of killing this with the device still claimed */
  if (fingerprint) {
    sigset_t block;
    sigemptyset (&block);
    sigaddset (&block, SIGINT);
    sigprocmask (SIG_BLOCK, &block, NULL);
  }

  conv.appdata_ptr = pw->pw_name;
  ret = pam_start (fingerprint ? "gnome-phone-fingerprint"
                   : voice ? "gnome-phone-voice" : "gnome-phone", pw->pw_name, &conv, &pamh);
  if (ret != PAM_SUCCESS)
    return 2;
  pam_set_item (pamh, PAM_FAIL_DELAY, (const void *) no_delay);
  ret = pam_authenticate (pamh, 0);
  if (ret == PAM_SUCCESS)
    ret = pam_acct_mgmt (pamh, 0);
  explicit_bzero (secret, sizeof secret);
  pam_end (pamh, ret);

  if (fingerprint && ret != PAM_SUCCESS)
    return ret == PAM_AUTH_ERR || ret == PAM_MAXTRIES ? 1 : ret == PAM_AUTHINFO_UNAVAIL ? 3 : 2;
  /* pam_exec answers a refusal with the command's non-zero exit as PAM_SERVICE_ERR: for the voice both mean "no",
   * and only a PAM that could not run at all (2) or a helper that is not there (3) is told apart */
  if (voice && ret != PAM_SUCCESS)
    return ret == PAM_AUTHINFO_UNAVAIL ? 3 : (ret == PAM_BUF_ERR || ret == PAM_CONV_ERR ? 2 : 1);
  return ret == PAM_SUCCESS ? 0 : 1;
}
