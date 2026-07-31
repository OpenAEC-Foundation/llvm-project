//===--- WorkspaceSourceCache.cpp - Incremental source inventory ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WorkspaceSourceCache.h"
#include "FileRenameInternal.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Driver/Types.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <limits>
#include <mutex>
#include <system_error>

namespace clang {
namespace clangd {
namespace {

template <typename T>
bool mapsEqual(const llvm::StringMap<T> &L, const llvm::StringMap<T> &R) {
  if (L.size() != R.size())
    return false;
  for (const auto &Entry : L) {
    auto It = R.find(Entry.first());
    if (It == R.end() || !(Entry.getValue() == It->getValue()))
      return false;
  }
  return true;
}

bool isUnderVCSMetadata(PathRef Root, PathRef Path) {
  if (!pathStartsWith(Root, Path))
    return false;
  llvm::StringRef Relative = Path.drop_front(Root.size());
  for (llvm::sys::path::const_iterator I = llvm::sys::path::begin(Relative),
                                       E = llvm::sys::path::end(Relative);
       I != E; ++I)
    if (fileRenamePrunesDirectory(*I))
      return true;
  return false;
}

Path normalizedPath(Path Path) {
  llvm::SmallString<256> Normalized(Path);
  llvm::sys::path::remove_dots(Normalized, /*remove_dot_dot=*/true);
  return Normalized.str().str();
}

} // namespace

bool WorkspaceSourceSnapshot::operator==(
    const WorkspaceSourceSnapshot &Other) const {
  if (Generation != Other.Generation || Sources != Other.Sources ||
      !mapsEqual(Digests, Other.Digests) || !mapsEqual(Files, Other.Files) ||
      !mapsEqual(Directories, Other.Directories) ||
      PrunedMetadataRoots.size() != Other.PrunedMetadataRoots.size())
    return false;
  for (PathRef Root : PrunedMetadataRoots.keys())
    if (!Other.PrunedMetadataRoots.contains(Root))
      return false;
  return true;
}

struct WorkspaceSourceCache::Impl {
  struct CachedFile {
    WorkspaceEntryMetadata Meta;
    WorkspaceFileClassification Classification =
        WorkspaceFileClassification::PossibleText;
    std::optional<FileDigest> Digest;
    std::optional<WorkspaceSourceFile> Source;
    uint64_t ReadBytes = 0;
    uint64_t ScanBytes = 0;
  };

  struct Totals {
    uint64_t Entries = 0;
    uint64_t Files = 0;
    uint64_t ReadBytes = 0;
    uint64_t ScanBytes = 0;
  };

  struct Subtree {
    llvm::StringMap<CachedFile> Files;
    llvm::StringMap<WorkspaceEntryMetadata> Directories;
    llvm::StringSet<> PrunedMetadataRoots;
  };

  Impl(Path WorkspaceRoot, WorkspaceSourceLimits Limits)
      : Root(normalizedPath(std::move(WorkspaceRoot))), Limits(Limits) {}

  static WorkspaceEntryMetadata metadata(const llvm::vfs::Status &S) {
    return {S.getUniqueID(), S.getLastModificationTime(), S.getSize()};
  }

  llvm::Error addToTotal(uint64_t &Total, uint64_t Amount, uint64_t Limit,
                         llvm::StringRef Resource) const {
    if (Amount > Limit || Total > Limit - Amount)
      return error("workspace file-rename inventory exceeds the {0} limit of "
                   "{1}",
                   Resource, Limit);
    Total += Amount;
    return llvm::Error::success();
  }

  llvm::Expected<bool> containsIncludes(PathRef File, llvm::StringRef Code) {
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

  llvm::Expected<CachedFile> readFile(PathRef File,
                                      const llvm::vfs::Status &Status,
                                      llvm::vfs::FileSystem &FS,
                                      Totals &Usage) {
    llvm::SmallString<256> Real;
    if (std::error_code EC = FS.getRealPath(File, Real))
      return error("cannot resolve real path {0}: {1}", File, EC.message());
    if (!pathStartsWith(CanonicalRoot, Real))
      return error("workspace source traverses outside the workspace: {0}",
                   File);
    if (auto Err = addToTotal(Usage.Files, 1, Limits.MaxFiles, "file-count"))
      return std::move(Err);

    CachedFile Entry;
    Entry.Meta = metadata(Status);
    namespace types = clang::driver::types;
    llvm::StringRef Extension = llvm::sys::path::extension(File);
    types::ID Type =
        Extension.empty()
            ? types::TY_INVALID
            : types::lookupTypeForExtension(Extension.drop_front());
    bool IsKnownSource =
        Type != types::TY_INVALID &&
        (types::isSrcFile(Type) || types::onlyPrecompileType(Type));
    if (Type != types::TY_INVALID && !IsKnownSource) {
      Entry.Classification = WorkspaceFileClassification::Metadata;
      return Entry;
    }

    if (auto Err = addToTotal(Usage.ReadBytes, Status.getSize(),
                              Limits.MaxBytesRead, "bytes-read"))
      return std::move(Err);
    auto Buffer = FS.getBufferForFile(File);
    if (!Buffer)
      return error("cannot read workspace file {0}: {1}", File,
                   Buffer.getError().message());
    auto Current = FS.status(File);
    if (!Current)
      return error("cannot revalidate workspace file {0}: {1}", File,
                   Current.getError().message());
    if (!Current->isRegularFile() || !(metadata(*Current) == Entry.Meta) ||
        Buffer.get()->getBufferSize() != Status.getSize())
      return error("workspace file changed while being inventoried: {0}", File);

    llvm::StringRef Code = Buffer.get()->getBuffer();
    Entry.ReadBytes = Code.size();
    if (Code.contains('\0')) {
      Entry.Classification = WorkspaceFileClassification::Binary;
      return Entry;
    }
    if (Code.size() > Limits.MaxTextFileBytes)
      return error("workspace text file {0} is {1} bytes, exceeding the {2}-"
                   "byte file-rename inventory limit",
                   File, Code.size(), Limits.MaxTextFileBytes);

    Entry.Digest = digest(Code);
    bool IsRelevant = IsKnownSource;
    if (!IsKnownSource) {
      if (auto Err =
              addToTotal(Usage.ScanBytes, Code.size(),
                         Limits.MaxDirectiveScanBytes, "directive-scan bytes"))
        return std::move(Err);
      Entry.ScanBytes = Code.size();
      auto HasIncludes = containsIncludes(File, Code);
      if (!HasIncludes)
        return HasIncludes.takeError();
      IsRelevant = *HasIncludes;
    }
    Entry.Classification = IsKnownSource
                               ? WorkspaceFileClassification::Source
                               : WorkspaceFileClassification::PossibleText;
    if (IsRelevant)
      Entry.Source =
          WorkspaceSourceFile{File.str(), /*IsHeader=*/!IsKnownSource ||
                                              types::onlyPrecompileType(Type)};
    return Entry;
  }

  Totals totalsOutside(PathRef Prefix) const {
    Totals Result;
    for (const auto &Entry : Files) {
      if (pathStartsWith(Prefix, Entry.first()))
        continue;
      ++Result.Files;
      ++Result.Entries;
      Result.ReadBytes += Entry.getValue().ReadBytes;
      Result.ScanBytes += Entry.getValue().ScanBytes;
    }
    for (PathRef Directory : Directories.keys())
      if (!pathStartsWith(Prefix, Directory))
        ++Result.Entries;
    return Result;
  }

  llvm::Expected<Subtree> buildSubtree(PathRef Directory,
                                       llvm::vfs::FileSystem &FS) {
    auto DirectoryStatus = FS.status(Directory);
    if (!DirectoryStatus)
      return error("cannot inspect workspace directory {0}: {1}", Directory,
                   DirectoryStatus.getError().message());
    if (!DirectoryStatus->isDirectory())
      return error("workspace path is not a directory: {0}", Directory);

    Subtree Result;
    Result.Directories[Directory] = metadata(*DirectoryStatus);
    if (fileRenamePrunesDirectory(Directory) && Directory != Root) {
      Result.PrunedMetadataRoots.insert(Directory);
      return Result;
    }

    Totals Usage = totalsOutside(Directory);
    if (auto Err =
            addToTotal(Usage.Entries, 1, Limits.MaxEntries, "entry-count"))
      return std::move(Err);
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
      if (auto Err =
              addToTotal(Usage.Entries, 1, Limits.MaxEntries, "entry-count"))
        return std::move(Err);
      if (Status->isDirectory()) {
        Result.Directories[It->path()] = metadata(*Status);
        if (fileRenamePrunesDirectory(It->path())) {
          Result.PrunedMetadataRoots.insert(It->path());
          It.no_push();
        }
        continue;
      }
      if (!Status->isRegularFile())
        continue;
      auto Entry = readFile(It->path(), *Status, FS, Usage);
      if (!Entry)
        return Entry.takeError();
      Result.Files[It->path()] = std::move(*Entry);
    }
    if (EC)
      return error("cannot enumerate workspace {0}: {1}", Directory,
                   EC.message());

    for (const auto &Entry : Result.Directories) {
      auto Current = FS.status(Entry.first());
      if (!Current || !Current->isDirectory() ||
          !(metadata(*Current) == Entry.getValue()))
        return error("workspace directory changed while being inventoried: {0}",
                     Entry.first());
    }
    return Result;
  }

  bool eraseSubtree(PathRef Prefix) {
    bool Changed = false;
    for (auto *Map : {&Files}) {
      std::vector<Path> Erase;
      for (PathRef Path : Map->keys())
        if (pathStartsWith(Prefix, Path))
          Erase.push_back(Path.str());
      for (PathRef Path : Erase)
        Changed |= Map->erase(Path);
    }
    std::vector<Path> DirectoriesToErase;
    for (PathRef Path : Directories.keys())
      if (pathStartsWith(Prefix, Path))
        DirectoriesToErase.push_back(Path.str());
    for (PathRef Path : DirectoriesToErase)
      Changed |= Directories.erase(Path);
    std::vector<Path> PrunedToErase;
    for (PathRef Path : PrunedMetadataRoots.keys())
      if (pathStartsWith(Prefix, Path))
        PrunedToErase.push_back(Path.str());
    for (PathRef Path : PrunedToErase)
      Changed |= PrunedMetadataRoots.erase(Path);
    return Changed;
  }

  void commitSubtree(PathRef Directory, Subtree Replacement) {
    eraseSubtree(Directory);
    for (auto &Entry : Replacement.Files)
      Files[Entry.first()] = std::move(Entry.getValue());
    for (auto &Entry : Replacement.Directories)
      Directories[Entry.first()] = Entry.getValue();
    for (PathRef Root : Replacement.PrunedMetadataRoots.keys())
      PrunedMetadataRoots.insert(Root);
  }

  llvm::Error refreshSubtree(PathRef Directory, llvm::vfs::FileSystem &FS) {
    auto Replacement = buildSubtree(Directory, FS);
    if (!Replacement)
      return Replacement.takeError();
    commitSubtree(Directory, std::move(*Replacement));
    ++Generation;
    return llvm::Error::success();
  }

  llvm::Error refreshFile(PathRef File, const llvm::vfs::Status &Status,
                          llvm::vfs::FileSystem &FS) {
    Totals Usage = totalsOutside(File);
    if (auto Err =
            addToTotal(Usage.Entries, 1, Limits.MaxEntries, "entry-count"))
      return Err;
    auto Replacement = readFile(File, Status, FS, Usage);
    if (!Replacement)
      return Replacement.takeError();
    Files[File] = std::move(*Replacement);
    ++Generation;
    return llvm::Error::success();
  }

  llvm::Error initialize(llvm::vfs::FileSystem &FS) {
    if (!llvm::sys::path::is_absolute(Root))
      return error("workspace path is not absolute: {0}", Root);
    llvm::SmallString<256> Canonical;
    if (std::error_code EC = FS.getRealPath(Root, Canonical))
      return error("cannot resolve workspace root {0}: {1}", Root,
                   EC.message());
    CanonicalRoot = Canonical.str().str();
    if (auto Err = refreshSubtree(Root, FS))
      return Err;
    // The complete scan observes every invalidation received before
    // initialization. Leaving these queued would needlessly refresh the same
    // paths on the next snapshot and make two identical snapshots appear to
    // have different generations.
    Invalidated.clear();
    Initialized = true;
    return llvm::Error::success();
  }

  llvm::Error refreshInvalidated(PathRef Path, llvm::vfs::FileSystem &FS) {
    if (isUnderVCSMetadata(Root, Path))
      return llvm::Error::success();
    auto Status = FS.status(Path);
    if (!Status) {
      if (Status.getError() != std::errc::no_such_file_or_directory)
        return error("cannot inspect invalidated workspace path {0}: {1}", Path,
                     Status.getError().message());
      if (eraseSubtree(Path))
        ++Generation;
      return llvm::Error::success();
    }
    if (Status->isDirectory())
      return refreshSubtree(Path, FS);
    if (Status->isRegularFile())
      return refreshFile(Path, *Status, FS);
    if (eraseSubtree(Path))
      ++Generation;
    return llvm::Error::success();
  }

  llvm::Error refresh(llvm::vfs::FileSystem &FS) {
    if (!Initialized)
      return initialize(FS);

    std::vector<Path> InvalidatedPaths;
    for (PathRef Path : Invalidated.keys())
      InvalidatedPaths.push_back(Path.str());
    llvm::sort(InvalidatedPaths,
               [](PathRef L, PathRef R) { return L.size() < R.size(); });
    for (PathRef Path : InvalidatedPaths) {
      if (!Invalidated.contains(Path))
        continue;
      if (auto Err = refreshInvalidated(Path, FS))
        return Err;
      Invalidated.erase(Path);
    }

    std::vector<Path> ChangedDirectories;
    for (const auto &Entry : Directories) {
      auto Status = FS.status(Entry.first());
      if (!Status || !Status->isDirectory() ||
          !(metadata(*Status) == Entry.getValue()))
        ChangedDirectories.push_back(Entry.first().str());
    }
    llvm::sort(ChangedDirectories,
               [](PathRef L, PathRef R) { return L.size() < R.size(); });
    for (PathRef Directory : ChangedDirectories) {
      if (!Directories.contains(Directory))
        continue;
      auto Status = FS.status(Directory);
      if (!Status) {
        if (Status.getError() != std::errc::no_such_file_or_directory)
          return error("cannot inspect cached workspace directory {0}: {1}",
                       Directory, Status.getError().message());
        if (eraseSubtree(Directory))
          ++Generation;
        continue;
      }
      auto Existing = Directories.find(Directory);
      if (Existing != Directories.end() && Status->isDirectory() &&
          metadata(*Status) == Existing->getValue())
        continue;
      if (auto Err = refreshSubtree(Directory, FS))
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
        if (Status.getError() != std::errc::no_such_file_or_directory)
          return error("cannot inspect cached workspace file {0}: {1}", File,
                       Status.getError().message());
        Files.erase(File);
        ++Generation;
        continue;
      }
      if (!Status->isRegularFile()) {
        Files.erase(File);
        ++Generation;
        continue;
      }
      if (!(metadata(*Status) == Existing->getValue().Meta))
        if (auto Err = refreshFile(File, *Status, FS))
          return Err;
    }
    return llvm::Error::success();
  }

  Path Root;
  Path CanonicalRoot;
  WorkspaceSourceLimits Limits;
  bool Initialized = false;
  uint64_t Generation = 0;
  llvm::StringMap<CachedFile> Files;
  llvm::StringMap<WorkspaceEntryMetadata> Directories;
  llvm::StringSet<> PrunedMetadataRoots;
  llvm::StringSet<> Invalidated;
  std::mutex Mu;
};

WorkspaceSourceCache::WorkspaceSourceCache(Path WorkspaceRoot,
                                           WorkspaceSourceLimits Limits)
    : State(std::make_unique<Impl>(std::move(WorkspaceRoot),
                                   std::move(Limits))) {}

WorkspaceSourceCache::~WorkspaceSourceCache() = default;

llvm::Expected<WorkspaceSourceSnapshot>
WorkspaceSourceCache::snapshot(llvm::vfs::FileSystem &FS) {
  std::lock_guard<std::mutex> Lock(State->Mu);
  if (auto Err = State->refresh(FS))
    return std::move(Err);
  WorkspaceSourceSnapshot Result;
  Result.Generation = State->Generation;
  Result.Directories = State->Directories;
  Result.PrunedMetadataRoots = State->PrunedMetadataRoots;
  for (const auto &Entry : State->Files) {
    WorkspaceFileSnapshot Public;
    Public.Metadata = Entry.getValue().Meta;
    Public.Classification = Entry.getValue().Classification;
    Public.Digest = Entry.getValue().Digest;
    Public.Source = Entry.getValue().Source;
    Result.Files[Entry.first()] = std::move(Public);
    if (Entry.getValue().Digest)
      Result.Digests[Entry.first()] = *Entry.getValue().Digest;
    if (Entry.getValue().Source)
      Result.Sources.push_back(*Entry.getValue().Source);
  }
  llvm::sort(Result.Sources,
             [](const auto &L, const auto &R) { return L.File < R.File; });
  return Result;
}

void WorkspaceSourceCache::invalidate(PathRef InputPath) {
  std::lock_guard<std::mutex> Lock(State->Mu);
  Path Normalized = normalizedPath(InputPath.str());
  if (!llvm::sys::path::is_absolute(State->Root) ||
      !llvm::sys::path::is_absolute(Normalized) ||
      (!pathEqual(State->Root, Normalized) &&
       !pathStartsWith(State->Root, Normalized)))
    return;
  if (State->Invalidated.insert(Normalized).second)
    ++State->Generation;
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
