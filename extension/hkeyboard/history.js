// What the user types, for the suggestions: words and word pairs with a use count that fades with time (half of it
// after a month), and the dictionary words the user asked to forget. A JSON file in the user's data directory, private
// (mode 600), written a while after the last change and when the keyboard goes away.
// No shell imports: the tests run it under plain gjs.
import GLib from 'gi://GLib';

import {fold} from './dictionary.js';

const HALF_LIFE_DAYS = 30;
const MAX_WORDS = 3000, MAX_PAIRS = 3000;
const SAVE_DELAY_S = 10;
const VERSION = 1;

function today() {
    return Math.floor(GLib.get_real_time() / 86400e6);
}

// a count faded by its age
function weight(entry, now) {
    return entry.n * 0.5 ** ((now - entry.t) / HALF_LIFE_DAYS);
}

export class History {
    // @path the JSON file, made on the first save
    constructor(path) {
        this._path = path;
        this._words = new Map(); // word -> {n, t, key}
        this._pairs = new Map(); // "prev\tword" -> {n, t}
        this._blocked = new Set(); // dictionary words not to suggest
        this._saveId = 0;
        this._load();
    }

    _load() {
        try {
            const [, contents] = GLib.file_get_contents(this._path);
            const data = JSON.parse(new TextDecoder().decode(contents));
            for (const [word, n, t] of data.words ?? [])
                this._words.set(word, {n, t, key: fold(word)});
            for (const [pair, n, t] of data.pairs ?? [])
                this._pairs.set(pair, {n, t});
            this._blocked = new Set(data.blocked ?? []);
        } catch (e) {
            if (!e.matches?.(GLib.FileError, GLib.FileError.NOENT))
                logError(e, `gnome-phone: keyboard history ${this._path}`);
        }
    }

    save() {
        if (this._saveId)
            GLib.source_remove(this._saveId);
        this._saveId = 0;
        const data = {
            version: VERSION,
            words: [...this._words].map(([w, e]) => [w, e.n, e.t]),
            pairs: [...this._pairs].map(([p, e]) => [p, e.n, e.t]),
            blocked: [...this._blocked],
        };
        try {
            GLib.mkdir_with_parents(GLib.path_get_dirname(this._path), 0o700);
            GLib.file_set_contents_full(this._path, JSON.stringify(data),
                GLib.FileSetContentsFlags.CONSISTENT, 0o600);
        } catch (e) {
            logError(e, `gnome-phone: keyboard history ${this._path}`);
        }
    }

    _queueSave() {
        if (this._saveId)
            return;
        this._saveId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, SAVE_DELAY_S, () => {
            this._saveId = 0;
            this.save();
            return GLib.SOURCE_REMOVE;
        });
    }

    // the least used entries out, past the limit
    _trim(map, max) {
        if (map.size <= max)
            return;
        const now = today();
        const sorted = [...map].sort((a, b) => weight(a[1], now) - weight(b[1], now));
        for (const [k] of sorted.slice(0, map.size - max))
            map.delete(k);
    }

    // @word typed (or chosen: @count more at once); @prev the word before it, or null
    learn(word, prev = null, count = 1) {
        const t = today();
        const entry = this._words.get(word) ?? {n: 0, t, key: fold(word)};
        entry.n = weight(entry, t) + count;
        entry.t = t;
        this._words.set(word, entry);
        this._blocked.delete(word);
        if (prev) {
            const key = `${prev}\t${word}`;
            const pair = this._pairs.get(key) ?? {n: 0, t};
            pair.n = weight(pair, t) + count;
            pair.t = t;
            this._pairs.set(key, pair);
        }
        this._trim(this._words, MAX_WORDS);
        this._trim(this._pairs, MAX_PAIRS);
        this._queueSave();
    }

    // the word's faded count, 0 if never typed
    count(word) {
        const entry = this._words.get(word);
        return entry ? weight(entry, today()) : 0;
    }

    // the typed words starting with the folded @key, used at least @min times: [{word, n}]
    complete(key, min) {
        const now = today(), out = [];
        for (const [word, entry] of this._words) {
            if (!entry.key.startsWith(key))
                continue;
            const n = weight(entry, now);
            if (n >= min)
                out.push({word, n});
        }
        return out;
    }

    // the words typed after @prev: [{word, n}]
    next(prev) {
        const now = today(), out = [], start = `${prev}\t`;
        for (const [pair, entry] of this._pairs) {
            if (pair.startsWith(start))
                out.push({word: pair.slice(start.length), n: weight(entry, now)});
        }
        return out;
    }

    isBlocked(word) {
        return this._blocked.has(word);
    }

    // never suggested again: the typed word and its pairs gone, a dictionary word blocked (@inDictionary)
    forget(word, inDictionary) {
        this._words.delete(word);
        for (const pair of [...this._pairs.keys()]) {
            const [a, b] = pair.split('\t');
            if (a === word || b === word)
                this._pairs.delete(pair);
        }
        if (inDictionary)
            this._blocked.add(word);
        this._queueSave();
    }

    // everything learned, gone
    clear() {
        this._words.clear();
        this._pairs.clear();
        this._blocked.clear();
        this.save();
    }

    destroy() {
        if (this._saveId)
            this.save();
    }
}
