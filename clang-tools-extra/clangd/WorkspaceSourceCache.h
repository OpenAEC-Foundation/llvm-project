//===--- WorkspaceSourceCache.h - Incremental source inventory -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_WORKSPACESOURCECACHE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_WORKSPACESOURCECACHE_H

#include "Headers.h"
#include "support/Path.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem/UniqueID.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace clang {
namespace clangd {

struct WorkspaceSourceFile {
  Path File;
  bool IsHeader = false;

  bool operator==(const WorkspaceSourceFile &Other) const {
    return File == Other.File && IsHeader == Other.IsHeader;
  }
};

struct WorkspaceSourceLimits {
  uint64_t MaxTextFileBytes = 256ULL * 1024 * 1024;
  uint64_t MaxEntries = 4'000'000;
  uint64_t MaxFiles = 2'000'000;
  uint64_t MaxBytesRead = 64ULL * 1024 * 1024 * 1024;
  uint64_t MaxDirectiveScanBytes = 16ULL * 1024 * 1024 * 1024;
};

struct WorkspaceEntryMetadata {
  llvm::sys::fs::UniqueID Identity;
  llvm::sys::TimePoint<> Modified;
  uint64_t Size = 0;

  bool operator==(const WorkspaceEntryMetadata &Other) const {
    return Identity == Other.Identity && Modified == Other.Modified &&
           Size == Other.Size;
  }
};

enum class WorkspaceFileClassification : uint8_t {
  Source,
  PossibleText,
  Binary,
  Metadata,
};

struct WorkspaceFileSnapshot {
  WorkspaceEntryMetadata Metadata;
  WorkspaceFileClassification Classification =
      WorkspaceFileClassification::PossibleText;
  std::optional<FileDigest> Digest;
  std::optional<WorkspaceSourceFile> Source;

  bool operator==(const WorkspaceFileSnapshot &Other) const {
    return Metadata == Other.Metadata &&
           Classification == Other.Classification && Digest == Other.Digest &&
           Source == Other.Source;
  }
};

struct WorkspaceSourceSnapshot {
  std::vector<WorkspaceSourceFile> Sources;
  llvm::StringMap<FileDigest> Digests;
  llvm::StringMap<WorkspaceFileSnapshot> Files;
  llvm::StringMap<WorkspaceEntryMetadata> Directories;
  llvm::StringSet<> PrunedMetadataRoots;
  uint64_t Generation = 0;

  bool operator==(const WorkspaceSourceSnapshot &Other) const;
};

/// Incremental, content-aware inventory of regular workspace files.
///
/// Watched-file and draft events must be passed to invalidate(). Between
/// events, identity, modification time, and size are used to avoid rereading
/// unchanged files. Portable filesystems provide no way to detect an unnotified
/// replacement that preserves identity, modification time, and size. Such a
/// replacement is outside this cache's consistency contract; in-process draft
/// and watched-file changes must synchronously call invalidate().
class WorkspaceSourceCache {
public:
  explicit WorkspaceSourceCache(Path WorkspaceRoot,
                                WorkspaceSourceLimits Limits = {});
  ~WorkspaceSourceCache();

  llvm::Expected<WorkspaceSourceSnapshot> snapshot(llvm::vfs::FileSystem &FS);
  void invalidate(PathRef Path);

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_WORKSPACESOURCECACHE_H
