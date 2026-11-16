// The lock screen's fingerprint states and words (finger.js), under plain gjs: finger.js imports no shell library.
//   gjs -m extension/tests/finger.test.js
import System from 'system';

import {fingerHint, fingerState, FingerStrings, secretWord} from '../finger.js';

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'fail'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

// --- the words
check('ready has its words', fingerHint('ready') === 'Tocca il sensore');
check('miss asks to try again', fingerHint('miss') === 'Non riconosciuto, riprova');
check('a state nobody named says nothing', fingerHint('') === '' && fingerHint('nonsense') === '');
check('the secret is named per lock type',
    fingerHint('tooMany', 'pin') === 'Troppi tentativi, usa il PIN' &&
    fingerHint('tooMany', 'pattern') === 'Troppi tentativi, usa il disegno');
check('restart is named per lock type',
    fingerHint('restart', 'pin') === 'Dopo il riavvio usa il PIN' &&
    fingerHint('restart', 'pattern') === 'Dopo il riavvio usa il disegno');
check('the way to the secret is named per lock type',
    secretWord('pin') === 'PIN' && secretWord('pattern') === 'Disegno' && secretWord('nonsense') === 'PIN');
for (const state of Object.keys(FingerStrings)) {
    check(`every lock type has words for ${state}`,
        !!fingerHint(state, 'pin') && !!fingerHint(state, 'pattern'));
}

// --- the state, and what it must never say
const open = {secure: true, bouncer: false, lockedDown: false, afterRestart: false, lockedOut: false,
    misses: 0, max: 5, signal: '', armed: true};

check('an armed sensor with nothing to report is ready', fingerState(open) === 'ready');
check('a sensor that is not armed is not ready', fingerState({...open, armed: false}) === 'unavailable');
check('the swipe lock says nothing', fingerState({...open, secure: false}) === 'off');
check('the bouncer says nothing', fingerState({...open, bouncer: true}) === 'off');
check('a restart asks for the PIN', fingerState({...open, afterRestart: true}) === 'restart');
check('Lockdown says the sensor is away', fingerState({...open, lockedDown: true}) === 'unavailable');
check('the PIN timeout turns the sensor off', fingerState({...open, lockedOut: true}) === 'lockedOut');
check('the last miss says the limit is reached', fingerState({...open, misses: 5}) === 'tooMany');
check('one under the limit is not', fingerState({...open, misses: 4}) === 'ready');
check('a miss signals itself', fingerState({...open, signal: 'miss'}) === 'miss');
check('fprintd reading says so', fingerState({...open, signal: 'scanning'}) === 'scanning');
// no policy in the settings: the sensor is off, and never reported as "too many attempts"
check('no limit is not a reached limit', fingerState({...open, max: 0}) === 'unavailable',
    fingerState({...open, max: 0}));

// no state that blocks the sensor may report it ready, whatever else is set
for (const blocked of [{afterRestart: true}, {lockedDown: true}, {lockedOut: true}, {bouncer: true},
    {secure: false}, {misses: 5}]) {
    const state = fingerState({...open, ...blocked});
    check(`never ready when blocked by ${Object.keys(blocked)[0]}`, state !== 'ready', state);
}

System.exit(failed ? 1 : 0);
