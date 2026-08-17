//===--- FileRenameInternal.h - Shared file rename helpers ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_FILERENAMEINTERNAL_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_FILERENAMEINTERNAL_H

#include "FileRename.h"
#include "support/Path.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cstdint>
#include <limits>

namespace clang {
namespace clangd {

llvm::Expected<Path> fileRenameCanonicalPath(PathRef Path,
                                             llvm::vfs::FileSystem &FS);

inline bool fileRenamePrunesDirectory(PathRef Path) {
  llvm::StringRef Name = llvm::sys::path::filename(Path);
  return Name == ".git" || Name == ".hg" || Name == ".svn";
}

/// Deduplicates directive scans by canonical path within one rename request.
class FileRenameDirectiveCache {
public:
  explicit FileRenameDirectiveCache(llvm::vfs::FileSystem &FS) : FS(FS) {}

  /// Scans at most MaxBytes, rejecting larger files before opening them.
  llvm::Expected<const FileRenameDirectiveScan *>
  scan(PathRef File, FileDigest ExpectedDigest,
       uint64_t MaxBytes = std::numeric_limits<uint64_t>::max());

private:
  struct CachedScan {
    FileDigest Digest{{0}};
    FileRenameDirectiveScan Scan;
  };

  llvm::vfs::FileSystem &FS;
  llvm::StringMap<CachedScan> Scans;
};

} // namespace clangd
} // namespace clang

#endif
