#include "WordLookupMode.h"

#include <DictKey.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <Logging.h>

#include "activities/RenderLock.h"

namespace {

// Extra pixels around the word so the inverted box does not hug the glyphs.
constexpr int HIGHLIGHT_PAD_X = 2;

bool startsWithEmSpace(const std::string& word) {
  return word.size() >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
         static_cast<uint8_t>(word[2]) == 0x83;
}

bool endsWithHyphen(const std::string& word) {
  if (word.empty()) return false;
  if (word.back() == '-') return true;
  // Soft hyphen U+00AD (C2 AD)
  return word.size() >= 2 && static_cast<uint8_t>(word[word.size() - 2]) == 0xC2 &&
         static_cast<uint8_t>(word.back()) == 0xAD;
}

}  // namespace

bool WordLookupMode::enter(Section& sec, const Page& page, const int fontIdIn, const int marginLeft,
                           const int marginTop) {
  section = &sec;
  fontId = fontIdIn;
  lineHeight = renderer.getLineHeight(fontId);
  buildWordIndex(page, marginLeft, marginTop);
  if (words.empty()) {
    section = nullptr;
    return false;
  }
  selected = 0;
  confirmTracker.reset();
  state = State::Selecting;
  return true;
}

void WordLookupMode::buildWordIndex(const Page& page, const int marginLeft, const int marginTop) {
  words.clear();

  size_t total = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() == TAG_PageLine) {
      total += static_cast<const PageLine&>(*el).getBlock()->wordCount();
    }
  }
  words.reserve(total);

  uint16_t lineOrdinal = 0;
  for (size_t elIdx = 0; elIdx < page.elements.size(); elIdx++) {
    const auto& el = page.elements[elIdx];
    if (el->getTag() != TAG_PageLine) {
      continue;
    }
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& block = *line.getBlock();
    const auto& lineWords = block.getWords();
    for (size_t w = 0; w < lineWords.size(); w++) {
      const std::string& word = lineWords[w];
      // Skip tokens with no dictionary-searchable content (bare punctuation,
      // the synthetic em-space indent).
      if (dictNormalizeKey(word).empty()) {
        continue;
      }
      const auto style = block.getWordStyle(w);
      int x = line.xPos + block.getWordXpos(w) + marginLeft;
      const char* visible = word.c_str();
      if (startsWithEmSpace(word)) {
        // The paragraph-indent em-space is part of the word string but should
        // not be highlighted (mirrors TextBlock's decoration handling).
        x += renderer.getTextAdvanceX(fontId, "\xE2\x80\x83", style);
        visible += 3;
      }
      int width = renderer.getTextWidth(fontId, visible, style);
      if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        width = (width + 1) / 2;  // drawText renders SUP/SUB at 50%
      }
      if (width <= 0) {
        continue;
      }
      WordRef ref;
      ref.x = static_cast<int16_t>(x);
      ref.y = static_cast<int16_t>(line.yPos + marginTop);
      ref.w = static_cast<int16_t>(width);
      ref.line = lineOrdinal;
      ref.element = static_cast<uint16_t>(elIdx);
      ref.word = static_cast<uint16_t>(w);
      words.push_back(ref);
    }
    lineOrdinal++;
  }
}

void WordLookupMode::drawHighlight() const {
  if (words.empty()) {
    return;
  }
  const WordRef& ref = words[selected];
  renderer.invertRect(ref.x - HIGHLIGHT_PAD_X, ref.y, ref.w + 2 * HIGHLIGHT_PAD_X, lineHeight);
}

void WordLookupMode::moveSelection(const size_t next) {
  if (next == selected || next >= words.size()) {
    return;
  }
  RenderLock lock;
  drawHighlight();  // XOR is self-inverse: un-draws the current highlight
  selected = next;
  drawHighlight();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void WordLookupMode::moveLine(const int direction) {
  const WordRef& cur = words[selected];
  // Find the nearest selectable word on the closest distinct line in the
  // given direction, minimizing horizontal center distance.
  const int curCenter = cur.x + cur.w / 2;
  int bestLine = -1;
  size_t bestIdx = selected;
  int bestDist = 0;
  for (size_t i = 0; i < words.size(); i++) {
    const WordRef& cand = words[i];
    if (direction > 0 ? cand.line <= cur.line : cand.line >= cur.line) {
      continue;
    }
    const int lineDist = direction > 0 ? cand.line - cur.line : cur.line - cand.line;
    if (bestLine != -1 && lineDist > bestLine) {
      continue;
    }
    const int center = cand.x + cand.w / 2;
    const int dist = center > curCenter ? center - curCenter : curCenter - center;
    if (bestLine == -1 || lineDist < bestLine || dist < bestDist) {
      bestLine = lineDist;
      bestIdx = i;
      bestDist = dist;
    }
  }
  moveSelection(bestIdx);
}

void WordLookupMode::exitToReading() {
  state = State::Inactive;
  section = nullptr;
  words.clear();
  words.shrink_to_fit();  // release the index; mode may stay allocated for the book
}

std::string WordLookupMode::selectedRawWord() const {
  if (section == nullptr || words.empty()) {
    return {};
  }
  // Transient reload: the Page is not retained while the mode is active (heap
  // discipline) — this is the same SD read every page turn already does.
  const auto page = section->loadPageFromSectionFile();
  if (!page) {
    return {};
  }
  const WordRef& ref = words[selected];
  if (ref.element >= page->elements.size()) {
    return {};
  }
  const auto& el = *page->elements[ref.element];
  if (el.getTag() != TAG_PageLine) {
    return {};
  }
  const auto& block = *static_cast<const PageLine&>(el).getBlock();
  if (ref.word >= block.wordCount()) {
    return {};
  }
  std::string word = block.getWords()[ref.word];

  // A line-final hyphen usually means the word continues on the next line;
  // join the fragment so "obser-" + "vation" looks up as "observation". A
  // misfire only costs a missed lookup.
  const bool lastOnLine = ref.word + 1 == block.wordCount();
  if (lastOnLine && endsWithHyphen(word) && selected + 1 < words.size()) {
    const WordRef& next = words[selected + 1];
    if (next.line == ref.line + 1 && next.element < page->elements.size()) {
      const auto& nextEl = *page->elements[next.element];
      if (nextEl.getTag() == TAG_PageLine) {
        const auto& nextBlock = *static_cast<const PageLine&>(nextEl).getBlock();
        if (next.word < nextBlock.wordCount()) {
          // Strip the trailing (soft) hyphen before joining.
          word.erase(word.size() - (word.back() == '-' ? 1 : 2));
          word += nextBlock.getWords()[next.word];
        }
      }
    }
  }
  return word;
}

WordLookupMode::LoopResult WordLookupMode::loop() {
  switch (state) {
    case State::Inactive:
      return LoopResult::Exited;
    case State::Selecting:
      handleSelectingInput();
      return state == State::Inactive ? LoopResult::Exited : LoopResult::Consumed;
    case State::Definition:
      // Implemented with the definition overlay.
      state = State::Selecting;
      return LoopResult::Consumed;
  }
  return LoopResult::Consumed;
}

void WordLookupMode::handleSelectingInput() {
  // Back always leaves the mode.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitToReading();
    return;
  }

  // Confirm: single = dictionary lookup, double = exit (mirrors mode entry).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmTracker.onRelease(millis()) == ReaderUtils::DoublePressTracker::Event::Double) {
      exitToReading();
      return;
    }
  }
  if (confirmTracker.poll(millis()) == ReaderUtils::DoublePressTracker::Event::Single) {
    const std::string raw = selectedRawWord();
    LOG_DBG("WLM", "Lookup requested: '%s'", raw.c_str());
    // Definition overlay lands in the next change; lookup wiring follows it.
    return;
  }

  // Front buttons step one word; honor the same swap setting as page turns.
  const bool swap = mappedInput.isNavDirectionSwapped();
  const auto prevButton = swap ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swap ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  if (mappedInput.wasPressed(prevButton) && selected > 0) {
    moveSelection(selected - 1);
    return;
  }
  if (mappedInput.wasPressed(nextButton) && selected + 1 < words.size()) {
    moveSelection(selected + 1);
    return;
  }

  // Side buttons jump a line.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveLine(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveLine(1);
  }
}
