//===--- FileRename.h - Include edits for file renames ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_FILERENAME_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_FILERENAME_H

#include "Headers.h"
#include "Protocol.h"
#include "support/Path.h"
#include "clang/Format/Format.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <vector>

namespace clang {
class HeaderSearch;
namespace clangd {

struct FileRenameMapping {
  Path OldPath;
  Path NewPath;
  llvm::sys::fs::UniqueID OldIdentity;
};

struct WorkspaceSourceFile {
  Path File;
  bool IsHeader = false;
};

/// Expands directory renames into file renames and validates the complete set.
///
/// Old paths must exist inside WorkspaceRoot. New paths must also be inside the
/// workspace and must not collide with existing files or with another mapping.
llvm::Expected<std::vector<FileRenameMapping>>
expandFileRenames(llvm::ArrayRef<std::pair<Path, Path>> Renames,
                  PathRef WorkspaceRoot, llvm::vfs::FileSystem &FS);

/// Enumerates source and header files whose include directives must be
/// represented in the background include graph.
llvm::Expected<std::vector<WorkspaceSourceFile>>
workspaceSourceFiles(PathRef WorkspaceRoot, llvm::vfs::FileSystem &FS);

struct ConditionalInclusion {
  tok::PPKeywordKind Directive = tok::pp_not_keyword;
  std::string Written;
  unsigned HashOffset = 0;
  int HashLine = 0;
};

/// Returns inclusion directives controlled by genuine preprocessor choices.
/// A conventional whole-file include guard is not considered conditional.
/// Written is empty when the operand is macro-generated.
llvm::Expected<std::vector<ConditionalInclusion>>
conditionalIncludeDirectives(PathRef File, llvm::vfs::FileSystem &FS);

/// Reports whether a file contains any textual inclusion directive.
llvm::Expected<bool> hasIncludeDirectives(PathRef File,
                                          llvm::vfs::FileSystem &FS);

/// Rejects renames that move compiler search/configuration paths.
llvm::Error validateCompileCommandForRenames(
    const tooling::CompileCommand &Command,
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    llvm::ArrayRef<FileRenameMapping> ExpandedRenames = {},
    llvm::vfs::FileSystem *FS = nullptr);

/// Rejects context-dependent edit results. A file must have one unambiguous
/// set of edits across every translation-unit context in which it appears.
llvm::Error validateCompatibleFileRenameEdits(PathRef File,
                                              llvm::ArrayRef<TextEdit> Expected,
                                              llvm::ArrayRef<TextEdit> Actual);

/// Produces edits for direct includes in one parsed file.
///
/// Inclusion identity is established from Inclusion::Resolved and filesystem
/// identity. The source operand must be a literal matching Inclusion::Written;
/// macro-generated or otherwise non-literal operands are rejected.
llvm::Expected<std::vector<TextEdit>> renameIncludeDirectives(
    PathRef File, llvm::StringRef Code, const IncludeStructure &Includes,
    HeaderSearch &HeaderSearchInfo, PathRef BuildDir,
    llvm::ArrayRef<FileRenameMapping> Renames, const format::FormatStyle &Style,
    llvm::vfs::FileSystem &FS);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_FILERENAME_H
