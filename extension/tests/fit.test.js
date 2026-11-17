// How a window that is too big for the screen is fitted (fitmath.js), under plain gjs: no shell library to import.
//   gjs -m extension/tests/fit.test.js
import System from 'system';

import {fitOffset, fitScale} from '../fitmath.js';

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

// the portal's Remote Desktop dialog: 540 x 761 in a work area of 360 x 761
check('too wide: scaled by the width', Math.abs(fitScale(540, 761, 360, 761) - 360 / 540) < 1e-9);
check('too high: scaled by the height', Math.abs(fitScale(300, 1000, 360, 761) - 0.761) < 1e-9);
check('both: the smaller factor', Math.abs(fitScale(720, 1522, 360, 761) - 0.5) < 1e-9);
check('fits: left alone', fitScale(360, 761, 360, 761) === 1);
check('smaller: never made bigger', fitScale(100, 100, 360, 761) === 1);
for (const [w, h, aw, ah] of [[0, 10, 360, 761], [10, -1, 360, 761], [10, 10, 0, 761], [NaN, 10, 360, 761]])
    check(`no size to fit (${w}, ${h}, ${aw}, ${ah})`, fitScale(w, h, aw, ah) === 1);

// the scaled frame ends up in the middle of the area, its shadow (the buffer around the frame) counted
const area = {x: 0, y: 39, width: 360, height: 761};
const s = fitScale(540, 600, area.width, area.height);
const o = fitOffset(s, {x: 0, y: 39, width: 540, height: 600}, {x: 0, y: 39, width: 540, height: 600}, area);
check('width fills the area', o.x === 0 && Math.abs(540 * s - 360) < 1e-9, JSON.stringify(o));
// the buffer starts at y = 39: the frame's top is there plus the offset, and the frame's middle is the area's
check('centred in height', Math.abs(39 + o.y + 600 * s / 2 - (area.y + area.height / 2)) <= 1, JSON.stringify(o));
const shadow = fitOffset(0.5, {x: 10, y: 20, width: 200, height: 100}, {x: 0, y: 0, width: 220, height: 130},
    {x: 0, y: 0, width: 400, height: 400});
// frame's scaled left = buffer.x + offset + (frame.x - buffer.x) * scale = (400 - 100) / 2 = 150
check('offset allows for the shadow', shadow.x + 10 * 0.5 === 150 && shadow.y + 20 * 0.5 === 175, JSON.stringify(shadow));

System.exit(failed ? 1 : 0);
