#include "CpDictSdFile.h"

#include <Logging.h>

namespace {
constexpr const char* DICTIONARY_DIR = "/dictionary";

bool hasCpdictExtension(const char* name) {
  const char* dot = strrchr(name, '.');
  return dot != nullptr && strcasecmp(dot, ".cpdict") == 0;
}
}  // namespace

bool CpDictSdFile::open(const std::string& path) {
  if (!Storage.openFileForRead("DIC", path, file)) {
    return false;
  }
  if (!begin()) {
    LOG_ERR("DIC", "Malformed dictionary: %s", path.c_str());
    file.close();
    return false;
  }
  LOG_DBG("DIC", "Opened %s (%u entries)", path.c_str(), static_cast<unsigned>(getEntryCount()));
  return true;
}

bool CpDictSdFile::readAt(const uint32_t offset, void* buf, const size_t len) {
  if (!file.seek64(offset)) {
    return false;
  }
  return file.read(buf, len) == static_cast<int>(len);
}

std::string CpDictSdFile::findFirstDictionary() {
  HalFile dir = Storage.open(DICTIONARY_DIR);
  if (!dir || !dir.isDirectory()) {
    return {};
  }

  char name[128];
  std::string best;
  HalFile entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      entry.getName(name, sizeof(name));
      if (name[0] != '.' && hasCpdictExtension(name)) {
        if (best.empty() || strcmp(name, best.c_str()) < 0) {
          best = name;
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (best.empty()) {
    return {};
  }
  return std::string(DICTIONARY_DIR) + "/" + best;
}
