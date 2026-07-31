//===--- WorkspaceSourceCache.cpp - Incremental source inventory ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WorkspaceSourceCache.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Driver/Types.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <mutex>
#include <optional>
#include <system_error>

namespace clang {
namespace clangd {

struct WorkspaceSourceCache::Impl {
  struct Metadata {
    llvm::sys::fs::UniqueID Identity;
    llvm::sys::TimePoint<> Modified;
    uint64_t Size = 0;

    static Metadata fromStatus(const llvm::vfs::Status &S) {
      return {S.getUniqueID(), S.getLastModificationTime(), S.getSize()};
    }
    bool operator==(const Metadata &Other) const {
      return Identity == Other.Identity && Modified == Other.Modified &&
             Size == Other.Size;
    }
  };
  struct CachedFile {
    Metadata Meta;
    FileDigest Digest{{0}};
    std::optional<WorkspaceSourceFile> Source;
  };

  explicit Impl(Path WorkspaceRoot) : Root(std::move(WorkspaceRoot)) {}

  llvm::Expected<bool> containsIncludes(PathRef File, llvm::StringRef Code) {
    // Dependency scanning arbitrary binary payloads is neither useful nor
    // well-defined. A NUL cannot occur in a C-family source file.
    if (Code.contains('\0') ||
        (!Code.contains("include") && !Code.contains("import")))
      return false;
    llvm::SmallVector<dependency_directives_scan::Token> Tokens;
    llvm::SmallVector<dependency_directives_scan::Directive> Directives;
    if (scanSourceForDependencyDirectives(Code, Tokens, Directives))
      return error("cannot scan dependency directives in {0}", File);
    return llvm::any_of(Directives, [](const auto &Directive) {
      using namespace dependency_directives_scan;
      return Directive.Kind == pp_include ||
             Directive.Kind == pp_include_next || Directive.Kind == pp_import;
    });
  }

  llvm::Error refreshFile(PathRef File, const llvm::vfs::Status &Status,
                          llvm::vfs::FileSystem &FS) {
    llvm::SmallString<256> Real;
    if (std::error_code EC = FS.getRealPath(File, Real))
      return error("cannot resolve real path {0}: {1}", File, EC.message());
    if (!pathStartsWith(CanonicalRoot, Real))
      return error("workspace source traverses outside the workspace: {0}",
                   File);
    if (Status.getSize() > MaxWorkspaceSourceFileSize)
      return error("workspace file {0} is {1} bytes, exceeding the {2}-byte "
                   "file-rename inventory limit",
                   File, Status.getSize(), MaxWorkspaceSourceFileSize);
    auto Buffer = FS.getBufferForFile(File);
    if (!Buffer)
      return error("cannot read workspace file {0}: {1}", File,
                   Buffer.getError().message());
    llvm::StringRef Code = Buffer.get()->getBuffer();

    namespace types = clang::driver::types;
    llvm::StringRef Extension = llvm::sys::path::extension(File);
    types::ID Type =
        Extension.empty()
            ? types::TY_INVALID
            : types::lookupTypeForExtension(Extension.drop_front());
    bool IsKnownSource =
        Type != types::TY_INVALID &&
        (types::isSrcFile(Type) || types::onlyPrecompileType(Type));
    bool IsRelevant = IsKnownSource;
    if (!IsRelevant) {
      auto HasIncludes = containsIncludes(File, Code);
      if (!HasIncludes)
        return HasIncludes.takeError();
      IsRelevant = *HasIncludes;
    }

    CachedFile Entry;
    Entry.Meta = Metadata::fromStatus(Status);
    Entry.Digest = digest(Code);
    if (IsRelevant)
      Entry.Source =
          WorkspaceSourceFile{File.str(), /*IsHeader=*/!IsKnownSource ||
                                              types::onlyPrecompileType(Type)};
    Files[File] = std::move(Entry);
    return llvm::Error::success();
  }

  void eraseSubtree(PathRef Prefix) {
    std::vector<Path> FilesToErase;
    for (PathRef File : Files.keys())
      if (pathStartsWith(Prefix, File))
        FilesToErase.push_back(File.str());
    for (PathRef File : FilesToErase)
      Files.erase(File);
    std::vector<Path> DirectoriesToErase;
    for (PathRef Directory : Directories.keys())
      if (pathStartsWith(Prefix, Directory))
        DirectoriesToErase.push_back(Directory.str());
    for (PathRef Directory : DirectoriesToErase)
      Directories.erase(Directory);
  }

  llvm::Error scanSubtree(PathRef Directory, llvm::vfs::FileSystem &FS) {
    eraseSubtree(Directory);
    auto DirectoryStatus = FS.status(Directory);
    if (!DirectoryStatus)
      return error("cannot inspect workspace directory {0}: {1}", Directory,
                   DirectoryStatus.getError().message());
    if (!DirectoryStatus->isDirectory())
      return error("workspace path is not a directory: {0}", Directory);
    Directories[Directory] = Metadata::fromStatus(*DirectoryStatus);

    std::error_code EC;
    llvm::vfs::recursive_directory_iterator It(FS, Directory, EC), End;
    if (EC)
      return error("cannot enumerate workspace {0}: {1}", Directory,
                   EC.message());
    for (; It != End; It.increment(EC)) {
      if (EC)
        return error("cannot enumerate workspace {0}: {1}", Directory,
                     EC.message());
      auto Status = FS.status(It->path());
      if (!Status)
        return error("cannot inspect workspace entry {0}: {1}", It->path(),
                     Status.getError().message());
      if (Status->isDirectory())
        Directories[It->path()] = Metadata::fromStatus(*Status);
      else if (Status->isRegularFile())
        if (auto Err = refreshFile(It->path(), *Status, FS))
          return Err;
    }
    if (EC)
      return error("cannot enumerate workspace {0}: {1}", Directory,
                   EC.message());
    return llvm::Error::success();
  }

  llvm::Error initialize(llvm::vfs::FileSystem &FS) {
    if (!llvm::sys::path::is_absolute(Root))
      return error("workspace path is not absolute: {0}", Root);
    llvm::SmallString<256> Normalized(Root);
    llvm::sys::path::remove_dots(Normalized, /*remove_dot_dot=*/true);
    Root = Normalized.str().str();
    llvm::SmallString<256> Canonical;
    if (std::error_code EC = FS.getRealPath(Root, Canonical))
      return error("cannot resolve workspace root {0}: {1}", Root,
                   EC.message());
    CanonicalRoot = Canonical.str().str();
    if (auto Err = scanSubtree(Root, FS))
      return Err;
    Initialized = true;
    return llvm::Error::success();
  }

  llvm::Error refresh(llvm::vfs::FileSystem &FS) {
    if (!Initialized)
      return initialize(FS);

    std::vector<Path> InvalidatedPaths;
    for (PathRef Path : Invalidated.keys())
      InvalidatedPaths.push_back(Path.str());
    Invalidated.clear();
    for (PathRef Path : InvalidatedPaths) {
      auto Status = FS.status(Path);
      if (!Status) {
        if (Status.getError() == std::errc::no_such_file_or_directory) {
          eraseSubtree(Path);
          continue;
        }
        return error("cannot inspect invalidated workspace path {0}: {1}", Path,
                     Status.getError().message());
      }
      if (Status->isDirectory()) {
        if (auto Err = scanSubtree(Path, FS))
          return Err;
      } else if (Status->isRegularFile()) {
        if (auto Err = refreshFile(Path, *Status, FS))
          return Err;
      } else {
        eraseSubtree(Path);
      }
    }

    std::vector<Path> ChangedDirectories;
    for (const auto &Entry : Directories) {
      auto Status = FS.status(Entry.first());
      if (!Status) {
        if (Status.getError() == std::errc::no_such_file_or_directory) {
          ChangedDirectories.push_back(Entry.first().str());
          continue;
        }
        return error("cannot inspect cached workspace directory {0}: {1}",
                     Entry.first(), Status.getError().message());
      }
      if (!Status->isDirectory() ||
          !(Metadata::fromStatus(*Status) == Entry.getValue()))
        ChangedDirectories.push_back(Entry.first().str());
    }
    llvm::sort(ChangedDirectories,
               [](PathRef L, PathRef R) { return L.size() < R.size(); });
    for (PathRef Directory : ChangedDirectories) {
      if (!Directories.contains(Directory))
        continue;
      auto Status = FS.status(Directory);
      if (!Status) {
        eraseSubtree(Directory);
        continue;
      }
      if (auto Err = scanSubtree(Directory, FS))
        return Err;
    }

    std::vector<Path> CachedFiles;
    for (PathRef File : Files.keys())
      CachedFiles.push_back(File.str());
    for (PathRef File : CachedFiles) {
      auto Existing = Files.find(File);
      if (Existing == Files.end())
        continue;
      auto Status = FS.status(File);
      if (!Status) {
        if (Status.getError() == std::errc::no_such_file_or_directory) {
          Files.erase(File);
          continue;
        }
        return error("cannot inspect cached workspace file {0}: {1}", File,
                     Status.getError().message());
      }
      if (!Status->isRegularFile()) {
        Files.erase(File);
        continue;
      }
      if (!(Metadata::fromStatus(*Status) == Existing->getValue().Meta))
        if (auto Err = refreshFile(File, *Status, FS))
          return Err;
    }
    return llvm::Error::success();
  }

  Path Root;
  Path CanonicalRoot;
  bool Initialized = false;
  llvm::StringMap<CachedFile> Files;
  llvm::StringMap<Metadata> Directories;
  llvm::StringSet<> Invalidated;
  std::mutex Mu;
};

WorkspaceSourceCache::WorkspaceSourceCache(Path WorkspaceRoot)
    : State(std::make_unique<Impl>(std::move(WorkspaceRoot))) {}

WorkspaceSourceCache::~WorkspaceSourceCache() = default;

llvm::Expected<WorkspaceSourceSnapshot>
WorkspaceSourceCache::snapshot(llvm::vfs::FileSystem &FS) {
  std::lock_guard<std::mutex> Lock(State->Mu);
  if (auto Err = State->refresh(FS))
    return std::move(Err);
  WorkspaceSourceSnapshot Result;
  for (const auto &Entry : State->Files) {
    Result.Digests[Entry.first()] = Entry.getValue().Digest;
    if (Entry.getValue().Source)
      Result.Sources.push_back(*Entry.getValue().Source);
  }
  return Result;
}

void WorkspaceSourceCache::invalidate(PathRef Path) {
  std::lock_guard<std::mutex> Lock(State->Mu);
  llvm::SmallString<256> Normalized(Path);
  llvm::sys::path::remove_dots(Normalized, /*remove_dot_dot=*/true);
  State->Invalidated.insert(Normalized);
}

llvm::Expected<std::vector<WorkspaceSourceFile>>
workspaceSourceFiles(PathRef WorkspaceRoot, llvm::vfs::FileSystem &FS) {
  WorkspaceSourceCache Cache(WorkspaceRoot.str());
  auto Snapshot = Cache.snapshot(FS);
  if (!Snapshot)
    return Snapshot.takeError();
  return std::move(Snapshot->Sources);
}

} // namespace clangd
} // namespace clang
