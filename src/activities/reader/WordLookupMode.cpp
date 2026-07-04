#include "WordLookupMode.h"

#include <DictKey.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Extra pixels around the word so the inverted box does not hug the glyphs.
constexpr int HIGHLIGHT_PAD_X = 2;

// Definition overlay geometry.
constexpr int PANEL_BORDER = 2;
constexpr int PANEL_PAD = 10;
constexpr float PANEL_MAX_HEIGHT_RATIO = 0.45f;

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
    const size_t wordCount = lineWords.size();
    for (size_t w = 0; w < wordCount; w++) {
      const std::string& word = lineWords[w];
      // Skip tokens with no dictionary-searchable content (bare punctuation,
      // the synthetic em-space indent). Cheap allocation-free check — the full
      // normalization only runs later, on the one word the user looks up.
      if (!dictHasWordContent(word)) {
        continue;
      }
      const auto style = block.getWordStyle(w);
      const int wordXpos = block.getWordXpos(w);
      int x = line.xPos + wordXpos + marginLeft;

      // Width without per-word font metrics: the layout already placed every
      // token, so the next token's x-position gives this word's advance for
      // free. getTextWidth() is O(glyphs) and, with SD fonts, does SD I/O — at
      // ~300 words/page that measured in whole seconds on device. Fall back to
      // it only where no next token exists (last on line) or the geometry is
      // special (paragraph-indent em-space, half-size SUP/SUB).
      const bool special = startsWithEmSpace(word) || (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
      int width = 0;
      if (w + 1 < wordCount && !special) {
        width = block.getWordXpos(w + 1) - wordXpos;  // word advance incl. trailing space
      }
      if (width <= 0) {
        // Last word on the line, special geometry, or an RTL line where the
        // next token sits to the left (negative delta): pay for the metrics.
        const char* visible = word.c_str();
        if (startsWithEmSpace(word)) {
          // The paragraph-indent em-space is part of the word string but should
          // not be highlighted (mirrors TextBlock's decoration handling).
          x += renderer.getTextAdvanceX(fontId, "\xE2\x80\x83", style);
          visible += 3;
        }
        width = renderer.getTextWidth(fontId, visible, style);
        if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          width = (width + 1) / 2;  // drawText renders SUP/SUB at 50%
        }
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
  if (pageDirty) {
    // A popup was painted over the page; the XOR fast path would move the
    // highlight through stale pixels. Repaint fully instead.
    selected = next;
    redrawPageWithHighlight();
    return;
  }
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
  headword.clear();
  defLines.clear();
  defLines.shrink_to_fit();
  if (dictOpened) {
    dict.close();
    dictOpened = false;
  }
  // Normally the snapshot is already freed by closing the definition overlay;
  // free it defensively so an unexpected exit path can't leak ~48KB.
  if (pageSnapshotted) {
    renderer.restoreBwBuffer();
    pageSnapshotted = false;
  }
  pageDirty = false;
}

void WordLookupMode::redrawPageWithHighlight() {
  if (redrawFn.fn != nullptr) {
    redrawFn.fn(redrawFn.ctx);
  }
  drawHighlight();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  pageDirty = false;
}

void WordLookupMode::lookupSelectedWord() {
  if (!dictOpened) {
    if (!dict.open(dictionaryPath)) {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_DICT_NOT_FOUND));
      pageDirty = true;
      return;
    }
    dictOpened = true;
  }

  const std::string raw = selectedRawWord();
  const std::string key = dictNormalizeKey(raw);
  if (key.empty()) {
    return;
  }

  const auto defBuf = makeUniqueNoThrow<char[]>(CpDictFile::MAX_DEF_LEN + 1);
  if (!defBuf) {
    LOG_ERR("WLM", "OOM: definition buffer");
    return;
  }

  // Exact key, then cheap morphological fallbacks.
  std::string matched = key;
  bool found = dict.lookup(key.c_str(), defBuf.get(), CpDictFile::MAX_DEF_LEN + 1);
  if (!found) {
    const std::string noPossessive = dictKeyStripPossessive(key);
    if (!noPossessive.empty() && dict.lookup(noPossessive.c_str(), defBuf.get(), CpDictFile::MAX_DEF_LEN + 1)) {
      matched = noPossessive;
      found = true;
    }
  }
  if (!found) {
    const std::string noPlural = dictKeyStripPluralS(key);
    if (!noPlural.empty() && dict.lookup(noPlural.c_str(), defBuf.get(), CpDictFile::MAX_DEF_LEN + 1)) {
      matched = noPlural;
      found = true;
    }
  }

  if (!found) {
    LOG_DBG("WLM", "No definition: '%s'", key.c_str());
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DEFINITION));
    pageDirty = true;
    return;
  }

  // Wrap the definition into overlay lines. wrappedText splits on spaces
  // only, so split on '\n' first (senses are '\n'-joined by the converter).
  headword = matched;
  defLines.clear();
  int t, r, b, l;
  renderer.getOrientedViewableTRBL(&t, &r, &b, &l);
  const int panelW = renderer.getScreenWidth() - l - r;
  const int wrapW = panelW - 2 * (PANEL_BORDER + PANEL_PAD);
  defLines.reserve(strlen(defBuf.get()) / 24 + 4);
  const char* seg = defBuf.get();
  while (seg != nullptr && *seg != '\0') {
    const char* nl = strchr(seg, '\n');
    std::string segment = nl != nullptr ? std::string(seg, nl - seg) : std::string(seg);
    if (segment.empty()) {
      defLines.emplace_back();
    } else {
      // maxLines is generous; scrolling handles the overflow.
      auto wrapped = renderer.wrappedText(UI_12_FONT_ID, segment.c_str(), wrapW, 512);
      for (auto& line : wrapped) {
        defLines.push_back(std::move(line));
      }
    }
    seg = nl != nullptr ? nl + 1 : nullptr;
  }
  if (defLines.empty()) {
    defLines.emplace_back();
  }

  scrollLine = 0;
  state = State::Definition;
  RenderLock lock;
  // Snapshot the clean page+highlight before the panel covers it, so closing
  // the definition is a buffer restore + refresh instead of an SD reload and
  // full re-render. (A prior miss-popup could still be on screen; repaint the
  // page first so the snapshot is clean.)
  if (pageDirty) {
    if (redrawFn.fn != nullptr) {
      redrawFn.fn(redrawFn.ctx);
    }
    drawHighlight();
    pageDirty = false;
  }
  pageSnapshotted = renderer.storeBwBuffer();
  drawDefinitionOverlay();
}

void WordLookupMode::drawDefinitionOverlay() {
  int t, r, b, l;
  renderer.getOrientedViewableTRBL(&t, &r, &b, &l);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int panelW = screenW - l - r;

  const int uiLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int headerH = uiLineH + PANEL_PAD;  // headword row incl. gap below

  const int maxPanelH = static_cast<int>(screenH * PANEL_MAX_HEIGHT_RATIO);
  const int neededH = 2 * (PANEL_BORDER + PANEL_PAD) + headerH + static_cast<int>(defLines.size()) * uiLineH;
  const int panelH = neededH < maxPanelH ? neededH : maxPanelH;
  const int panelX = l;
  const int panelY = screenH - b - panelH;

  visibleLines = (panelH - 2 * (PANEL_BORDER + PANEL_PAD) - headerH) / uiLineH;
  if (visibleLines < 1) visibleLines = 1;
  const int maxScroll = static_cast<int>(defLines.size()) - visibleLines;
  if (scrollLine > maxScroll) scrollLine = maxScroll < 0 ? 0 : maxScroll;

  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH, PANEL_BORDER, true);

  const int textX = panelX + PANEL_BORDER + PANEL_PAD;
  int y = panelY + PANEL_BORDER + PANEL_PAD;
  renderer.drawText(UI_12_FONT_ID, textX, y, headword.c_str(), true, EpdFontFamily::BOLD);

  // Scroll indicator (e.g. "3/9") in the top-right corner when overflowing.
  if (static_cast<int>(defLines.size()) > visibleLines) {
    char indicator[24];
    snprintf(indicator, sizeof(indicator), "%d/%d", scrollLine + visibleLines, static_cast<int>(defLines.size()));
    const int indicatorW = renderer.getTextWidth(UI_10_FONT_ID, indicator);
    renderer.drawText(UI_10_FONT_ID, panelX + panelW - PANEL_BORDER - PANEL_PAD - indicatorW, y, indicator, true);
  }
  y += headerH;

  for (int i = 0; i < visibleLines; i++) {
    const size_t lineIdx = static_cast<size_t>(scrollLine) + i;
    if (lineIdx >= defLines.size()) break;
    if (!defLines[lineIdx].empty()) {
      renderer.drawText(UI_12_FONT_ID, textX, y, defLines[lineIdx].c_str(), true);
    }
    y += uiLineH;
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
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
      handleDefinitionInput();
      return LoopResult::Consumed;
  }
  return LoopResult::Consumed;
}

void WordLookupMode::handleDefinitionInput() {
  // Back dismisses the overlay back to word selection.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state = State::Selecting;
    headword.clear();
    defLines.clear();
    defLines.shrink_to_fit();
    RenderLock lock;
    if (pageSnapshotted) {
      // Restore the page+highlight captured when the panel opened — no SD
      // reload, no re-render; just a framebuffer copy.
      renderer.restoreBwBuffer();
      pageSnapshotted = false;
    } else if (redrawFn.fn != nullptr) {
      redrawFn.fn(redrawFn.ctx);
      drawHighlight();
    }
    // The definition panel is a large high-contrast box; a FAST_REFRESH would
    // leave a ghost of it. Dismiss with HALF_REFRESH to fully clear the area —
    // the same balanced refresh the reader uses to clear page-turn ghosting.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Up/Down scroll by a near-full panel.
  const int step = visibleLines > 1 ? visibleLines - 1 : 1;
  const int maxScroll = static_cast<int>(defLines.size()) - visibleLines;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) && scrollLine > 0) {
    scrollLine = scrollLine > step ? scrollLine - step : 0;
    RenderLock lock;
    drawDefinitionOverlay();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && scrollLine < maxScroll) {
    scrollLine += step;
    RenderLock lock;
    drawDefinitionOverlay();
  }
}

void WordLookupMode::handleSelectingInput() {
  // Back always leaves the mode.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitToReading();
    return;
  }

  // Confirm: single = dictionary lookup, double = exit (mirrors mode entry).
  // Double fires on the second press; a lone press looks up once the window closes.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && confirmTracker.consumeSecondPress(millis())) {
    exitToReading();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmTracker.arm(millis());
  }
  if (confirmTracker.consumeExpired(millis())) {
    lookupSelectedWord();
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
