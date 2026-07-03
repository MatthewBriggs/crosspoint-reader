#pragma once

#include <cstddef>
#include <cstdint>

// Reader for the CPD1 dictionary format (see docs/file-formats.md and
// scripts/build_dictionary.py, which must be kept in sync). Lookup is a
// streaming binary search over the on-file index: no in-RAM index is built.
// Keys on file are NFC-normalized, casefolded UTF-8 sorted in memcmp byte
// order; queries must be pre-normalized with dictNormalizeKey().
//
// IO goes through the readAt() seam so the format logic is unit-testable on
// the host; the firmware uses CpDictSdFile (HalFile-backed).
class CpDictFile {
 public:
  static constexpr uint16_t MAX_KEY_LEN = 63;
  static constexpr uint16_t MAX_DEF_LEN = 4096;

  virtual ~CpDictFile() = default;

  // Reads and validates the header. Returns false on malformed files.
  bool begin();
  bool isReady() const { return entryCount > 0; }
  uint32_t getEntryCount() const { return entryCount; }

  // Binary-searches for an exact key match (raw byte comparison). On a hit,
  // copies the definition into defBuf (always NUL-terminated, truncated to
  // defBufSize - 1 bytes) and returns true.
  bool lookup(const char* key, char* defBuf, size_t defBufSize);

 protected:
  // Reads len bytes at absolute file offset. Returns false on short reads.
  virtual bool readAt(uint32_t offset, void* buf, size_t len) = 0;

 private:
  struct IndexRecord {
    uint32_t keyOffset;
    uint32_t defOffset;
    uint16_t keyLen;
    uint16_t defLen;
  };

  bool readIndexRecord(uint32_t idx, IndexRecord& rec);
  // memcmp-style compare of the query against the key of rec: <0 when the
  // query sorts before the record key.
  int compareWithRecordKey(const IndexRecord& rec, const char* key, size_t keyLen);

  uint32_t entryCount = 0;
  uint32_t indexOffset = 0;
  uint32_t keysOffset = 0;
  uint32_t defsOffset = 0;
};
