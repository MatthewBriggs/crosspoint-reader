#include "CpDictSdFile.h"

#include <Logging.h>

#include <algorithm>

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

std::vector<std::string> CpDictSdFile::listDictionaries() {
  std::vector<std::string> names;
  HalFile dir = Storage.open(DICTIONARY_DIR);
  if (!dir || !dir.isDirectory()) {
    return names;
  }

  char name[128];
  HalFile entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      entry.getName(name, sizeof(name));
      if (name[0] != '.' && hasCpdictExtension(name)) {
        names.emplace_back(name);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  std::sort(names.begin(), names.end());
  return names;
}

std::string CpDictSdFile::findDictionary(const char* preferredName) {
  const auto names = listDictionaries();
  if (names.empty()) {
    return {};
  }
  if (preferredName != nullptr && preferredName[0] != '\0') {
    for (const auto& name : names) {
      if (name == preferredName) {
        return std::string(DICTIONARY_DIR) + "/" + name;
      }
    }
    // Preferred file was removed from the card — fall back to the first.
  }
  return std::string(DICTIONARY_DIR) + "/" + names.front();
}
