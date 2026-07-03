#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "Dictionary/CpDictFile.h"
#include "Dictionary/DictKey.h"

// FIXTURE_CPDICT_PATH is defined by CMake; the file is generated at configure
// time from fixtures/mini.tsv by scripts/build_dictionary.py so the binary
// format never diverges from the converter.

namespace {

class FileCpDict final : public CpDictFile {
 public:
  explicit FileCpDict(const char* path) : fp(fopen(path, "rb")) {}
  ~FileCpDict() override {
    if (fp != nullptr) fclose(fp);
  }
  bool opened() const { return fp != nullptr; }

 protected:
  bool readAt(uint32_t offset, void* buf, size_t len) override {
    if (fp == nullptr) return false;
    if (fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) return false;
    return fread(buf, 1, len, fp) == len;
  }

 private:
  FILE* fp;
};

class DictLookupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dict = std::make_unique<FileCpDict>(FIXTURE_CPDICT_PATH);
    ASSERT_TRUE(dict->opened()) << "missing fixture: " << FIXTURE_CPDICT_PATH;
    ASSERT_TRUE(dict->begin());
  }

  std::string lookup(const char* key) {
    char buf[CpDictFile::MAX_DEF_LEN + 1];
    if (!dict->lookup(key, buf, sizeof(buf))) return {};
    return buf;
  }

  std::unique_ptr<FileCpDict> dict;
};

}  // namespace

// Fixture holds 11 entries; see fixtures/mini.tsv.
TEST_F(DictLookupTest, HeaderParses) {
  EXPECT_TRUE(dict->isReady());
  EXPECT_EQ(dict->getEntryCount(), 11u);
}

TEST_F(DictLookupTest, FindsFirstMiddleAndLastKeys) {
  // Sorted order: abandon..zebra — the classic binary-search off-by-one traps.
  EXPECT_NE(lookup("abandon"), "");
  EXPECT_NE(lookup("holmes"), "");
  EXPECT_NE(lookup("zebra"), "");
}

TEST_F(DictLookupTest, MissesAbsentAndBoundaryKeys) {
  EXPECT_EQ(lookup("aardvark"), "");  // sorts before first entry
  EXPECT_EQ(lookup("zzz"), "");       // sorts after last entry
  EXPECT_EQ(lookup("holme"), "");     // prefix of an existing key
  EXPECT_EQ(lookup("holmesx"), "");   // existing key is a prefix of query
  EXPECT_EQ(lookup(""), "");
}

TEST_F(DictLookupTest, MergedDuplicateSensesJoinedWithNewline) {
  // mini.tsv lists "abandon" twice; converter merges with '\n'.
  const std::string def = lookup("abandon");
  EXPECT_NE(def.find("give up completely"), std::string::npos);
  EXPECT_NE(def.find('\n'), std::string::npos);
  EXPECT_NE(def.find("lack of inhibition"), std::string::npos);
}

TEST_F(DictLookupTest, KeysAreCasefoldedByConverter) {
  // "Abbey" and "Über" were written capitalized in the TSV.
  EXPECT_NE(lookup("abbey"), "");
  EXPECT_NE(lookup("\xC3\xBC"
                   "ber"),
            "");                   // "über"
  EXPECT_EQ(lookup("Abbey"), "");  // device queries must be pre-normalized
}

TEST_F(DictLookupTest, Utf8KeysRoundTrip) {
  EXPECT_NE(lookup("caf\xC3\xA9"), "");   // café
  EXPECT_NE(lookup("na\xC3\xAFve"), "");  // naïve
}

TEST_F(DictLookupTest, TruncatesDefinitionToBufferSize) {
  char small[16];
  ASSERT_TRUE(dict->lookup("zebra", small, sizeof(small)));
  EXPECT_EQ(small[15], '\0');
  EXPECT_EQ(std::string(small), std::string("n. an African w").substr(0, 15));
}

// --- dictNormalizeKey ---

TEST(DictKeyTest, StripsEdgePunctuationAndFolds) {
  EXPECT_EQ(dictNormalizeKey("Holmes,"), "holmes");
  EXPECT_EQ(dictNormalizeKey("\"Watson!\""), "watson");
  EXPECT_EQ(dictNormalizeKey("(observation)"), "observation");
  // Curly quotes and em dash
  EXPECT_EQ(dictNormalizeKey("\xE2\x80\x9C"
                             "caf\xC3\xA9\xE2\x80\x9D\xE2\x80\x94"),
            "caf\xC3\xA9");
  // Em-space paragraph indent token followed by a word
  EXPECT_EQ(dictNormalizeKey("\xE2\x80\x83Word"), "word");
}

TEST(DictKeyTest, KeepsInternalPunctuation) {
  EXPECT_EQ(dictNormalizeKey("mother-in-law"), "mother-in-law");
  EXPECT_EQ(dictNormalizeKey("it's"), "it's");
}

TEST(DictKeyTest, FoldsLatinGreekCyrillic) {
  EXPECT_EQ(dictNormalizeKey("\xC3\x9C"
                             "BER"),
            "\xC3\xBC"
            "ber");  // ÜBER -> über
  EXPECT_EQ(dictNormalizeKey("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91"),
            "\xCF\x83\xCE\xBF\xCF\x86\xCE\xB9\xCE\xB1");  // ΣΟΦΙΑ -> σοφια
  EXPECT_EQ(dictNormalizeKey("\xD0\x9C\xD0\xB8\xD1\x80"),
            "\xD0\xBC\xD0\xB8\xD1\x80");  // Мир -> мир
  EXPECT_EQ(dictNormalizeKey("Stra\xC3\x9F"
                             "e"),
            "strasse");  // ß -> ss, matches Python casefold
}

TEST(DictKeyTest, ComposesNfd) {
  // "café" written as NFD (e + combining acute) must match the NFC key.
  EXPECT_EQ(dictNormalizeKey("cafe\xCC\x81"), "caf\xC3\xA9");
}

TEST(DictKeyTest, EmptyForPunctuationOnlyTokens) {
  EXPECT_EQ(dictNormalizeKey("—"), "");
  EXPECT_EQ(dictNormalizeKey("..."), "");
  EXPECT_EQ(dictNormalizeKey(""), "");
}

TEST(DictKeyTest, HasWordContentMatchesNormalizeEmptiness) {
  // dictHasWordContent must agree with dictNormalizeKey's non-emptiness, since
  // buildWordIndex uses the cheap check to decide selectability.
  for (const char* tok : {"Holmes,", "it's", "mother-in-law", "3", "caf\xC3\xA9", "\xE2\x80\x83Word"}) {
    EXPECT_TRUE(dictHasWordContent(tok)) << tok;
    EXPECT_FALSE(dictNormalizeKey(tok).empty()) << tok;
  }
  for (const char* tok : {"", "...", "\xE2\x80\x94", "\xE2\x80\x83", "\"'"}) {
    EXPECT_FALSE(dictHasWordContent(tok)) << tok;
    EXPECT_TRUE(dictNormalizeKey(tok).empty()) << tok;
  }
}

TEST(DictKeyTest, FallbackVariants) {
  EXPECT_EQ(dictKeyStripPossessive("detective's"), "detective");
  EXPECT_EQ(dictKeyStripPossessive("holmes\xE2\x80\x99s"), "holmes");
  EXPECT_EQ(dictKeyStripPossessive("plain"), "");
  EXPECT_EQ(dictKeyStripPluralS("bakers"), "baker");
  EXPECT_EQ(dictKeyStripPluralS("s"), "");
}
