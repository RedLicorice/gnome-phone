// The camera indicator's words (cameratext.js), under plain gjs:
//   gjs -m extension/tests/camera.test.js
import System from 'system';

import {clock, doing, facesText, historyLine, owner, parseState} from '../cameratext.js';

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}

const mine = (purpose, extra = {}) => ({source: 'minerva', camera: 'front', label: 'frontale', purpose, detail: '',
    faces: [], note: '', since: 0, ...extra});
const other = who => ({source: 'system', camera: 'main', label: 'principale', who, faces: [], since: 0});

// the state document
check('empty state for garbage', parseState('nope').active.length === 0 && parseState('').history.length === 0);
check('state parsed', parseState('{"active":[{"id":"a"}],"history":[]}').active[0].id === 'a');
check('missing lists are empty', parseState('{}').active.length === 0);

// who and what
check('Minerva owns its captures', owner(mine('who')) === 'Minerva');
check('an app is named by its program', owner(other('neo-camera')) === 'Fotocamera');
check('an unknown program keeps its name', owner(other('foo')) === 'foo');
check('no name is "Un\'app"', owner(other('')) === "Un'app");
check('enrol says whose face', doing(mine('enroll', {detail: 'Giuseppe'})) === 'Sto imparando il volto di Giuseppe',
    doing(mine('enroll', {detail: 'Giuseppe'})));
check('enrol without a name', doing(mine('enroll')).startsWith('Sto imparando il volto di'));
check('who has its line', doing(mine('who')).includes('riconoscendo'));
check('other app says which camera', doing(other('x')) === 'sta usando la fotocamera principale');
check('unknown purpose still says Minerva', doing(mine('zzz')).startsWith('Minerva'));

// what was recognised
check('names with scores', facesText(mine('who', {faces: [{name: 'Giuseppe', score: 0.711}]})) === 'Giuseppe (0,71)');
check('unknown is sconosciuto', facesText(mine('who', {faces: [{name: 'unknown', score: 0.3}]})) === 'sconosciuto (0,30)');
check('several faces', facesText(mine('who', {faces: [{name: 'A', score: 0.5}, {name: 'unknown', score: 0.1}]})) ===
    'A (0,50), sconosciuto (0,10)');
check('the note when no face', facesText(mine('who', {note: 'nessun volto'})) === 'nessun volto');
check('nothing to say', facesText(mine('who')) === '');

// history lines
const t = new Date(2026, 10, 14, 22, 41).getTime() / 1000;
check('clock is local HH:MM', clock(t) === '22:41', clock(t));
check('history line', historyLine({...mine('who', {faces: [{name: 'Giuseppe', score: 0.7}]}), start: t, end: t + 3}) ===
    '22:41  Minerva · Sto riconoscendo chi è davanti alla fotocamera · Giuseppe (0,70)');
check('history line of another app', historyLine({...other('neo-camera'), start: t, end: t + 3}) ===
    '22:41  Fotocamera · fotocamera principale');

System.exit(failed ? 1 : 0);
