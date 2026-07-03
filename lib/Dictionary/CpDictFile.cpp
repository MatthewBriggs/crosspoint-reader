#include "CpDictFile.h"

#include <cstring>

namespace {
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t INDEX_RECORD_SIZE = 12;
constexpr char MAGIC[4] = {'C', 'P', 'D', '1'};

// All multi-byte fields are little-endian on file, which matches both the
// ESP32-C3 and the simulator hosts; memcpy avoids unaligned loads (RISC-V).
uint32_t readU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

uint16_t readU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
}  // namespace

bool CpDictFile::begin() {
  entryCount = 0;

  uint8_t header[HEADER_SIZE];
  if (!readAt(0, header, sizeof(header))) {
    return false;
  }
  if (memcmp(header, MAGIC, sizeof(MAGIC)) != 0) {
    return false;
  }

  const uint32_t count = readU32(header + 4);
  indexOffset = readU32(header + 8);
  keysOffset = readU32(header + 12);
  defsOffset = readU32(header + 16);

  // Offsets must be monotonic and consistent with the record count, otherwise
  // the file is corrupt and binary-search reads could run wild.
  if (count == 0 || indexOffset != HEADER_SIZE || keysOffset != indexOffset + count * INDEX_RECORD_SIZE ||
      defsOffset < keysOffset) {
    return false;
  }

  entryCount = count;
  return true;
}

bool CpDictFile::readIndexRecord(const uint32_t idx, IndexRecord& rec) {
  uint8_t raw[INDEX_RECORD_SIZE];
  if (!readAt(indexOffset + idx * INDEX_RECORD_SIZE, raw, sizeof(raw))) {
    return false;
  }
  rec.keyOffset = readU32(raw);
  rec.defOffset = readU32(raw + 4);
  rec.keyLen = readU16(raw + 8);
  rec.defLen = readU16(raw + 10);
  return rec.keyLen <= MAX_KEY_LEN;
}

int CpDictFile::compareWithRecordKey(const IndexRecord& rec, const char* key, const size_t keyLen) {
  char recKey[MAX_KEY_LEN];
  if (!readAt(keysOffset + rec.keyOffset, recKey, rec.keyLen)) {
    // Fail towards "not found": pretend the query sorts before the corrupt
    // record so the search terminates.
    return -1;
  }
  const size_t common = keyLen < rec.keyLen ? keyLen : rec.keyLen;
  const int c = memcmp(key, recKey, common);
  if (c != 0) {
    return c;
  }
  if (keyLen == rec.keyLen) {
    return 0;
  }
  return keyLen < rec.keyLen ? -1 : 1;
}

bool CpDictFile::lookup(const char* key, char* defBuf, const size_t defBufSize) {
  if (!isReady() || key == nullptr || defBuf == nullptr || defBufSize == 0) {
    return false;
  }
  const size_t keyLen = strlen(key);
  if (keyLen == 0 || keyLen > MAX_KEY_LEN) {
    return false;
  }

  uint32_t lo = 0;
  uint32_t hi = entryCount;  // half-open [lo, hi)
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    IndexRecord rec;
    if (!readIndexRecord(mid, rec)) {
      return false;
    }
    const int c = compareWithRecordKey(rec, key, keyLen);
    if (c == 0) {
      size_t defLen = rec.defLen;
      if (defLen > defBufSize - 1) {
        defLen = defBufSize - 1;
      }
      if (defLen > 0 && !readAt(defsOffset + rec.defOffset, defBuf, defLen)) {
        return false;
      }
      defBuf[defLen] = '\0';
      return true;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}
