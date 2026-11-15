// The fingerprint sensor in the power key, as the lock screen tells it: its states, the words for them, and the
// mapping from what the keyguard knows to one state (fingerhint.js draws it, keyguard.js calls it).
//
// Data and pure functions only, no shell imports: the test runs it under plain gjs (tests/finger.test.js), which
// has no St or Clutter to import.

// The words are the phone's, not a translation of Android's: Italian, the language its owner reads on this screen.
// The states that name the secret have one text per lock type.
export const FingerStrings = {
    ready: 'Tocca il sensore',
    scanning: 'Lettura in corso',
    miss: 'Non riconosciuto, riprova',
    tooMany: {
        pin: 'Troppi tentativi, usa il PIN',
        pattern: 'Troppi tentativi, usa il disegno',
    },
    restart: {
        pin: 'Dopo il riavvio usa il PIN',
        pattern: 'Dopo il riavvio usa il disegno',
    },
    lockedOut: 'Attendi per riprovare',
    unavailable: 'Sensore non disponibile',
};

// fprintd's device signals (net.reactivated.Fprint.Device.VerifyStatus), as the states they mean. The ones that say
// the touch was read but not accepted are 'scanning': the sensor is still reading. 'verify-match' is the unlock,
// which the run's own exit status confirms; it only puts the hint back where it starts.
export const VERIFY_STATES = {
    'verify-retry-scan': 'scanning',
    'verify-swipe-too-short': 'scanning',
    'verify-finger-not-centered': 'scanning',
    'verify-remove-and-retry': 'scanning',
    'verify-no-match': 'miss',
    'verify-disconnected': 'unavailable',
    'verify-unknown-error': 'unavailable',
    'verify-match': 'ready',
};

// @state: one of the states above; @kind: the bouncer's kind ('pin' or 'pattern') for the words that name it.
// '' means there is nothing to say.
export function fingerHint(state, kind = 'pin') {
    const words = FingerStrings[state];
    if (!words)
        return '';
    return typeof words === 'string' ? words : words[kind] ?? words.pin;
}

// @flags: what the keyguard knows about the sensor right now —
//   secure        the lock has a secret at all (a swipe lock has no sensor feedback)
//   bouncer       the PIN pad or the pattern is up, and carries its own messages
//   lockedDown    Lockdown, or a lock type with the sensor off: never armed
//   afterRestart  PIN first after a restart: policy, so the sensor cannot unlock yet
//   lockedOut     the PIN's own retry timeout is running: the sensor is off until it ends
//   misses, max   the misses so far, and the limit the policy allows
//   signal        the last fprintd VerifyStatus state, while it still applies ('' or null: none)
//   armed         a gp-auth --fingerprint run holds the device
// @returns the state to show, 'off' when the hint stays hidden.
export function fingerState(flags) {
    const {secure, bouncer, lockedDown, afterRestart, lockedOut, misses, max, signal, armed} = flags;
    if (!secure || bouncer)
        return 'off';
    if (lockedDown)
        return 'unavailable';
    if (afterRestart)
        return 'restart';
    if (lockedOut)
        return 'lockedOut';
    // no limit configured: there is no policy to arm under, so the sensor is not there to touch
    if (!(max > 0))
        return 'unavailable';
    if (misses >= max)
        return 'tooMany';
    if (signal)
        return signal;
    if (armed)
        return 'ready';
    // nothing holds the device and nothing was said since: the sensor is not listening
    return 'unavailable';
}
