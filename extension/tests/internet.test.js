// The internet state the lock screen and the status bar show (internet.js), under plain gjs, with no minerva-net on the bus:
//   gjs -m extension/tests/internet.test.js
import System from 'system';

import {InternetWatch} from '../internet.js';

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

const w = new InternetWatch();
let changes = 0;
w.connect('changed', () => changes++);

check('nothing is claimed while minerva-net is not on the bus', !w.known && !w.offline);
check('the word comes from internet.json', w.text === 'Offline', w.text);
w._set(true);
check('online is not offline', w.known && w.online && !w.offline && changes === 1);
w._set(false);
check('offline once it says so', w.offline && changes === 2);
w._set(false);
check('the same state is not announced again', changes === 2);
w._set(null);
check('the service going away claims nothing', !w.known && !w.offline && changes === 3);
w.destroy();
System.exit(failed ? 1 : 0);
