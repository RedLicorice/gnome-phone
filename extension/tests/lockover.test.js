// The lock screen's two pure decisions (lockover.js), under plain gjs: the module imports no shell library.
//   gjs -m extension/tests/lockover.test.js
import System from 'system';

import {fingerAllowed, mayOcclude} from '../lockover.js';

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'fail'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

// --- a window over the lock screen (the kiosk, an alarm, a call)
const over = {showing: true, revealing: false, allowed: true, overWindow: 'win'};
check('a declaring window goes over the lock screen', mayOcclude(over));
check('a window does not go over an unlocked screen', !mayOcclude({...over, showing: false}));
check('the switch off keeps every window behind the lock screen', !mayOcclude({...over, allowed: false}));
check('no window declared, nothing to step behind', !mayOcclude({...over, overWindow: null}));
check('while the unlock is under way the lock screen stays in front', !mayOcclude({...over, revealing: true}));

// --- the sensor
const armed = {showing: true, secure: true, afterRestart: false, lockedDown: false, unlocking: false,
    lockedOut: false, misses: 0, max: 5};
check('the sensor is armed on a PIN lock screen', fingerAllowed(armed));
check('not on an unlocked screen', !fingerAllowed({...armed, showing: false}));
check('not on the swipe lock', !fingerAllowed({...armed, secure: false}));
check('not after a restart', !fingerAllowed({...armed, afterRestart: true}));
check('not in Lockdown', !fingerAllowed({...armed, lockedDown: true}));
check('not during the unlock', !fingerAllowed({...armed, unlocking: true}));
check('not in the PIN timeout', !fingerAllowed({...armed, lockedOut: true}));
check('not past the miss limit', !fingerAllowed({...armed, misses: 5}));
check('armed again one under the limit', fingerAllowed({...armed, misses: 4}));

System.exit(failed ? 1 : 0);
