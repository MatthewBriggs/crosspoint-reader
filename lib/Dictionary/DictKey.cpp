#include "DictKey.h"

#include <Utf8.h>

#include <cctype>
#include <cstdint>
#include <vector>

namespace {

// Codepoints treated as trimmable at word edges. Anything not listed here and
// not ASCII punctuation counts as word content, so unlisted scripts degrade to
// pass-through rather than being eaten.
bool isTrimmableCp(const uint32_t cp) {
  if (cp < 0x80) {
    return !isalnum(static_cast<int>(cp));
  }
  switch (cp) {
    case 0x00A1:  // inverted exclamation
    case 0x00AB:  // «
    case 0x00AD:  // soft hyphen
    case 0x00BB:  // »
    case 0x00BF:  // inverted question
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2018:  // left single quote
    case 0x2019:  // right single quote
    case 0x201A:  // low single quote
    case 0x201C:  // left double quote
    case 0x201D:  // right double quote
    case 0x201E:  // low double quote
    case 0x2026:  // ellipsis
    case 0x2003:  // em space (TextBlock paragraph indent token)
      return true;
    default:
      return false;
  }
}

// Casefolds the ranges the converter's str.casefold() covers for our target
// languages. Unhandled scripts return the codepoint unchanged.
uint32_t foldCp(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
  // Latin-1 Supplement: À-Þ except ×
  if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 0x20;
  // Latin Extended-A: cased pairs alternate upper/lower
  if (cp >= 0x0100 && cp <= 0x0137) return (cp % 2 == 0) ? cp + 1 : cp;
  if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 1) ? cp + 1 : cp;
  if (cp >= 0x014A && cp <= 0x0177) return (cp % 2 == 0) ? cp + 1 : cp;
  if (cp == 0x0178) return 0x00FF;  // Ÿ -> ÿ
  if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? cp + 1 : cp;
  // Greek: Α-Ω (skipping the unassigned 0x03A2), plus final sigma
  if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;
  if (cp == 0x03C2) return 0x03C3;  // ς -> σ, matches Python casefold
  // Cyrillic: А-Я and Ѐ-Џ
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
  return cp;
}

}  // namespace

std::string dictNormalizeKey(const std::string& raw) {
  // Decode once; the codepoint list stays tiny (words, not paragraphs).
  std::vector<uint32_t> cps;
  cps.reserve(raw.size());
  const auto* p = reinterpret_cast<const unsigned char*>(raw.c_str());
  while (*p != '\0') {
    cps.push_back(utf8NextCodepoint(&p));
  }

  size_t first = 0;
  size_t last = cps.size();
  while (first < last && isTrimmableCp(cps[first])) first++;
  while (last > first && isTrimmableCp(cps[last - 1])) last--;
  if (first == last) {
    return {};
  }

  std::string folded;
  folded.reserve(raw.size());
  for (size_t i = first; i < last; i++) {
    const uint32_t cp = foldCp(cps[i]);
    if (cp == 0x00DF) {
      // ß -> "ss", matches Python casefold
      folded += "ss";
      continue;
    }
    utf8AppendCodepoint(cp, folded);
  }
  return utf8ComposeNfc(folded);
}

bool dictHasWordContent(const std::string& raw) {
  const auto* p = reinterpret_cast<const unsigned char*>(raw.c_str());
  while (*p != '\0') {
    if (!isTrimmableCp(utf8NextCodepoint(&p))) {
      return true;
    }
  }
  return false;
}

std::string dictKeyStripPossessive(const std::string& key) {
  // "'s" (2 bytes) or "’s" (U+2019 + s, 4 bytes)
  if (key.size() > 2 && key.compare(key.size() - 2, 2, "'s") == 0) {
    return key.substr(0, key.size() - 2);
  }
  if (key.size() > 4 && key.compare(key.size() - 4, 4, "\xE2\x80\x99s") == 0) {
    return key.substr(0, key.size() - 4);
  }
  return {};
}

std::string dictKeyStripPluralS(const std::string& key) {
  if (key.size() > 1 && key.back() == 's') {
    return key.substr(0, key.size() - 1);
  }
  return {};
}
