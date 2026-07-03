#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "CpDictFile.h"

// HalFile-backed CPD1 reader used on the device and in the simulator. The
// file handle is a member so repeated lookups reuse one open file; it is
// released via close() at the owner's release point (or by the destructor).
class CpDictSdFile final : public CpDictFile {
 public:
  // Opens the file and validates the header. Returns false when the file is
  // missing or malformed (already logged).
  bool open(const std::string& path);
  void close() { file.close(); }

  // Returns the file names (not paths) of all *.cpdict files in /dictionary,
  // sorted alphabetically. Empty when the directory is missing or empty.
  static std::vector<std::string> listDictionaries();

  // Returns the path of the dictionary to use: preferredName when it exists
  // in /dictionary, otherwise the first file alphabetically, otherwise an
  // empty string. preferredName may be null or empty (no preference).
  static std::string findDictionary(const char* preferredName);

 protected:
  bool readAt(uint32_t offset, void* buf, size_t len) override;

 private:
  HalFile file;
};
