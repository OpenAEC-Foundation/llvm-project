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
  return Current.Directory == Expected.Directory &&
         Current.CommandLine == Expected.CommandLine &&
         Current.HadResponseFile == Expected.HadResponseFile &&
         Current.HadConfigFile == Expected.HadConfigFile;
}

bool renameSetIsNoop(llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  return Renames.empty() || llvm::all_of(Renames, [](const auto &Rename) {
           return removeDots(Rename.first) == removeDots(Rename.second);
         });
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
    if (OldKey != NewKey)
      Plan->ChangedFiles.push_back(NewKey.str());
  };

  auto Rewrite = [&](tooling::CompileCommand &Command,
                     const CompilerInputArgument &Span, PathRef NewBase,
                     PathRef Mapped) {
    std::string &Argument = Command.CommandLine[Span.ArgumentIndex];
    llvm::StringRef OldValue(Argument.data() + Span.ValueOffset,
                             Span.ValueLength);
    std::string Replacement = Mapped.str();
    if (!llvm::sys::path::is_absolute(OldValue)) {
      llvm::SmallString<256> Preserved(NewBase);
      llvm::sys::path::append(Preserved, OldValue);
      llvm::sys::path::remove_dots(Preserved, /*remove_dot_dot=*/true);
      if (pathEqual(Preserved, Mapped))
        Replacement = OldValue.str();
      else if (pathEqual(llvm::sys::path::parent_path(Mapped), NewBase))
        Replacement = llvm::sys::path::filename(Mapped).str();
    }
    Argument.replace(Span.ValueOffset, Span.ValueLength, Replacement);
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
    // First prove that the original command is classifiable in isolation.
    // Paths that move with the command (notably its working directory and
    // source operands) are rewritten below, so validating them against the
    // rename before rewriting would reject migrations we can preserve.
    if (auto Err = validateCompileCommandForRenames(RawCommand, {}, {},
                                                    nullptr, {})) {
      vlog("Invalidating unprovable overlay command {0} during file rename: "
           "{1}",
           Entry.first(), llvm::toString(std::move(Err)));
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    auto Normalized = normalizeCompilerCommand(Command);
    if (!Normalized) {
      vlog("Invalidating unclassifiable overlay command {0} during file "
           "rename: {1}",
           Entry.first(), llvm::toString(Normalized.takeError()));
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    auto NewDirectory = mapPathAfterRenames(Command.Directory, Renames);
    if (!NewDirectory)
      return NewDirectory.takeError();
    auto NewEffective =
        mapPathAfterRenames(Normalized->EffectiveDirectory, Renames);
    if (!NewEffective)
      return NewEffective.takeError();
    for (const CompilerInputArgument &Input : Normalized->Inputs) {
      auto Mapped = mapPathAfterRenames(Input.AbsolutePath, Renames);
      if (!Mapped)
        return Mapped.takeError();
      Rewrite(Command, Input, *NewEffective, *Mapped);
    }
    if (Normalized->WorkingDirectory) {
      auto Mapped =
          mapPathAfterRenames(Normalized->WorkingDirectory->AbsolutePath,
                              Renames);
      if (!Mapped)
        return Mapped.takeError();
      Rewrite(Command, *Normalized->WorkingDirectory, *NewDirectory, *Mapped);
    }
    if (!Command.Filename.empty()) {
      llvm::SmallString<256> Filename(Command.Filename);
      if (!llvm::sys::path::is_absolute(Filename)) {
        Filename = Command.Directory;
        llvm::sys::path::append(Filename, Command.Filename);
      }
      llvm::sys::path::remove_dots(Filename, /*remove_dot_dot=*/true);
      auto Mapped = mapPathAfterRenames(Filename, Renames);
      if (!Mapped)
        return Mapped.takeError();
      if (!pathEqual(Filename, *Mapped))
        Command.Filename = *Mapped;
    }
    Command.Directory = *NewDirectory;
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
    if (CollidingDestinations.contains(*NewKey)) {
      Invalidate(Entry.first(), *NewKey);
      continue;
    }
    if (!Plan->Commands.try_emplace(*NewKey, std::move(Command)).second) {
      Invalidate(Entry.first(), *NewKey);
      Invalidate(PlannedOrigins.lookup(*NewKey), *NewKey);
      Plan->Commands.erase(*NewKey);
      PlannedOrigins.erase(*NewKey);
      CollidingDestinations.insert(*NewKey);
      continue;
    }
    PlannedOrigins[*NewKey] = Entry.first().str();
    if (Entry.first() != *NewKey || CommandChanged) {
      Plan->ChangedFiles.push_back(Entry.first().str());
      if (Entry.first() != *NewKey)
        Plan->ChangedFiles.push_back(*NewKey);
    }
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
