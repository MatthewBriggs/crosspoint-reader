#pragma once

#include <HalStorage.h>

#include <string>

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

  // Returns the path of the first *.cpdict file (alphabetical) in /dictionary,
  // or an empty string when none exists.
  static std::string findFirstDictionary();

 protected:
  bool readAt(uint32_t offset, void* buf, size_t len) override;

 private:
  HalFile file;
};
