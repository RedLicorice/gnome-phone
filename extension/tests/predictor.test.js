// The touch keyboard's suggestions (hkeyboard/predictor.js) on the shipped dictionaries, under plain gjs:
//   gjs -m extension/tests/predictor.test.js [--bench]
// --bench also times a word typed letter by letter (the per-key cost on the phone's cores).
import GLib from 'gi://GLib';
import System from 'system';

import {Dictionary, fold} from '../hkeyboard/dictionary.js';
import {Predictor} from '../hkeyboard/predictor.js';

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const dicts = GLib.build_filenamev([here, '..', 'hkdicts']);
const scratch = GLib.dir_make_tmp('hkpredictor-XXXXXX');
const historyPath = GLib.build_filenamev([scratch, 'history.json']);

let failed = 0;
function check(name, ok, detail = '') {
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
    if (!ok)
        failed++;
}
const words = list => list.map(s => s.word);

// dictionary
const it = new Dictionary(GLib.build_filenamev([dicts, 'it.dict']));
check('it has many words', it.count > 50000, `${it.count}`);
check('fold drops accents and case', fold('Perché') === 'perche');
check('find exact', it.find('perché') >= 0);
check('complete is by frequency', words(it.complete('ch', 3))[0] === 'che', words(it.complete('ch', 3)).join());
check('folded prefix finds accented word', words(it.complete('perch', 5)).includes('perché'));
check('next words', it.next('di').length === 3, words(it.next('di')).join());

// predictor
const p = new Predictor(dicts, ['it', 'en'], historyPath);
let s = p.suggest('ciao com');
check('completes the word', words(s).includes('come'), words(s).join());
check('replaces the prefix', s.every(x => x.replace === 3));
s = p.suggest('Com');
check('keeps capital', words(s).every(w => w[0] === 'C'), words(s).join());
s = p.suggest('the hous');
check('second language', words(s).includes('house'), words(s).join());
s = p.suggest("l'acq");
check('elision', words(s).some(w => w.startsWith("l'acq") || w.startsWith('acq')), words(s).join());
s = p.suggest('grazie di ');
check('next word after space', s.length > 0 && s.every(x => x.replace === 0), words(s).join());
check('nothing after a full stop', p.suggest('Fine. ').length === 0);
check('nothing in an empty field', p.suggest('').length === 0);
check('nothing for digits', p.suggest('12').length === 0);
s = p.suggest('ciao xqzw');
check('unknown word offered as typed', s.some(x => x.literal && x.word === 'xqzw'), JSON.stringify(s));

// learning
p.learn('ciao zorbaz ');
check('a typo typed once is not suggested', !words(p.suggest('zorb')).includes('zorbaz'));
p.learn('ciao zorbaz ');
check('twice, it is', words(p.suggest('zorb')).includes('zorbaz'), words(p.suggest('zorb')).join());
p.learn('a qwfp ', true);
check('chosen once, it is', words(p.suggest('qwf')).includes('qwfp'));
check('learned pair', words(p.suggest('ciao ')).includes('zorbaz'), words(p.suggest('ciao ')).join());
p.forget('zorbaz');
check('forgotten', !words(p.suggest('zorb')).includes('zorbaz'));
p.forget('come');
check('dictionary word blocked', !words(p.suggest('com')).includes('come'), words(p.suggest('com')).join());
p.destroy();
const again = new Predictor(dicts, ['it', 'en'], historyPath);
check('history saved and read back', words(again.suggest('qwf')).includes('qwfp') &&
    !words(again.suggest('com')).includes('come'));
const mode = GLib.file_test(historyPath, GLib.FileTest.EXISTS)
    ? (new TextDecoder().decode(GLib.spawn_command_line_sync(`stat -c %a ${historyPath}`)[1]).trim()) : 'none';
check('history file is private', mode === '600', mode);
again.clearHistory();

if (ARGV.includes('--bench')) {
    const fresh = new Predictor(dicts, ['it', 'en'], null);
    let t = GLib.get_monotonic_time();
    fresh.suggest('a');
    print(`first suggestion (loads the dictionaries): ${((GLib.get_monotonic_time() - t) / 1000).toFixed(1)} ms`);
    const text = 'buongiorno a tutti come state oggi the quick brown fox ';
    const times = [];
    for (let i = 1; i <= text.length; i++) {
        t = GLib.get_monotonic_time();
        fresh.suggest(text.slice(0, i));
        times.push((GLib.get_monotonic_time() - t) / 1000);
    }
    times.sort((a, b) => a - b);
    const mean = times.reduce((a, b) => a + b) / times.length;
    print(`per key: mean ${mean.toFixed(2)} ms, median ${times[times.length >> 1].toFixed(2)} ms, ` +
        `max ${times[times.length - 1].toFixed(2)} ms (${times.length} keys)`);
}

GLib.unlink(historyPath);
GLib.rmdir(scratch);
print(failed ? `${failed} failed` : 'all passed');
System.exit(failed ? 1 : 0);
