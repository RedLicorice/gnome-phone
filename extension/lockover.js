// Three decisions of the lock screen, pure so they are tested under plain gjs (tests/lockover.test.js): whether an app's
// window shows over the lock screen (whenlocked.js), whether the sensor is armed meanwhile, and whether the phone wants
// its secret first.

// Whether the lock screen steps behind @overWindow: the phone locked, the top window declaring it may show over the lock
// screen and the stacks allowing it, the user's switch on (de.yesman.neo lock-over-window), no unlock under way
// (@revealing: a swipe up or the bouncer has the lock screen in front, and the window goes behind it) and no secret
// wanted first (@requiresSecret): while the PIN or the pattern is what unlocks the phone, it is that screen the user
// needs, and a window over it would hide the way to it.
export function mayOcclude({showing, revealing = false, allowed = true, overWindow = null, requiresSecret = false}) {
    return !!showing && !revealing && !requiresSecret && !!allowed && !!overWindow;
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
