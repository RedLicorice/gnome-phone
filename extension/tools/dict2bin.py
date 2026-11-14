#!/usr/bin/env python3
"""HeliBoard word lists -> the touch keyboard's dictionaries (hkdicts/<language>.dict, read by hkeyboard/dictionary.js).

Input: the text word lists of https://codeberg.org/Helium314/aosp-dictionaries (wordlists/main_<lang>.combined):
a header line, then ` word=<word>,f=<0-255>` lines, each followed by up to three `  bigram=<next>,f=<rank>` lines.
The lists are built from the Leipzig Wortschatz corpora, CC BY 4.0 (see hkdicts/ATTRIBUTION).

Output, little endian, one file per language:
    char[4] magic 'HKD1'
    u32     count               words, at most 65535 (bigram ids are u16)
    u32     offsets_at          count + 1 u32: each word's start in the strings, then the end of the last one
    u32     freq_at             count u8: the list's frequency, 0-255 (log scale)
    u32     bigrams_at          count * 3 u16: the ids of the next words, best first; 0xffff for none
    u32     strings_at          the words, UTF-8, back to back
The words are sorted by their folded form (decomposed, marks dropped, lower case) in UTF-16 order, the order of
JavaScript's string comparison, so a typed prefix finds its words with a binary search ("perche" finds "perché").

usage: dict2bin.py <main_xx.combined> <out.dict> [--words N]
"""
import argparse
import re
import struct
import unicodedata

LINE = re.compile(r'^ word=(.*),f=(\d+)')
BIGRAM = re.compile(r'^  bigram=(.*),f=(\d+)')
MAX_WORDS = 0xffff - 1


def fold(word):
    """the key a typed prefix is compared with: hkeyboard/dictionary.js fold() does the same"""
    return ''.join(c for c in unicodedata.normalize('NFD', word) if unicodedata.category(c) != 'Mn').lower()


def read(path):
    words = []  # [word, freq, [(rank, next)]]
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = LINE.match(line)
            if m:
                words.append([m.group(1), int(m.group(2)), []])
                continue
            m = BIGRAM.match(line)
            if m and words:
                words[-1][2].append((int(m.group(2)), m.group(1)))
    return words


def convert(src, dst, limit):
    words = read(src)
    words = [w for w in words if w[0] and ' ' not in w[0] and len(w[0]) <= 40]
    # the most frequent ones; the list's own order breaks ties (it is sorted by frequency already)
    order = sorted(range(len(words)), key=lambda i: -words[i][1])[:min(limit, MAX_WORDS)]
    kept = [words[i] for i in order]
    kept.sort(key=lambda w: (fold(w[0]).encode('utf-16-be'), w[0]))
    ids = {}
    for i, w in enumerate(kept):
        ids.setdefault(w[0], i)

    blob = bytearray()
    offsets = []
    for w in kept:
        offsets.append(len(blob))
        blob += w[0].encode('utf-8')
    offsets.append(len(blob))

    bigrams = []
    for w in kept:
        nexts = [ids[n] for _, n in sorted(w[2]) if n in ids][:3]
        bigrams += nexts + [0xffff] * (3 - len(nexts))

    header = 4 + 5 * 4
    offsets_at = header
    freq_at = offsets_at + 4 * len(offsets)
    bigrams_at = freq_at + len(kept)
    bigrams_at += bigrams_at % 2
    strings_at = bigrams_at + 2 * len(bigrams)
    out = bytearray(b'HKD1')
    out += struct.pack('<5I', len(kept), offsets_at, freq_at, bigrams_at, strings_at)
    out += struct.pack(f'<{len(offsets)}I', *offsets)
    out += bytes(w[1] for w in kept)
    out += b'\0' * (bigrams_at - len(out))
    out += struct.pack(f'<{len(bigrams)}H', *bigrams)
    out += blob
    with open(dst, 'wb') as f:
        f.write(out)
    print(f'{dst}: {len(kept)} words, {len(out)} bytes')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('src')
    p.add_argument('dst')
    p.add_argument('--words', type=int, default=60000)
    a = p.parse_args()
    convert(a.src, a.dst, a.words)
