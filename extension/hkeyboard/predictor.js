// Word suggestions in the shell's own process, as Android's keyboards make them (LatinIME, HeliBoard): the word being
// typed completed from the dictionaries of the chosen languages (hkdicts/, HeliBoard's lists) and from what the user
// types (history.js), and after a space the likeliest next words. Nothing goes through the input method, so opening
// the keyboard switches no engine and a key costs no round trip.
// No shell imports: the tests run it under plain gjs.
import GLib from 'gi://GLib';

import {Dictionary, fold} from './dictionary.js';
import {History} from './history.js';

const MAX = 3;
// a typed word unknown to the dictionaries is suggested from this many uses on (a typo typed once is not)
const MIN_UNKNOWN = 2;
const LANGUAGE_STEP = 6; // the second language's words weigh a little less than the first's
const WORD = /[\p{L}\p{M}']+$/u;
const LETTER = /\p{L}/u;
// the end of a sentence: no next word guessed after it
const SENTENCE_END = /[.!?…:;]\s*$/u;

// the user's frequency on the dictionaries' scale (log, 0-255): once typed is about a common word
function userScore(n) {
    return 180 + 25 * Math.log2(1 + n);
}

// @word in the case the user typed @typed in: Capitalized or ALL CAPS
function matchCase(word, typed) {
    if (typed.length > 1 && typed === typed.toUpperCase() && typed !== typed.toLowerCase())
        return word.toUpperCase();
    if (typed[0] !== typed[0].toLowerCase())
        return word[0].toUpperCase() + word.slice(1);
    return word;
}

export class Predictor {
    // @dir the directory of the <language>.dict files; @languages ['it', 'en'], the first preferred;
    // @historyPath the user's history (history.js), or null to learn nothing
    constructor(dir, languages, historyPath) {
        this._dir = dir;
        this._dicts = new Map(); // language -> Dictionary, loaded on first use
        this.setLanguages(languages);
        this._history = historyPath ? new History(historyPath) : null;
    }

    setLanguages(languages) {
        this._languages = languages;
    }

    _dictionaries() {
        const out = [];
        for (const language of this._languages) {
            let dict = this._dicts.get(language);
            if (dict === undefined) {
                try {
                    dict = new Dictionary(GLib.build_filenamev([this._dir, `${language}.dict`]));
                } catch (e) {
                    logError(e, `gnome-phone: keyboard dictionary ${language}`);
                    dict = null;
                }
                this._dicts.set(language, dict);
            }
            if (dict)
                out.push(dict);
        }
        return out;
    }

    // a word any dictionary has (as typed, in lower case, or folded the same)
    known(word) {
        return this._dictionaries().some(d => d.find(word) >= 0 || d.find(word.toLowerCase()) >= 0);
    }

    // the suggestions for the text before the cursor: [{word, replace, literal}], best first; @replace is how many
    // characters before the cursor the word takes the place of (0 after a space: a next word)
    suggest(before) {
        if (before === null || before === undefined)
            return [];
        const match = before.match(WORD);
        const token = match?.[0].replace(/^'+/, '') ?? '';
        if (token && LETTER.test(token))
            return this._complete(token);
        if (/\s$/.test(before) && !SENTENCE_END.test(before))
            return this._next(before);
        return [];
    }

    _complete(token) {
        const scores = new Map(); // word -> {score, replace}
        const add = (word, score, replace) => {
            if (this._history?.isBlocked(word))
                return;
            const old = scores.get(word);
            if (!old || old.score < score)
                scores.set(word, {score, replace});
        };
        // "l'acq": the elided forms the dictionary has, and the word after the apostrophe
        const prefixes = [token];
        const apostrophe = token.lastIndexOf("'");
        if (apostrophe >= 0 && apostrophe < token.length - 1)
            prefixes.push(token.slice(apostrophe + 1));
        const dicts = this._dictionaries();
        for (const prefix of prefixes) {
            dicts.forEach((dict, i) => {
                for (const {word, freq} of dict.complete(prefix, MAX * 2))
                    add(word, freq - i * LANGUAGE_STEP, prefix.length);
            });
            for (const {word, n} of this._history?.complete(fold(prefix), 0) ?? []) {
                if (n >= MIN_UNKNOWN || this.known(word))
                    add(word, userScore(n), prefix.length);
            }
        }
        const out = [...scores].sort((a, b) => b[1].score - a[1].score).slice(0, MAX)
            .map(([word, {replace}]) => ({word: matchCase(word, token.slice(token.length - replace)), replace,
                literal: false}));
        // the word as typed, when no dictionary knows it: tapping it keeps it (and learns it)
        if (token.length > 1 && apostrophe < 0 && !this.known(token) && !out.some(s => s.word === token)) {
            out.splice(Math.min(1, out.length), 0, {word: token, replace: token.length, literal: true});
            out.length = Math.min(out.length, MAX);
        }
        return out;
    }

    _next(before) {
        const words = before.trimEnd().match(WORD);
        const prev = words?.[0].replace(/^'+/, '');
        if (!prev || !LETTER.test(prev))
            return [];
        const scores = new Map();
        const add = (word, score) => {
            if (!this._history?.isBlocked(word) && (scores.get(word) ?? -1) < score)
                scores.set(word, score);
        };
        this._dictionaries().forEach((dict, i) => {
            for (const {word, rank} of dict.next(prev))
                add(word, 230 - 10 * rank - i * LANGUAGE_STEP);
        });
        for (const {word, n} of this._history?.next(prev.toLowerCase()) ?? [])
            add(word, 220 + 20 * Math.log2(1 + n));
        return [...scores].sort((a, b) => b[1] - a[1]).slice(0, MAX)
            .map(([word]) => ({word, replace: 0, literal: false}));
    }

    // the form a word is learned in: lower case when it was capitalized only by its place (a sentence's start)
    _learnedForm(word) {
        const lower = word.toLowerCase();
        if (word !== lower && word.slice(1) === lower.slice(1) && this.known(lower))
            return lower;
        return word;
    }

    // a word just ended (a space or a punctuation mark after it): @before is the text before the cursor, ending with
    // the word and that mark. @chosen: the user picked it from the bar (an unknown word counts as known at once).
    learn(before, chosen = false) {
        if (!this._history || !before)
            return;
        const m = before.match(/([\p{L}\p{M}']+)([^\p{L}\p{M}']+)$/u);
        if (!m || /[\p{N}_@/]/u.test(m[2]))
            return;
        const word = this._learnedForm(m[1].replace(/^'+|'+$/g, ''));
        if (word.length < 2 || !LETTER.test(word))
            return;
        // the word before, if only spaces stand between them
        const rest = before.slice(0, before.length - m[0].length);
        const p = rest.match(/([\p{L}\p{M}']+)\s+$/u);
        const prev = p ? this._learnedForm(p[1].replace(/^'+|'+$/g, '')).toLowerCase() : null;
        const count = chosen && !this.known(word) && this._history.count(word) < MIN_UNKNOWN ? MIN_UNKNOWN : 1;
        this._history.learn(word, prev, count);
    }

    // a suggestion the user does not want: out of the history, and out of the suggestions if a dictionary has it
    forget(word) {
        this._history?.forget(word, this.known(word));
        this._history?.forget(word.toLowerCase(), this.known(word.toLowerCase()));
    }

    clearHistory() {
        this._history?.clear();
    }

    destroy() {
        this._history?.destroy();
        this._history = null;
    }
}
