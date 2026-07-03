#pragma once

#include <string>

// Normalizes a word plucked from a rendered page into a CPD1 lookup key:
// strips surrounding punctuation/quotes/soft hyphens, casefolds (ASCII,
// Latin-1, Latin Extended-A, Greek, Cyrillic; other scripts pass through)
// and NFC-composes. Mirrors normalize_key() in scripts/build_dictionary.py —
// keep the two in sync or lookups silently miss.
// Returns an empty string for tokens with no word content (bare punctuation).
std::string dictNormalizeKey(const std::string& raw);

// Cheap, allocation-free predicate: true when the token has any word content
// (a letter/digit codepoint), i.e. exactly when dictNormalizeKey() would return
// non-empty. Used to filter selectable words on a page without paying the full
// normalization cost per word.
bool dictHasWordContent(const std::string& raw);

// Fallback variants tried in order when the exact key misses:
// "word's"/"word’s" -> "word", then trailing "s" -> "" (naive plural).
// Returns an empty string when the variant does not apply.
std::string dictKeyStripPossessive(const std::string& key);
std::string dictKeyStripPluralS(const std::string& key);
