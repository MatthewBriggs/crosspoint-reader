# Dictionary Word Lookup

CrossPoint can look up words in an offline dictionary while you read — no
network, no leaving the page. Highlight a word, press to look it up, and a
definition panel slides over the bottom of the page. You can carry several
dictionaries (e.g. English plus a bilingual one for a foreign-language book)
and switch between them in Settings.

This is opt-in: the feature only appears once you put at least one dictionary
file on the SD card. With no dictionary present, the reader behaves exactly as
before and Confirm/Select opens the reader menu with no added latency.

- [Using it](#using-it)
- [Getting a dictionary onto the device](#getting-a-dictionary-onto-the-device)
- [Building a dictionary](#building-a-dictionary)
- [Choosing the active dictionary](#choosing-the-active-dictionary)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)

## Using it

While reading an EPUB:

| Action | Button |
| --- | --- |
| **Enter word lookup** | Double-tap **Select** (Confirm) |
| Move selection to previous / next word | **Left / Right** |
| Move selection up / down a line | **Up / Down** |
| **Look up** the highlighted word | **Select** (single press) |
| Scroll a long definition | **Up / Down** |
| Close the definition (back to word selection) | **Back** |
| Exit word lookup, back to reading | **Back**, or double-tap **Select** |

The currently selected word is drawn inverted (white text on black). When you
look a word up, the definition appears in a panel over the lower part of the
page; a `3/9` indicator in the corner shows your scroll position when the entry
is longer than the panel. Punctuation is trimmed automatically before lookup,
and common inflections are handled with light fallbacks (a trailing `'s` or
plural `s` is retried if the exact word isn't found).

Double-tap detection has a short (300 ms) window. Because a single Select press
is ambiguous until that window closes, the reader menu opens ~300 ms after a
lone Select press *when a dictionary is installed*; without one, it opens
instantly.

## Getting a dictionary onto the device

Dictionary files use the `.cpdict` extension and live in a `/dictionary/`
folder at the root of the SD card:

```
SD card root
├── books/
└── dictionary/
    ├── en-wordnet.cpdict
    └── nb-en-wiktionary.cpdict
```

The files are large (a full English dictionary is ~10 MB), so copy them with a
card reader rather than over the Wi-Fi file transfer. Create the `dictionary`
folder if it doesn't exist, drop the `.cpdict` files in, and eject the card.

Pre-built dictionaries are not distributed with the firmware — you generate
them yourself from a free source (see below). This keeps licensing clean and
lets you pick the languages and size that suit you.

## Building a dictionary

Dictionaries are produced by [`scripts/build_dictionary.py`](../scripts/build_dictionary.py),
a stand-alone Python 3 script with no third-party dependencies. It takes either
a StarDict dictionary or a tab-separated word list and emits a `.cpdict` file.

### From a StarDict dictionary

StarDict `.ifo` / `.idx` / `.dict` (or `.dict.dz`) sets are widely available —
for example the public-domain **Webster's 1913** (GCIDE) English dictionary.

```bash
python3 scripts/build_dictionary.py \
  --stardict path/to/dictd_www.dict.org_web1913 \
  --out en-webster.cpdict --verify
```

`--stardict` takes the base path (without the `.ifo` extension). Webster's-style
sources carry pronunciation respelling markup (`\He\`, `(h[=e])`, `{Him}`) that
is cleaned automatically; pass `--no-gcide-cleanup` to keep it raw.

### From a TSV word list

For anything else, produce a `word<TAB>definition` file (one entry per line;
repeated words are merged) and convert it:

```bash
python3 scripts/build_dictionary.py --tsv words.tsv --out mydict.cpdict --verify
```

This is the easiest path for Wiktionary-derived data. Free per-language
extracts are available from [kaikki.org](https://kaikki.org/) as JSONL; a few
lines of Python turn one into a TSV of headword + gloss. A lean modern English
dictionary can likewise be built from WordNet.

`--verify` re-reads the output and checks that the index is sorted and internally
consistent — worth using the first time you convert a new source.

### Naming

The reader shows dictionary files by filename in the Settings picker, and
defaults to the first one alphabetically. A short prefix keeps things tidy and
predictable, e.g. `en-…`, `nb-…`, `is-…`.

## Choosing the active dictionary

- **One dictionary**: it's used automatically; there's nothing to configure.
- **Two or more**: a **Dictionary** entry appears in **Settings → Reader**
  listing every `.cpdict` on the card. Pick one; the choice is saved and
  persists across reboots and file changes. (The setting is also exposed in the
  web settings interface.)

If the selected dictionary is later removed from the card, the reader falls back
to the first available one.

## How it works

A few notes for the curious; none of this is needed to use the feature.

- **File format.** `.cpdict` (magic `CPD1`) is a compact binary format: a fixed
  header, a sorted fixed-size index, and concatenated key and definition blobs.
  Full layout is in [file-formats.md](file-formats.md). Lookups are a streaming
  binary search directly over the file — no in-RAM index — so even a 150k-entry
  dictionary costs only a handful of small SD reads per lookup and almost no RAM.
- **Keys** are stored NFC-normalized and case-folded, sorted by raw UTF-8 byte
  order, and the device compares queries the same way. The converter and the
  firmware must agree on this ordering, so if you build with a different tool,
  sort by `key.encode("utf-8")`.
- **Rendering.** The selected-word highlight is drawn by inverting the word's
  rectangle in place, so moving between words is a fast local refresh rather
  than a full page redraw. Word positions come from the layout the page was
  already rendered with, so entering lookup doesn't re-measure the page.
- **Memory.** The feature adds no permanent RAM. While lookup is active it uses
  a small per-page word index and, while a definition is open, a page snapshot;
  both are freed as soon as you exit.

## Troubleshooting

**Double-tap does nothing / no Dictionary setting appears.**
There is no `.cpdict` file in `/dictionary/` on the SD card. Double-tap lookup
needs at least one dictionary; the Settings picker needs at least two.

**"No dictionary on SD card" popup.**
The `/dictionary/` folder is missing or empty, or the file couldn't be opened.
Check the path and that the file has a `.cpdict` extension.

**"No definition found."**
The word isn't in the active dictionary (it may be inflected, hyphenated across
a line, archaic, or simply absent). Try a different dictionary, or a more
complete source when building one.

**Lookups seem to pull from the wrong dictionary.**
With multiple files, confirm the selection in **Settings → Reader → Dictionary**.
The default is the alphabetically first file.
