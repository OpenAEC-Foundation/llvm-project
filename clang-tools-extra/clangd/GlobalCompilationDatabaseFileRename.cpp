//===--- GlobalCompilationDatabaseFileRename.cpp -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CompilerInvocation.h"
#include "FS.h"
#include "FileRename.h"
#include "GlobalCompilationDatabase.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

bool commandMatchesFileRenameSnapshot(const tooling::CompileCommand &Current,
                                      const tooling::CompileCommand &Expected) {
  return Current == Expected;
}

bool renameSetIsNoop(llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  return Renames.empty() || llvm::all_of(Renames, [](const auto &Rename) {
           return pathEqual(removeDots(Rename.first),
                            removeDots(Rename.second));
         });
}

bool pathWithinRenameNamespace(PathRef RawPath,
                               llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  Path Normalized = removeDots(RawPath);
  if (!llvm::sys::path::is_absolute(Normalized))
    return true;
  return llvm::any_of(Renames, [&](const auto &Rename) {
    Path Old = removeDots(Rename.first);
    Path New = removeDots(Rename.second);
    if (!llvm::sys::path::is_absolute(Old) ||
        !llvm::sys::path::is_absolute(New))
      return true;
    return pathStartsWith(Old, Normalized) || pathStartsWith(New, Normalized);
  });
}

Path absoluteCommandPath(PathRef RawPath, PathRef BaseDirectory) {
  llvm::SmallString<256> Result(RawPath);
  if (!llvm::sys::path::is_absolute(Result)) {
    Result = BaseDirectory;
    llvm::sys::path::append(Result, RawPath);
  }
  llvm::sys::path::remove_dots(Result, /*remove_dot_dot=*/true);
  return Result.str().str();
}

bool unparsedArgumentsMayIntersectRenames(
    const tooling::CompileCommand &Command, PathRef EffectiveDirectory,
    llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  auto CandidateIntersects = [&](llvm::StringRef Raw) {
    Raw = Raw.trim(" \t\"'");
    if (Raw.consume_front("@"))
      Raw = Raw.trim(" \t\"'");
    if (Raw.empty())
      return false;
    if (llvm::sys::path::is_absolute(Raw) ||
        llvm::sys::path::has_parent_path(Raw))
      return pathWithinRenameNamespace(
          absoluteCommandPath(Raw, EffectiveDirectory), Renames);
    return llvm::any_of(Renames, [&](const auto &Rename) {
      return pathEqual(Raw, llvm::sys::path::filename(Rename.first)) ||
             pathEqual(Raw, llvm::sys::path::filename(Rename.second));
    });
  };

  for (llvm::StringRef Argument : Command.CommandLine) {
    llvm::SmallVector<llvm::StringRef> Pieces;
    Argument.split(Pieces, ",;=", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    for (llvm::StringRef Piece : Pieces) {
      if (CandidateIntersects(Piece))
        return true;
      // Joined options such as -I/path keep the option spelling and path in
      // one piece. An absolute suffix is still unambiguous.
      size_t Root = Piece.find(llvm::sys::path::get_separator());
      if (Root != llvm::StringRef::npos &&
          CandidateIntersects(Piece.drop_front(Root)))
        return true;
    }
  }
  return false;
}

} // namespace

llvm::Error GlobalCompilationDatabase::prepareFileRenames(
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    llvm::ArrayRef<FileRenameCompileCommand> ValidatedCommands) const {
  for (const FileRenameCompileCommand &Expected : ValidatedCommands) {
    auto Current = getCompileCommand(Expected.File);
    if (!Current ||
        !commandMatchesFileRenameSnapshot(*Current, Expected.Command))
      return error("compilation command changed while preparing file rename: "
                   "{0}",
                   Expected.File);
    if (auto Err = validateCompileCommandForRenames(*Current, Renames, {},
                                                    nullptr, {}))
      return joinErrors(
          error("cannot prepare compilation command {0} for file rename",
                Expected.File),
          std::move(Err));
  }
  return llvm::Error::success();
}

struct OverlayCDB::PreparedRename {
  std::vector<std::pair<Path, Path>> Renames;
  uint64_t Generation = 0;
  llvm::StringMap<tooling::CompileCommand> Commands;
  std::vector<std::string> ChangedFiles;
};

OverlayCDB::OverlayCDB(const GlobalCompilationDatabase *Base,
                       std::vector<std::string> FallbackFlags,
                       CommandMangler Mangler,
                       std::optional<std::string> FallbackWorkingDirectory)
    : DelegatingCDB(Base, FallbackWorkingDirectory),
      Mangler(std::move(Mangler)), FallbackFlags(std::move(FallbackFlags)) {}

OverlayCDB::~OverlayCDB() = default;

void OverlayCDB::expandResponseFileProvenance(
    tooling::CompileCommand &Command) {
  // Overlay commands arrive over LSP and have not passed through the file-CDB
  // response-file expansion wrapper.
  auto FS = llvm::vfs::getRealFileSystem();
  auto Tokenizer = llvm::Triple(llvm::sys::getProcessTriple()).isOSWindows()
                       ? llvm::cl::TokenizeWindowsCommandLine
                       : llvm::cl::TokenizeGNUCommandLine;
  Command.HadResponseFile |= tooling::addExpandedResponseFiles(
      Command.CommandLine, Command.Directory, Tokenizer, *FS);
}

bool OverlayCDB::setCompileCommand(PathRef File,
                                   std::optional<tooling::CompileCommand> Cmd) {
  std::string CanonPath = removeDots(File);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Cmd) {
      auto It = Commands.find(CanonPath);
      if (It == Commands.end())
        Commands.try_emplace(CanonPath, std::move(*Cmd));
      else {
        if (It->second == *Cmd)
          return false;
        It->second = std::move(*Cmd);
      }
    } else if (!Commands.erase(CanonPath)) {
      return false;
    }
    ++CommandGeneration;
    Prepared.reset();
  }
  OnCommandChanged.broadcast({CanonPath});
  return true;
}

llvm::Expected<std::unique_ptr<OverlayCDB::PreparedRename>>
OverlayCDB::buildRenamePlanLocked(
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    const llvm::StringMap<tooling::CompileCommand> *ValidatedCommands) const {
  auto Plan = std::make_unique<PreparedRename>();
  Plan->Renames.assign(Renames.begin(), Renames.end());
  Plan->Generation = CommandGeneration;
  llvm::StringMap<Path> PlannedOrigins;
  llvm::StringSet<> CollidingDestinations;

  auto Invalidate = [&](PathRef OldKey, PathRef NewKey) {
    Plan->ChangedFiles.push_back(OldKey.str());
    if (!pathEqual(OldKey, NewKey))
      Plan->ChangedFiles.push_back(NewKey.str());
  };

  auto AddCommand = [&](PathRef OldKey, PathRef NewKey,
                        tooling::CompileCommand Command, bool CommandChanged) {
    if (CollidingDestinations.contains(NewKey)) {
      Invalidate(OldKey, NewKey);
      return;
    }
    if (!Plan->Commands.try_emplace(NewKey, std::move(Command)).second) {
      Invalidate(OldKey, NewKey);
      Invalidate(PlannedOrigins.lookup(NewKey), NewKey);
      Plan->Commands.erase(NewKey);
      PlannedOrigins.erase(NewKey);
      CollidingDestinations.insert(NewKey);
      return;
    }
    PlannedOrigins[NewKey] = OldKey.str();
    if (!pathEqual(OldKey, NewKey) || CommandChanged) {
      Plan->ChangedFiles.push_back(OldKey.str());
      if (!pathEqual(OldKey, NewKey))
        Plan->ChangedFiles.push_back(NewKey.str());
    }
  };

  for (const auto &Entry : Commands) {
    auto NewKey = mapPathAfterRenames(Entry.first(), Renames);
    if (!NewKey)
      return NewKey.takeError();
    tooling::CompileCommand Command = Entry.getValue();
    const tooling::CompileCommand OriginalCommand = Command;
    tooling::CompileCommand RawCommand = Command;
    expandResponseFileProvenance(RawCommand);
    RawCommand.HadConfigFile |= compilerLoadsConfigFile(RawCommand);
    if (ValidatedCommands) {
      auto Expected = ValidatedCommands->find(Entry.first());
      if (Expected != ValidatedCommands->end()) {
        auto EffectiveCommand = Command;
        expandResponseFileProvenance(EffectiveCommand);
        if (Mangler)
          Mangler(EffectiveCommand, Entry.first());
        EffectiveCommand.HadConfigFile |=
            compilerLoadsConfigFile(EffectiveCommand);
        if (!commandMatchesFileRenameSnapshot(EffectiveCommand,
                                              Expected->getValue()))
          return error(
              "compilation command changed while preparing file rename: {0}",
              Entry.first());
      }
    }
    auto Normalized = normalizeCompilerCommand(Command);
    bool StructurallyAffected =
        !pathEqual(*NewKey, Entry.first()) ||
        pathWithinRenameNamespace(Command.Directory, Renames);
    Path EffectiveDirectory = Command.Directory;
    if (Normalized) {
      EffectiveDirectory = Normalized->EffectiveDirectory;
      StructurallyAffected |=
          pathWithinRenameNamespace(Normalized->EffectiveDirectory, Renames);
      StructurallyAffected |= llvm::any_of(
          Normalized->Inputs, [&](const CompilerInputArgument &Input) {
            return pathWithinRenameNamespace(Input.AbsolutePath, Renames);
          });
    }
    if (!Command.Filename.empty())
      StructurallyAffected |= pathWithinRenameNamespace(
          absoluteCommandPath(Command.Filename, Command.Directory), Renames);

    std::string OriginalProofFailure;
    if (auto Err =
            validateCompileCommandForRenames(RawCommand, {}, {}, nullptr, {}))
      OriginalProofFailure = llvm::toString(std::move(Err));
    if (OriginalProofFailure.empty() && !StructurallyAffected) {
      if (auto Err = validateCompileCommandForRenames(RawCommand, Renames, {},
                                                      nullptr, {})) {
        llvm::consumeError(std::move(Err));
        StructurallyAffected = true;
      }
    }
    if (!OriginalProofFailure.empty() && !StructurallyAffected)
      StructurallyAffected = unparsedArgumentsMayIntersectRenames(
          Command, EffectiveDirectory, Renames);
    if (!StructurallyAffected) {
      AddCommand(Entry.first(), Entry.first(), std::move(Command),
                 /*CommandChanged=*/false);
      if (!Normalized)
        llvm::consumeError(Normalized.takeError());
      continue;
    }
    if (!OriginalProofFailure.empty()) {
      vlog("Invalidating affected unprovable overlay command {0} during file "
           "rename: {1}",
           Entry.first(), OriginalProofFailure);
      Invalidate(Entry.first(), *NewKey);
      if (!Normalized)
        llvm::consumeError(Normalized.takeError());
      continue;
    }
    if (!Normalized) {
      vlog("Invalidating affected unclassifiable overlay command {0} during "
           "file rename: {1}",
           Entry.first(), llvm::toString(Normalized.takeError()));
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    auto Projected = projectCompileCommandAfterRenames(Command, Renames);
    if (!Projected)
      return Projected.takeError();
    Command = std::move(*Projected);
    tooling::CompileCommand PlannedRaw = Command;
    expandResponseFileProvenance(PlannedRaw);
    PlannedRaw.HadConfigFile |= compilerLoadsConfigFile(PlannedRaw);
    if (auto Err = validateCompileCommandForRenames(PlannedRaw, Renames, {},
                                                    nullptr, {})) {
      vlog("Invalidating unprovable renamed overlay command {0}: {1}",
           Entry.first(), llvm::toString(std::move(Err)));
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    tooling::CompileCommand PlannedEffective = Command;
    expandResponseFileProvenance(PlannedEffective);
    if (Mangler)
      Mangler(PlannedEffective, *NewKey);
    PlannedEffective.HadConfigFile |= compilerLoadsConfigFile(PlannedEffective);
    if (auto Err = validateCompileCommandForRenames(PlannedEffective, Renames,
                                                    {}, nullptr, {})) {
      vlog("Invalidating unprovable effective overlay command {0}: {1}",
           Entry.first(), llvm::toString(std::move(Err)));
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    const bool CommandChanged = Command != OriginalCommand;
    AddCommand(Entry.first(), *NewKey, std::move(Command), CommandChanged);
  }
  return Plan;
}

llvm::Error OverlayCDB::prepareFileRenames(
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    llvm::ArrayRef<FileRenameCompileCommand> ValidatedCommands) const {
  discardPreparedFileRenames();
  if (renameSetIsNoop(Renames))
    return llvm::Error::success();
  llvm::StringMap<tooling::CompileCommand> Validated;
  for (const FileRenameCompileCommand &Command : ValidatedCommands)
    Validated[removeDots(Command.File)] = Command.Command;

  std::vector<FileRenameCompileCommand> InheritedExpectations;
  std::unique_ptr<PreparedRename> Plan;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    for (const FileRenameCompileCommand &Command : ValidatedCommands)
      if (!Commands.contains(removeDots(Command.File)))
        InheritedExpectations.push_back(Command);
    auto Built = buildRenamePlanLocked(Renames, &Validated);
    if (!Built)
      return Built.takeError();
    Plan = std::move(*Built);
  }

  std::vector<FileRenameCompileCommand> BaseCommands;
  for (const FileRenameCompileCommand &Expected : InheritedExpectations) {
    auto Inherited = DelegatingCDB::getCompileCommand(Expected.File);
    if (!Inherited)
      return error("compilation command changed while preparing file rename: "
                   "{0}",
                   Expected.File);
    tooling::CompileCommand Effective = *Inherited;
    if (Mangler)
      Mangler(Effective, Expected.File);
    Effective.HadConfigFile |= compilerLoadsConfigFile(Effective);
    if (!commandMatchesFileRenameSnapshot(Effective, Expected.Command))
      return error("compilation command changed while preparing file rename: "
                   "{0}",
                   Expected.File);
    BaseCommands.push_back({Expected.File, std::move(*Inherited)});
  }
  if (auto Err = DelegatingCDB::prepareFileRenames(Renames, BaseCommands))
    return Err;

  bool CommandsChanged = false;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    CommandsChanged = Plan->Generation != CommandGeneration;
    if (!CommandsChanged)
      Prepared = std::move(Plan);
  }
  if (CommandsChanged) {
    DelegatingCDB::discardPreparedFileRenames();
    return error("compilation commands changed while preparing file rename");
  }
  return llvm::Error::success();
}

void OverlayCDB::discardPreparedFileRenames() const {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    Prepared.reset();
  }
  DelegatingCDB::discardPreparedFileRenames();
}

std::optional<tooling::CompileCommand>
OverlayCDB::getCompileCommandAfterPreparedFileRenames(PathRef File) const {
  std::optional<tooling::CompileCommand> Command;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Prepared) {
      auto It = Prepared->Commands.find(removeDots(File));
      if (It != Prepared->Commands.end())
        Command = It->second;
    }
  }
  if (Command)
    expandResponseFileProvenance(*Command);
  if (!Command)
    Command = DelegatingCDB::getCompileCommandAfterPreparedFileRenames(File);
  if (Command && Mangler)
    Mangler(*Command, File);
  return Command;
}

llvm::Error
OverlayCDB::filesRenamed(llvm::ArrayRef<std::pair<Path, Path>> Renames) const {
  if (renameSetIsNoop(Renames))
    return llvm::Error::success();

  std::vector<std::string> Changed;
  std::unique_ptr<PreparedRename> Plan;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Prepared && Prepared->Generation == CommandGeneration &&
        llvm::ArrayRef(Prepared->Renames) == Renames)
      Plan = std::move(Prepared);
    else {
      Prepared.reset();
      auto Built = buildRenamePlanLocked(Renames, nullptr);
      if (!Built)
        return Built.takeError();
      Plan = std::move(*Built);
    }
  }
  if (auto Err = DelegatingCDB::filesRenamed(Renames))
    return Err;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Plan->Generation != CommandGeneration) {
      auto Rebuilt = buildRenamePlanLocked(Renames, nullptr);
      if (!Rebuilt)
        return Rebuilt.takeError();
      Plan = std::move(*Rebuilt);
    }
    Changed = std::move(Plan->ChangedFiles);
    Commands = std::move(Plan->Commands);
    ++CommandGeneration;
    Prepared.reset();
  }
  llvm::sort(Changed);
  Changed.erase(std::unique(Changed.begin(), Changed.end()), Changed.end());
  if (!Changed.empty())
    OnCommandChanged.broadcast(Changed);
  return llvm::Error::success();
}

llvm::Error DelegatingCDB::filesRenamed(
    llvm::ArrayRef<std::pair<Path, Path>> Renames) const {
  if (!Base)
    return llvm::Error::success();
  return Base->filesRenamed(Renames);
}

llvm::Error DelegatingCDB::prepareFileRenames(
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    llvm::ArrayRef<FileRenameCompileCommand> ValidatedCommands) const {
  if (!Base)
    return llvm::Error::success();
  return Base->prepareFileRenames(Renames, ValidatedCommands);
}

void DelegatingCDB::discardPreparedFileRenames() const {
  if (Base)
    Base->discardPreparedFileRenames();
}

std::optional<tooling::CompileCommand>
DelegatingCDB::getCompileCommandAfterPreparedFileRenames(PathRef File) const {
  if (!Base)
    return std::nullopt;
  return Base->getCompileCommandAfterPreparedFileRenames(File);
}

} // namespace clangd
} // namespace clang
