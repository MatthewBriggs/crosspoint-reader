#pragma once

#include <CpDictSdFile.h>
#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "ReaderUtils.h"

class Page;
class Section;

// Modal word-selection state for the EPUB reader: highlights one word on the
// current page (XOR-inverted rect), navigates word-by-word, and looks the
// selected word up in the SD-card dictionary. Owned lazily by
// EpubReaderActivity; all drawing happens synchronously under the caller's
// RenderLock in BW (the grayscale AA pipeline is suspended while active and
// restored by the activity's requestUpdate() on exit).
class WordLookupMode {
 public:
  enum class LoopResult {
    Consumed,  // input handled, mode still active
    Exited,    // mode left; caller should requestUpdate() to restore the page
  };

  // redrawFn re-renders the current page BW (clearScreen + page render +
  // status bar) into the framebuffer without displaying it. Plain function
  // pointer + context per the no-std::function rule.
  struct RedrawFn {
    void* ctx = nullptr;
    void (*fn)(void* ctx) = nullptr;
  };

  WordLookupMode(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& dictionaryPath,
                 const RedrawFn redrawFn)
      : renderer(renderer), mappedInput(mappedInput), dictionaryPath(dictionaryPath), redrawFn(redrawFn) {}

  bool isActive() const { return state != State::Inactive; }

  // Builds the word index from the given page. Returns false (and stays
  // inactive) when the page holds no dictionary-searchable word. Does not
  // draw; the caller re-renders BW and then calls drawHighlight() + display.
  bool enter(Section& section, const Page& page, int fontId, int marginLeft, int marginTop);

  // XOR-inverts the currently selected word's rect into the framebuffer.
  void drawHighlight() const;

  // Per-frame input handling while active. Caller must hold no RenderLock;
  // the mode takes one itself around any drawing.
  LoopResult loop();

 private:
  enum class State { Inactive, Selecting, Definition };

  struct WordRef {
    int16_t x;         // logical screen x of the word (margins applied)
    int16_t y;         // logical screen y of the line top
    int16_t w;         // highlight width in pixels
    uint16_t line;     // ordinal of the PageLine on the page (line navigation)
    uint16_t element;  // index of the PageLine within Page::elements
    uint16_t word;     // word index within the line's TextBlock
  };

  void buildWordIndex(const Page& page, int marginLeft, int marginTop);
  void moveSelection(size_t next);
  void moveLine(int direction);
  void exitToReading();
  void handleSelectingInput();
  void handleDefinitionInput();
  // Fetches the selected word (raw, joined with a trailing line-end hyphen
  // fragment when applicable) by transiently reloading the page.
  std::string selectedRawWord() const;
  // Looks the selected word up (with possessive/plural fallbacks) and opens
  // the definition overlay, or shows a popup on miss / missing dictionary.
  void lookupSelectedWord();
  void drawDefinitionOverlay();
  // Re-renders the BW page + highlight and displays it (popup cleanup and
  // overlay dismissal both land here).
  void redrawPageWithHighlight();

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  const std::string& dictionaryPath;  // owned by EpubReaderActivity
  RedrawFn redrawFn;

  Section* section = nullptr;  // non-owning; valid while active (page turns are blocked)
  int fontId = 0;
  int lineHeight = 0;
  State state = State::Inactive;
  std::vector<WordRef> words;  // selectable words only, reading order
  size_t selected = 0;
  ReaderUtils::DoublePressTracker confirmTracker;

  // Dictionary handle, opened on the first lookup and kept for the mode's
  // lifetime on this page set; closed on exit (member-handle release point).
  CpDictSdFile dict;
  bool dictOpened = false;

  // Definition overlay state; transient, cleared when the overlay closes.
  std::string headword;
  std::vector<std::string> defLines;
  int scrollLine = 0;
  int visibleLines = 0;
  // A popup (miss / missing dictionary) painted over the page; the next
  // interaction must repaint the page instead of taking the XOR fast path.
  bool pageDirty = false;
};
