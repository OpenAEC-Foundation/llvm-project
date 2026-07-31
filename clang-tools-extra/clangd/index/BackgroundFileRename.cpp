//===-- BackgroundFileRename.cpp - Migrate background index state --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FS.h"
#include "URI.h"
#include "index/Background.h"
#include "index/BackgroundIndexLoader.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {

llvm::Error
BackgroundIndex::filesRenamed(llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  if (Renames.empty() || llvm::all_of(Renames, [](const auto &Rename) {
        return removeDots(Rename.first) == removeDots(Rename.second);
      }))
    return llvm::Error::success();
  std::lock_guard<std::mutex> RenameLock(RenameMu);
  std::vector<Path> IncompleteTUs;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    for (PathRef TU : KnownTUs.keys())
      if (!FreshlyIndexedTUs.contains(TU))
        IncompleteTUs.push_back(TU.str());
  }

  // A didRename notification may arrive before queued cache loading starts.
  // Read the persisted per-TU context directly so dependency shards moved by
  // a directory rename are still invalidated and the owning TU is rebuilt.
  llvm::StringSet<> CachedAffectedTUs;
  llvm::StringSet<> CachedRemovedPaths;
  llvm::DenseSet<BackgroundIndexStorage *> StorageScheduledForClear;
  std::vector<BackgroundIndexStorage *> StoragesToClear;
  for (PathRef TU : IncompleteTUs) {
    // A destination can change header lookup even when no recorded edge names
    // a moved path. Every TU must therefore be rebuilt under the post-rename
    // namespace; persisted context is retained only as an input to migration.
    CachedAffectedTUs.insert(TU);
    auto NewTU = mapPathAfterRenames(TU, Renames);
    if (!NewTU)
      return NewTU.takeError();
    if (TU != *NewTU) {
      CachedAffectedTUs.insert(TU);
      CachedRemovedPaths.insert(TU);
    }
    BackgroundIndexStorage *Storage = IndexStorageFactory(TU);
    auto MainShard = Storage->loadShard(TU);
    if (!MainShard) {
      CachedAffectedTUs.insert(TU);
      if (StorageScheduledForClear.insert(Storage).second)
        StoragesToClear.push_back(Storage);
      continue;
    }
    if (!MainShard->ContextSources || !MainShard->CC1CommandLine) {
      CachedAffectedTUs.insert(TU);
      Path LegacyTU = TU.str();
      for (const LoadedShard &Legacy :
           loadIndexShards({LegacyTU}, IndexStorageFactory, CDB)) {
        auto NewPath = mapPathAfterRenames(Legacy.AbsolutePath, Renames);
        if (!NewPath)
          return NewPath.takeError();
        if (Legacy.AbsolutePath != *NewPath)
          CachedRemovedPaths.insert(Legacy.AbsolutePath);
      }
      continue;
    }
    for (const auto &Source : *MainShard->ContextSources) {
      auto SourcePath = URI::resolve(Source.first(), TU);
      if (!SourcePath)
        return SourcePath.takeError();
      auto NewSource = mapPathAfterRenames(*SourcePath, Renames);
      if (!NewSource)
        return NewSource.takeError();
      if (*SourcePath != *NewSource) {
        CachedAffectedTUs.insert(TU);
        CachedRemovedPaths.insert(*SourcePath);
      }
      for (llvm::StringRef IncludedURI : Source.getValue().DirectIncludes) {
        auto Included = URI::resolve(IncludedURI, TU);
        if (!Included)
          return Included.takeError();
        auto NewInclude = mapPathAfterRenames(*Included, Renames);
        if (!NewInclude)
          return NewInclude.takeError();
        if (*Included != *NewInclude)
          CachedAffectedTUs.insert(TU);
      }
    }
  }

  std::vector<IndexedFile> NewFiles;
  llvm::StringMap<ShardVersion> NewVersions;
  llvm::StringSet<> NewKnownTUs;
  llvm::StringMap<Path> NewKnownTUOrigins;
  llvm::StringSet<> NewFreshTUs;
  llvm::StringMap<std::string> NewFailures;
  std::vector<IncludeGraphError> NewGraphErrors;
  llvm::StringMap<tooling::CompileCommand> NewCommands;
  llvm::StringMap<std::vector<std::string>> NewCC1Commands;
  llvm::StringSet<> InvalidatedFiles;
  llvm::StringSet<> RemovedPaths;
  llvm::StringSet<> AffectedTUs;
  for (PathRef TU : CachedAffectedTUs.keys())
    AffectedTUs.insert(TU);
  for (PathRef File : CachedRemovedPaths.keys())
    RemovedPaths.insert(File);
  std::vector<std::string> TranslationUnits;
  llvm::StringMap<uint64_t> NewPendingIncludeGraphTUs;
  uint64_t ScheduledRenameEpoch;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    ScheduledRenameEpoch = RenameEpoch + 1;
    for (PathRef TU : KnownTUs.keys())
      AffectedTUs.insert(TU);
    for (const IndexedFile &File : IndexedFiles) {
      auto NewFile = mapPathAfterRenames(File.File, Renames);
      if (!NewFile)
        return NewFile.takeError();
      bool Affected = File.File != *NewFile;
      for (PathRef Included : File.DirectIncludes) {
        auto NewInclude = mapPathAfterRenames(Included, Renames);
        if (!NewInclude)
          return NewInclude.takeError();
        Affected |= Included != *NewInclude;
      }
      if (Affected)
        AffectedTUs.insert(File.DependentTU);
    }

    llvm::StringSet<> ContextKeys;
    NewFiles.reserve(IndexedFiles.size());
    for (IndexedFile File : IndexedFiles) {
      bool Invalidated = false;
      auto NewFile = mapPathAfterRenames(File.File, Renames);
      if (!NewFile)
        return NewFile.takeError();
      Invalidated |= File.File != *NewFile;
      Path OldFile = File.File;
      File.File = std::move(*NewFile);
      auto NewTU = mapPathAfterRenames(File.DependentTU, Renames);
      if (!NewTU)
        return NewTU.takeError();
      File.DependentTU = std::move(*NewTU);
      for (Path &Included : File.DirectIncludes) {
        auto NewInclude = mapPathAfterRenames(Included, Renames);
        if (!NewInclude)
          return NewInclude.takeError();
        Invalidated |= Included != *NewInclude;
        Included = std::move(*NewInclude);
      }
      if (Invalidated) {
        RemovedPaths.insert(OldFile);
        InvalidatedFiles.insert(File.File);
      }
      std::string ContextKey = File.DependentTU + "\n" + File.File;
      if (!ContextKeys.insert(ContextKey).second)
        return error("file rename collides at indexed context {0} from {1}",
                     File.File, File.DependentTU);
      NewFiles.push_back(std::move(File));
    }
    for (const auto &Entry : ShardVersions) {
      auto NewFile = mapPathAfterRenames(Entry.first(), Renames);
      if (!NewFile)
        return NewFile.takeError();
      if (InvalidatedFiles.contains(*NewFile))
        continue;
      if (!NewVersions.try_emplace(*NewFile, Entry.getValue()).second)
        return error("file rename collides at shard path {0}", *NewFile);
    }
    for (llvm::StringRef TU : KnownTUs.keys()) {
      Path OriginalTU = removeDots(TU);
      auto NewTU = mapPathAfterRenames(OriginalTU, Renames);
      if (!NewTU)
        return NewTU.takeError();
      if (OriginalTU != *NewTU)
        AffectedTUs.insert(TU);
      // Compilation-command migration broadcasts both the old and new names
      // before this state migration runs. The new name is therefore commonly
      // already present as an incomplete KnownTU with no graph state.
      auto Origin = NewKnownTUOrigins.try_emplace(*NewTU, OriginalTU);
      if (!Origin.second && Origin.first->getValue() != *NewTU &&
          OriginalTU != *NewTU)
        return error("file rename collides at translation unit {0}", *NewTU);
      NewKnownTUs.insert(*NewTU);
    }
    for (llvm::StringRef TU : FreshlyIndexedTUs.keys()) {
      auto NewTU = mapPathAfterRenames(TU, Renames);
      if (!NewTU)
        return NewTU.takeError();
      if (!AffectedTUs.contains(TU))
        NewFreshTUs.insert(*NewTU);
    }
    for (const auto &Entry : IndexFailures) {
      auto NewTU = mapPathAfterRenames(Entry.first(), Renames);
      if (!NewTU)
        return NewTU.takeError();
      if (!NewFailures.try_emplace(*NewTU, Entry.getValue()).second)
        return error("file rename collides at failed translation unit {0}",
                     *NewTU);
    }
    for (IncludeGraphError Entry : IncludeGraphErrors) {
      auto NewPath = mapPathAfterRenames(Entry.File, Renames);
      if (!NewPath)
        return NewPath.takeError();
      auto NewTU = mapPathAfterRenames(Entry.DependentTU, Renames);
      if (!NewTU)
        return NewTU.takeError();
      Entry.File = std::move(*NewPath);
      Entry.DependentTU = std::move(*NewTU);
      NewGraphErrors.push_back(std::move(Entry));
    }
    for (const auto &Entry : IndexedCommands) {
      auto NewTU = mapPathAfterRenames(Entry.first(), Renames);
      if (!NewTU)
        return NewTU.takeError();
      if (!AffectedTUs.contains(Entry.first()))
        NewCommands.try_emplace(*NewTU, Entry.getValue());
    }
    for (const auto &Entry : IndexedCC1Commands) {
      auto NewTU = mapPathAfterRenames(Entry.first(), Renames);
      if (!NewTU)
        return NewTU.takeError();
      if (!AffectedTUs.contains(Entry.first()))
        NewCC1Commands.try_emplace(*NewTU, Entry.getValue());
    }
    // The new epoch invalidates every queued task from the old namespace.
    // Reschedule every TU that will still be incomplete after the migration,
    // including unrelated work that happened to be queued during the rename.
    for (llvm::StringRef TU : NewKnownTUs.keys()) {
      if (NewFreshTUs.contains(TU))
        continue;
      NewPendingIncludeGraphTUs.try_emplace(TU, ScheduledRenameEpoch);
      TranslationUnits.push_back(TU.str());
    }

    // Everything below is the no-fail commit point.
    RenameEpoch = ScheduledRenameEpoch;
    ++GraphGeneration;
    IndexedFiles = std::move(NewFiles);
    IndexedCommands = std::move(NewCommands);
    IndexedCC1Commands = std::move(NewCC1Commands);
    ShardVersions = std::move(NewVersions);
    KnownTUs = std::move(NewKnownTUs);
    FreshlyIndexedTUs = std::move(NewFreshTUs);
    IndexFailures = std::move(NewFailures);
    IncludeGraphErrors = std::move(NewGraphErrors);
    PendingIncludeGraphTUs = std::move(NewPendingIncludeGraphTUs);
  }

  llvm::Error RemoveErrors = llvm::Error::success();
  // Persisted shards are reconstructable. Keep storage I/O outside the state
  // lock and report all failures after the in-memory migration is committed.
  for (BackgroundIndexStorage *Storage : StoragesToClear)
    RemoveErrors = llvm::joinErrors(std::move(RemoveErrors), Storage->clear());
  for (PathRef OldPath : RemovedPaths.keys()) {
    RemoveErrors =
        llvm::joinErrors(std::move(RemoveErrors),
                         IndexStorageFactory(OldPath)->removeShard(OldPath));
    IndexedSymbols.update(URI::create(OldPath).toString(),
                          /*Symbols=*/nullptr, /*Refs=*/nullptr,
                          /*Relations=*/nullptr, /*CountReferences=*/false);
    Rebuilder.indexedTU();
  }
  std::vector<BackgroundQueue::Task> Tasks;
  Tasks.reserve(TranslationUnits.size());
  for (std::string &TU : TranslationUnits)
    Tasks.push_back(indexFileTask(std::move(TU),
                                  /*BypassDuplicateSuppression=*/true,
                                  ScheduledRenameEpoch));
  Queue.append(std::move(Tasks));
  return RemoveErrors;
}

} // namespace clangd
} // namespace clang
