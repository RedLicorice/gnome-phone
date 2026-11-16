// The decisions of the lock screen, pure so they are tested under plain gjs (tests/lockover.test.js): whether an app's
// window shows over the lock screen (whenlocked.js), whether the sensor is armed meanwhile, whether the phone wants
// its secret first, whether the secret has been entered since the boot, and whether a recognized voice may wake the
// screen (voicewake.js).

// Whether the lock screen steps behind @overWindow: the phone locked, the top window declaring it may show over the lock
// screen and the stacks allowing it, the user's switch on (de.yesman.neo lock-over-window), no unlock under way
// (@revealing: a swipe up or the bouncer has the lock screen in front, and the window goes behind it) and no secret
// wanted first (@requiresSecret): while the PIN or the pattern is what unlocks the phone, it is that screen the user
// needs, and a window over it would hide the way to it.
export function mayOcclude({showing, revealing = false, allowed = true, overWindow = null, requiresSecret = false}) {
    return !!showing && !revealing && !requiresSecret && !!allowed && !!overWindow;
}

// Whether the secret was entered since the boot or the shell started: org.minerva.ScreenWake1's SecretEntered, which
// every Minerva service waits for before it engages (hears the wake word, speaks, asks, wakes the agent). The swipe lock
// has no secret to wait for. Unlike secretRequired this is the boot rule alone: Lockdown or a PIN timeout later do not take it back.
export function secretEntered({secure = true, afterRestart = false}) {
    return !secure || !afterRestart;
}

// Whether the phone wants its secret before anything else: the first unlock after a boot or a shell restart
// (@afterRestart), a Lockdown, the PIN's own retry timeout, or a sensor out of misses. The swipe lock wants none, and a
// policy that arms no sensor (no limit configured) leaves the PIN as the only way in.
export function secretRequired({secure = true, afterRestart = false, lockedDown = false, lockedOut = false,
    misses = 0, max = 0}) {
    return !!secure && (!!afterRestart || !!lockedDown || !!lockedOut || !(max > 0) || misses >= max);
}

// Whether a finger on the sensor may unlock. A window over the lock screen is not in this list on purpose: it changes
// the picture, never the lock, and the sensor stays armed while it is in front.
export function fingerAllowed({showing, secure, afterRestart, lockedDown, unlocking, lockedOut, misses, max}) {
    return !!showing && !!secure && !afterRestart && !lockedDown && !unlocking && !lockedOut && misses < max;
}

// Whether a recognized voice may wake the screen: '' when it may, else the first reason it may not: Lockdown and the
// PIN's own timeout first, then a PIN lock nobody has entered since boot or a shell restart, which keeps the screen
// dark to a voice too. A wake shows the lock screen with the kiosk face over it, never the unlock (power.js wakeUp).
// The yes/no is secretRequired's own (the sensor's misses play no part: a voice is no finger, so none are counted
// against it); the reason is the first that holds.
export function voiceWakeAllowed({secure, afterRestart, lockedDown, lockedOut}) {
    if (!secretRequired({secure, afterRestart, lockedDown, lockedOut, misses: 0, max: 1}))
        return '';
    if (lockedDown)
        return 'locked-down';
    if (lockedOut)
        return 'locked-out';
    return 'pin-not-entered';
}

// Whether the lock screen offers "Sblocca con la voce". Voice is the weakest biometric (a recording of the owner can be
// replayed), so it is offered only when the owner turned it on, the lock has a secret, a PIN or a finger already
// unlocked since the boot or the shell started, no lock-down and no PIN timeout is on, and its own misses are not used
// up. The window (N hours) and the threshold are the voice service's: this is only whether to offer the button.
export function voiceUnlockAllowed({enabled, showing, secure, afterRestart, lockedDown, unlocking, lockedOut, misses,
    max}) {
    return !!enabled && !!showing && !!secure && !afterRestart && !lockedDown && !unlocking && !lockedOut && misses < max;
}
