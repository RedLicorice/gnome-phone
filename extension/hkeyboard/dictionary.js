// A language's word list for the suggestions (tools/dict2bin.py writes it): words sorted by their folded form, a
// frequency each (0-255, log scale) and the three likeliest next words. The file is mapped, not parsed: a lookup is a
// binary search for the typed prefix and a scan of the frequencies of the words that start with it.
// No shell imports: the tests run it under plain gjs.
import GLib from 'gi://GLib';

const MAGIC = 0x31444b48; // 'HKD1'
const NONE = 0xffff;
const decoder = new TextDecoder();

// the form a typed prefix and a word are compared in: decomposed, marks dropped, lower case (tools/dict2bin.py fold)
export function fold(text) {
    return text.normalize('NFD').replace(/\p{Mn}/gu, '').toLowerCase();
}

export class Dictionary {
    // @path a .dict file
    constructor(path) {
        this._file = GLib.MappedFile.new(path, false);
        const bytes = this._file.get_bytes().toArray();
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (view.getUint32(0, true) !== MAGIC)
            throw new Error(`${path}: not a dictionary`);
        this.count = view.getUint32(4, true);
        const at = i => view.getUint32(8 + 4 * i, true);
        this._offsets = new Uint32Array(bytes.buffer.slice(bytes.byteOffset + at(0),
            bytes.byteOffset + at(0) + 4 * (this.count + 1)));
        this._freq = bytes.subarray(at(1), at(1) + this.count);
        this._bigrams = new Uint16Array(bytes.buffer.slice(bytes.byteOffset + at(2),
            bytes.byteOffset + at(2) + 6 * this.count));
        this._strings = bytes.subarray(at(3));
    }

    word(id) {
        return decoder.decode(this._strings.subarray(this._offsets[id], this._offsets[id + 1]));
    }

    freq(id) {
        return this._freq[id];
    }

    // the first word whose folded form is not below @key
    _lowerBound(key) {
        let lo = 0, hi = this.count;
        while (lo < hi) {
            const mid = (lo + hi) >>> 1;
            if (fold(this.word(mid)) < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    // the word's id, or -1: the exact word, or (@folded) one that folds the same
    find(word, folded = false) {
        const key = fold(word);
        for (let id = this._lowerBound(key); id < this.count; id++) {
            const w = this.word(id);
            if (fold(w) !== key)
                break;
            if (folded || w === word)
                return id;
        }
        return -1;
    }

    // the @n most frequent words starting with the folded @prefix: [{word, freq}], best first
    complete(prefix, n) {
        const key = fold(prefix);
        const start = this._lowerBound(key), end = this._lowerBound(`${key}￿`);
        const best = []; // ids, by frequency
        for (let id = start; id < end; id++) {
            const f = this._freq[id];
            if (best.length === n && f <= this._freq[best[n - 1]])
                continue;
            let i = Math.min(best.length, n - 1);
            while (i > 0 && this._freq[best[i - 1]] < f)
                i--;
            best.splice(i, 0, id);
            if (best.length > n)
                best.pop();
        }
        return best.map(id => ({word: this.word(id), freq: this._freq[id]}));
    }

    // the likeliest words after @word: [{word, rank}], rank 0 the best
    next(word) {
        let id = this.find(word);
        if (id < 0)
            id = this.find(word.toLowerCase());
        if (id < 0)
            return [];
        const out = [];
        for (let r = 0; r < 3; r++) {
            const next = this._bigrams[3 * id + r];
            if (next !== NONE)
                out.push({word: this.word(next), rank: r});
        }
        return out;
    }
}
