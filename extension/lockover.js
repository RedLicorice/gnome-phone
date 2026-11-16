// Two decisions of the lock screen, pure so they are tested under plain gjs (tests/lockover.test.js):
// whether an app's window shows over the lock screen (whenlocked.js), and whether the sensor is armed meanwhile.

// Whether the lock screen steps behind @overWindow: the phone locked, the top window declaring it may show over the lock
// screen and the stacks allowing it, the user's switch on (de.yesman.neo lock-over-window), and no unlock under way
// (@revealing: a swipe up or the bouncer has the lock screen in front, and the window goes behind it).
export function mayOcclude({showing, revealing = false, allowed = true, overWindow = null}) {
    return !!showing && !revealing && !!allowed && !!overWindow;
}

// Whether a finger on the sensor may unlock. A window over the lock screen is not in this list on purpose: it changes
// the picture, never the lock, and the sensor stays armed while it is in front.
export function fingerAllowed({showing, secure, afterRestart, lockedDown, unlocking, lockedOut, misses, max}) {
    return !!showing && !!secure && !afterRestart && !lockedDown && !unlocking && !lockedOut && misses < max;
}
