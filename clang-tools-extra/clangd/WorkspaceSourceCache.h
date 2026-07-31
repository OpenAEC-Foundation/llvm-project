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
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace clang {
namespace clangd {

struct WorkspaceSourceFile {
  Path File;
  bool IsHeader = false;
};

struct WorkspaceSourceSnapshot {
  std::vector<WorkspaceSourceFile> Sources;
  llvm::StringMap<FileDigest> Digests;
};

/// File-rename inventory must read every regular workspace file to decide
/// whether it can contain dependency directives. Preparation fails rather
/// than silently excluding a file larger than this closed-world proof limit.
inline constexpr uint64_t MaxWorkspaceSourceFileSize = 16 * 1024 * 1024;

/// Incremental, content-aware inventory of regular workspace files.
///
/// Watched-file and draft events must be passed to invalidate(). Between
/// events, identity, modification time, and size are used to avoid rereading
/// unchanged files. An unwatched replacement preserving all three metadata
/// fields cannot be detected without rereading every file.
class WorkspaceSourceCache {
public:
  explicit WorkspaceSourceCache(Path WorkspaceRoot);
  ~WorkspaceSourceCache();

  llvm::Expected<WorkspaceSourceSnapshot> snapshot(llvm::vfs::FileSystem &FS);
  void invalidate(PathRef Path);

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};

/// One-shot inventory for callers that do not own a long-lived cache.
llvm::Expected<std::vector<WorkspaceSourceFile>>
workspaceSourceFiles(PathRef WorkspaceRoot, llvm::vfs::FileSystem &FS);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_WORKSPACESOURCECACHE_H
