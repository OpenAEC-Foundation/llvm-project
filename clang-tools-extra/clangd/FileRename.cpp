//===--- FileRename.cpp - Include edits for file renames ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FileRename.h"
#include "Config.h"
#include "FileRenameInternal.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include <optional>
#include <system_error>

namespace clang {
namespace clangd {

bool fileRenamePathInside(PathRef Ancestor, PathRef Path) {
  if (Ancestor == Path)
    return true;
  if (!Path.starts_with(Ancestor))
    return false;
  return Path.size() > Ancestor.size() &&
         llvm::sys::path::is_separator(Path[Ancestor.size()]);
}

llvm::Expected<Path> fileRenameCanonicalPath(PathRef Path,
                                             llvm::vfs::FileSystem &FS) {
  llvm::SmallString<256> Existing(Path);
  llvm::SmallVector<llvm::StringRef> MissingComponents;
  while (true) {
    if (auto S = FS.status(Existing)) {
      llvm::SmallString<256> Real;
      if (std::error_code EC = FS.getRealPath(Existing, Real))
        return error("cannot resolve real path {0}: {1}", Existing,
                     EC.message());
      for (llvm::StringRef Component : llvm::reverse(MissingComponents))
        llvm::sys::path::append(Real, Component);
      llvm::sys::path::remove_dots(Real, /*remove_dot_dot=*/true);
      return Real.str().str();
    } else if (S.getError() != std::errc::no_such_file_or_directory) {
      return error("cannot inspect path {0}: {1}", Existing,
                   S.getError().message());
    }
    llvm::StringRef Filename = llvm::sys::path::filename(Existing);
    if (Filename.empty())
      return error("cannot find an existing ancestor of {0}", Path);
    MissingComponents.push_back(Filename);
    llvm::sys::path::remove_filename(Existing);
  }
}

namespace {

llvm::Expected<Path> normalizeAbsolute(PathRef Path) {
  if (!llvm::sys::path::is_absolute(Path))
    return error("file rename path is not absolute: {0}", Path);
  llvm::SmallString<256> Normalized(Path);
  llvm::sys::path::remove_dots(Normalized, /*remove_dot_dot=*/true);
  llvm::sys::path::native(Normalized);
  return Normalized.str().str();
}

llvm::Error checkInsideWorkspace(PathRef Path, PathRef CanonicalWorkspaceRoot,
                                 llvm::vfs::FileSystem &FS) {
  auto Real = fileRenameCanonicalPath(Path, FS);
  if (!Real)
    return Real.takeError();
  if (!fileRenamePathInside(CanonicalWorkspaceRoot, *Real))
    return error("file rename path is outside the workspace: {0}", Path);
  return llvm::Error::success();
}

llvm::Expected<llvm::vfs::Status> status(PathRef Path,
                                         llvm::vfs::FileSystem &FS) {
  auto S = FS.status(Path);
  if (!S)
    return error("cannot inspect file rename path {0}: {1}", Path,
                 S.getError().message());
  return *S;
}

llvm::Error checkDestination(PathRef OldPath, PathRef NewPath,
                             const llvm::vfs::Status &OldStatus,
                             llvm::vfs::FileSystem &FS) {
  if (auto Existing = FS.status(NewPath)) {
    auto RealOld = fileRenameCanonicalPath(OldPath, FS);
    if (!RealOld)
      return RealOld.takeError();
    auto RealNew = fileRenameCanonicalPath(NewPath, FS);
    if (!RealNew)
      return RealNew.takeError();
    if (*RealOld != *RealNew ||
        Existing->getUniqueID() != OldStatus.getUniqueID())
      return error("rename destination already exists: {0}", NewPath);
  } else if (Existing.getError() != std::errc::no_such_file_or_directory) {
    return error("cannot inspect rename destination {0}: {1}", NewPath,
                 Existing.getError().message());
  }
  return llvm::Error::success();
}

llvm::Expected<bool> caseSensitiveAt(PathRef Path, llvm::vfs::FileSystem &FS) {
  llvm::SmallString<256> Existing(llvm::sys::path::parent_path(Path));
  while (!Existing.empty()) {
    if (auto S = FS.status(Existing))
      break;
    else if (S.getError() != std::errc::no_such_file_or_directory)
      return error("cannot inspect destination parent {0}: {1}", Existing,
                   S.getError().message());
    llvm::sys::path::remove_filename(Existing);
  }
  while (!Existing.empty()) {
    llvm::StringRef Name = llvm::sys::path::filename(Existing);
    std::string Changed = Name.str();
    auto Letter =
        llvm::find_if(Changed, [](char C) { return llvm::isAlpha(C); });
    if (Letter != Changed.end()) {
      *Letter = llvm::isLower(*Letter) ? llvm::toUpper(*Letter)
                                       : llvm::toLower(*Letter);
      llvm::SmallString<256> Probe(llvm::sys::path::parent_path(Existing));
      llvm::sys::path::append(Probe, Changed);
      auto Original = FS.status(Existing);
      if (!Original)
        return error("cannot inspect destination parent {0}: {1}", Existing,
                     Original.getError().message());
      auto Alternate = FS.status(Probe);
      if (Alternate)
        return Alternate->getUniqueID() != Original->getUniqueID();
      if (Alternate.getError() == std::errc::no_such_file_or_directory)
        return true;
      return error("cannot probe filesystem case behavior at {0}: {1}", Probe,
                   Alternate.getError().message());
    }
    llvm::sys::path::remove_filename(Existing);
  }
  return error("cannot determine filesystem case behavior for {0}", Path);
}

llvm::Expected<std::string> destinationKey(PathRef Path,
                                           llvm::vfs::FileSystem &FS) {
  auto Sensitive = caseSensitiveAt(Path, FS);
  if (!Sensitive)
    return Sensitive.takeError();
  return *Sensitive ? Path.str() : Path.lower();
}

llvm::Error
addMapping(Path OldPath, Path NewPath, llvm::vfs::FileSystem &FS,
           std::vector<FileRenameMapping> &Result,
           llvm::DenseMap<llvm::sys::fs::UniqueID, size_t> &ByIdentity,
           llvm::StringMap<size_t> &ByDestination) {
  auto OldStatus = status(OldPath, FS);
  if (!OldStatus)
    return OldStatus.takeError();
  if (!OldStatus->isRegularFile())
    return error("rename source is not a regular file: {0}", OldPath);

  if (auto Err = checkDestination(OldPath, NewPath, *OldStatus, FS))
    return Err;

  auto ExistingIdentity = ByIdentity.find(OldStatus->getUniqueID());
  if (ExistingIdentity != ByIdentity.end()) {
    const auto &Previous = Result[ExistingIdentity->second];
    auto PreviousKey = destinationKey(Previous.NewPath, FS);
    if (!PreviousKey)
      return PreviousKey.takeError();
    auto NewKey = destinationKey(NewPath, FS);
    if (!NewKey)
      return NewKey.takeError();
    if (*PreviousKey != *NewKey)
      return error("the same file is renamed to both {0} and {1}",
                   Previous.NewPath, NewPath);
    return llvm::Error::success();
  }

  auto DestinationKey = destinationKey(NewPath, FS);
  if (!DestinationKey)
    return DestinationKey.takeError();
  if (auto ExistingDestination = ByDestination.find(*DestinationKey);
      ExistingDestination != ByDestination.end())
    return error("multiple files are renamed to {0}", NewPath);

  ByIdentity[OldStatus->getUniqueID()] = Result.size();
  ByDestination[*DestinationKey] = Result.size();
  Result.push_back(
      {std::move(OldPath), std::move(NewPath), OldStatus->getUniqueID()});
  return llvm::Error::success();
}

llvm::Expected<std::pair<size_t, size_t>>
literalOperandRange(const Inclusion &Inc, llvm::StringRef Code) {
  size_t Pos = Inc.HashOffset;
  if (Pos >= Code.size() || Code[Pos] != '#')
    return error("include at offset {0} has no literal source range",
                 Inc.HashOffset);
  ++Pos;
  while (Pos < Code.size() && llvm::isSpace(Code[Pos]) && Code[Pos] != '\n' &&
         Code[Pos] != '\r')
    ++Pos;

  llvm::StringRef Keyword;
  switch (Inc.Directive) {
  case tok::pp_include:
    Keyword = "include";
    break;
  case tok::pp_include_next:
    return error("cannot prove #include_next resolution at offset {0}",
                 Inc.HashOffset);
  case tok::pp_import:
    Keyword = "import";
    break;
  default:
    return error("unsupported inclusion directive at offset {0}",
                 Inc.HashOffset);
  }
  if (!Code.substr(Pos).starts_with(Keyword))
    return error("inclusion directive at offset {0} is not spelled literally",
                 Inc.HashOffset);
  Pos += Keyword.size();
  if (Pos >= Code.size() || !llvm::isSpace(Code[Pos]))
    return error("inclusion directive at offset {0} is not spelled literally",
                 Inc.HashOffset);
  while (Pos < Code.size() && llvm::isSpace(Code[Pos]) && Code[Pos] != '\n' &&
         Code[Pos] != '\r')
    ++Pos;
  if (!Code.substr(Pos).starts_with(Inc.Written))
    return error("include operand at offset {0} is macro-generated or "
                 "otherwise not a literal",
                 Inc.HashOffset);
  return std::pair<size_t, size_t>{Pos, Pos + Inc.Written.size()};
}

llvm::Error verifyNewIncludeResolution(llvm::StringRef Written,
                                       PathRef IncludingFile, PathRef NewTarget,
                                       HeaderSearch &HeaderSearchInfo,
                                       PathRef BuildDir,
                                       llvm::ArrayRef<FileRenameMapping> Renames,
                                       llvm::vfs::FileSystem &FS) {
  if (Written.size() < 2 ||
      !((Written.front() == '"' && Written.back() == '"') ||
        (Written.front() == '<' && Written.back() == '>')))
    return error("calculated include is not a literal: {0}", Written);
  bool Quoted = Written.front() == '"';
  llvm::StringRef Name = Written.drop_front().drop_back();
  if (llvm::sys::path::is_absolute(Name))
    return error("calculated include path is absolute: {0}", Written);

  auto IdentityAfterRename =
      [&](PathRef RawPath)
      -> llvm::Expected<std::optional<llvm::sys::fs::UniqueID>> {
    llvm::SmallString<256> Path(RawPath);
    llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
    for (const auto &Mapping : Renames)
      if (pathEqual(Path, Mapping.NewPath))
        return std::optional<llvm::sys::fs::UniqueID>(Mapping.OldIdentity);
    for (const auto &Mapping : Renames)
      if (pathEqual(Path, Mapping.OldPath))
        return std::optional<llvm::sys::fs::UniqueID>();
    auto Existing = FS.status(Path);
    if (Existing)
      return Existing->isRegularFile()
                 ? std::optional<llvm::sys::fs::UniqueID>(
                       Existing->getUniqueID())
                 : std::optional<llvm::sys::fs::UniqueID>();
    if (Existing.getError() == std::errc::no_such_file_or_directory)
      return std::optional<llvm::sys::fs::UniqueID>();
    return error("cannot inspect include candidate {0}: {1}", Path,
                 Existing.getError().message());
  };

  auto ExpectedIdentity = IdentityAfterRename(NewTarget);
  if (!ExpectedIdentity)
    return ExpectedIdentity.takeError();
  if (!*ExpectedIdentity)
    return error("renamed include target does not exist after rename: {0}",
                 NewTarget);

  auto CheckDirectory = [&](PathRef RawDirectory) -> llvm::Expected<bool> {
    llvm::SmallString<256> Directory(RawDirectory);
    if (!llvm::sys::path::is_absolute(Directory)) {
      llvm::SmallString<256> Absolute(BuildDir);
      llvm::sys::path::append(Absolute, Directory);
      Directory = std::move(Absolute);
    }
    llvm::SmallString<256> Candidate(Directory);
    llvm::sys::path::append(Candidate, Name);
    llvm::sys::path::remove_dots(Candidate, /*remove_dot_dot=*/true);
    auto CandidateIdentity = IdentityAfterRename(Candidate);
    if (!CandidateIdentity)
      return CandidateIdentity.takeError();
    if (!*CandidateIdentity)
      return false;
    if (**CandidateIdentity == **ExpectedIdentity)
      return true;
    return error("calculated include {0} would resolve to file {1} instead "
                 "of {2} after the rename",
                 Written, Candidate, NewTarget);
  };

  if (Quoted) {
    auto Matches = CheckDirectory(llvm::sys::path::parent_path(IncludingFile));
    if (!Matches)
      return Matches.takeError();
    if (*Matches)
      return llvm::Error::success();
  }

  const HeaderSearch &Search = HeaderSearchInfo;
  auto It = Quoted ? Search.search_dir_begin() : Search.angled_dir_begin();
  for (auto End = Search.search_dir_end(); It != End; ++It) {
    if (!It->isNormalDir())
      return error("cannot prove include resolution through search entry {0}",
                   It->getName());
    auto Matches = CheckDirectory(It->getName());
    if (!Matches)
      return Matches.takeError();
    if (*Matches)
      return llvm::Error::success();
  }
  return error("calculated include {0} does not resolve to renamed file {1}",
               Written, NewTarget);
}

std::optional<std::string> relativeIncludePath(PathRef IncludingFile,
                                               PathRef Target) {
  llvm::SmallString<256> Base(llvm::sys::path::parent_path(IncludingFile));
  llvm::SmallString<256> Destination(Target);
  llvm::sys::path::remove_dots(Base, /*remove_dot_dot=*/true);
  llvm::sys::path::remove_dots(Destination, /*remove_dot_dot=*/true);
  if (llvm::sys::path::root_name(Base) !=
          llvm::sys::path::root_name(Destination) ||
      llvm::sys::path::has_root_directory(Base) !=
          llvm::sys::path::has_root_directory(Destination))
    return std::nullopt;

  auto BaseIt = llvm::sys::path::begin(Base);
  auto BaseEnd = llvm::sys::path::end(Base);
  auto DestinationIt = llvm::sys::path::begin(Destination);
  auto DestinationEnd = llvm::sys::path::end(Destination);
  while (BaseIt != BaseEnd && DestinationIt != DestinationEnd &&
         *BaseIt == *DestinationIt) {
    ++BaseIt;
    ++DestinationIt;
  }

  llvm::SmallString<256> Relative;
  for (; BaseIt != BaseEnd; ++BaseIt)
    if (!BaseIt->empty() && !llvm::sys::path::is_separator(BaseIt->front()))
      llvm::sys::path::append(Relative, "..");
  for (; DestinationIt != DestinationEnd; ++DestinationIt)
    if (!DestinationIt->empty() &&
        !llvm::sys::path::is_separator(DestinationIt->front()))
      llvm::sys::path::append(Relative, *DestinationIt);
  if (Relative.empty())
    return std::nullopt;
  return llvm::sys::path::convert_to_slash(Relative);
}

} // namespace

llvm::Expected<std::vector<FileRenameMapping>>
expandFileRenames(llvm::ArrayRef<std::pair<Path, Path>> Renames,
                  PathRef WorkspaceRoot, llvm::vfs::FileSystem &FS) {
  if (Renames.empty())
    return error("file rename contains no paths");
  auto NormalizedRoot = normalizeAbsolute(WorkspaceRoot);
  if (!NormalizedRoot)
    return NormalizedRoot.takeError();
  auto CanonicalRoot = fileRenameCanonicalPath(*NormalizedRoot, FS);
  if (!CanonicalRoot)
    return CanonicalRoot.takeError();

  std::vector<FileRenameMapping> Result;
  llvm::DenseMap<llvm::sys::fs::UniqueID, size_t> ByIdentity;
  llvm::StringMap<size_t> ByDestination;
  for (const auto &[RawOld, RawNew] : Renames) {
    auto Old = normalizeAbsolute(RawOld);
    if (!Old)
      return Old.takeError();
    auto New = normalizeAbsolute(RawNew);
    if (!New)
      return New.takeError();
    if (auto Err = checkInsideWorkspace(*Old, *CanonicalRoot, FS))
      return std::move(Err);
    if (auto Err = checkInsideWorkspace(*New, *CanonicalRoot, FS))
      return std::move(Err);
    if (*Old == *New)
      return error("rename source and destination are identical: {0}", *Old);

    auto OldStatus = status(*Old, FS);
    if (!OldStatus)
      return OldStatus.takeError();
    if (OldStatus->isRegularFile()) {
      if (auto Err = addMapping(std::move(*Old), std::move(*New), FS, Result,
                                ByIdentity, ByDestination))
        return std::move(Err);
      continue;
    }
    if (!OldStatus->isDirectory())
      return error("rename source is neither a file nor directory: {0}", *Old);
    auto CanonicalOld = fileRenameCanonicalPath(*Old, FS);
    if (!CanonicalOld)
      return CanonicalOld.takeError();
    auto CanonicalNew = fileRenameCanonicalPath(*New, FS);
    if (!CanonicalNew)
      return CanonicalNew.takeError();
    if (fileRenamePathInside(*CanonicalOld, *CanonicalNew))
      return error("rename destination is inside its source directory: {0}",
                   *New);
    if (auto Err = checkDestination(*Old, *New, *OldStatus, FS))
      return std::move(Err);
    if (fileRenamePrunesDirectory(*Old))
      continue;

    std::error_code EC;
    llvm::vfs::recursive_directory_iterator It(FS, *Old, EC), End;
    if (EC)
      return error("cannot enumerate rename directory {0}: {1}", *Old,
                   EC.message());
    for (; It != End; It.increment(EC)) {
      if (EC)
        return error("cannot enumerate rename directory {0}: {1}", *Old,
                     EC.message());
      auto EntryStatus = FS.status(It->path());
      if (!EntryStatus)
        return error("cannot inspect rename entry {0}: {1}", It->path(),
                     EntryStatus.getError().message());
      if (EntryStatus->isDirectory() && fileRenamePrunesDirectory(It->path())) {
        It.no_push();
        continue;
      }
      if (!EntryStatus->isRegularFile())
        continue;
      llvm::SmallString<256> Relative(It->path());
      if (!llvm::sys::path::replace_path_prefix(Relative, *Old, ""))
        return error("rename entry is outside its source directory: {0}",
                     It->path());
      llvm::SmallString<256> Destination(*New);
      llvm::sys::path::append(Destination,
                              llvm::sys::path::relative_path(Relative));
      auto NormalizedDestination = normalizeAbsolute(Destination);
      if (!NormalizedDestination)
        return NormalizedDestination.takeError();
      if (auto Err =
              addMapping(It->path().str(), std::move(*NormalizedDestination),
                         FS, Result, ByIdentity, ByDestination))
        return std::move(Err);
    }
    if (EC)
      return error("cannot enumerate rename directory {0}: {1}", *Old,
                   EC.message());
  }
  return Result;
}

llvm::Error validateCompatibleFileRenameEdits(PathRef File,
                                              llvm::ArrayRef<TextEdit> Expected,
                                              llvm::ArrayRef<TextEdit> Actual) {
  bool Equal =
      Expected.size() == Actual.size() &&
      llvm::equal(Expected, Actual, [](const TextEdit &L, const TextEdit &R) {
        return L.range == R.range && L.newText == R.newText &&
               L.annotationId == R.annotationId;
      });
  if (!Equal)
    return error("compilation contexts require incompatible include edits for "
                 "{0}",
                 File);
  return llvm::Error::success();
}

llvm::Expected<std::vector<TextEdit>> renameIncludeDirectives(
    PathRef File, llvm::StringRef Code, const IncludeStructure &Includes,
    HeaderSearch &HeaderSearchInfo, PathRef BuildDir,
    llvm::ArrayRef<FileRenameMapping> Renames, const format::FormatStyle &Style,
    llvm::vfs::FileSystem &FS) {
  Path EffectiveFile = File.str();
  auto FileStatus = FS.status(File);
  if (!FileStatus)
    return error("cannot inspect includer {0}: {1}", File,
                 FileStatus.getError().message());
  if (auto MovedIncluder = llvm::find_if(Renames,
                                         [&](const auto &Candidate) {
                                           return Candidate.OldIdentity ==
                                                  FileStatus->getUniqueID();
                                         });
      MovedIncluder != Renames.end()) {
    if (File != MovedIncluder->OldPath)
      return error("cannot disambiguate moved includer {0} from filesystem "
                   "alias {1}",
                   MovedIncluder->OldPath, File);
    EffectiveFile = MovedIncluder->NewPath;
  }

  IncludeInserter Inserter(EffectiveFile, Code, Style, BuildDir,
                           &HeaderSearchInfo,
                           Config::current().Style.QuotedHeaders,
                           Config::current().Style.AngledHeaders);
  bool IncluderMoved = EffectiveFile != File;
  std::vector<TextEdit> Result;
  for (const Inclusion &Inc : Includes.MainFileIncludes) {
    if (Inc.Resolved.empty())
      return error("cannot prove unresolved include {0} in {1}", Inc.Written,
                   File);
    auto IncludedStatus = FS.status(Inc.Resolved);
    if (!IncludedStatus)
      return error("cannot inspect resolved include {0} in {1}: {2}",
                   Inc.Resolved, File, IncludedStatus.getError().message());
    const auto *RenamedTarget =
        llvm::find_if(Renames, [&](const auto &Candidate) {
          return Candidate.OldIdentity == IncludedStatus->getUniqueID();
        });
    if (RenamedTarget != Renames.end() &&
        Inc.Resolved != RenamedTarget->OldPath)
      return error("cannot disambiguate renamed include {0} from filesystem "
                   "alias {1}",
                   RenamedTarget->OldPath, Inc.Resolved);
    if (RenamedTarget == Renames.end() && !IncluderMoved) {
      if (auto Err = verifyNewIncludeResolution(
              Inc.Written, EffectiveFile, Inc.Resolved, HeaderSearchInfo,
              BuildDir, Renames, FS))
        return std::move(Err);
      continue;
    }

    auto OperandRange = literalOperandRange(Inc, Code);
    if (!OperandRange)
      return OperandRange.takeError();
    PathRef NewTarget = RenamedTarget == Renames.end()
                            ? PathRef(Inc.Resolved)
                            : PathRef(RenamedTarget->NewPath);
    auto NewWritten = Inserter.calculateIncludePath(
        HeaderFile{NewTarget.str(), /*Verbatim=*/false}, EffectiveFile);
    if (!NewWritten) {
      auto Relative = relativeIncludePath(EffectiveFile, NewTarget);
      if (!Relative)
        return error("cannot calculate an include path from {0} to {1}", File,
                     NewTarget);
      NewWritten = "\"" + *Relative + "\"";
    }
    if (Inc.Written.front() == '<' && NewWritten->front() == '"') {
      NewWritten->front() = '<';
      NewWritten->back() = '>';
    } else if (Inc.Written.front() == '"' && NewWritten->front() == '<') {
      NewWritten->front() = '"';
      NewWritten->back() = '"';
    }
    llvm::StringRef NewSpelling = *NewWritten;
    if (NewSpelling.size() < 2 ||
        ((NewSpelling.front() != '"' || NewSpelling.back() != '"') &&
         (NewSpelling.front() != '<' || NewSpelling.back() != '>')) ||
        NewSpelling.drop_front().drop_back().contains(
            NewSpelling.front() == '"' ? '"' : '>') ||
        NewSpelling.contains('\n') || NewSpelling.contains('\r') ||
        NewSpelling.contains('\0'))
      return error("cannot represent renamed include path {0} as a valid "
                   "header-name token",
                   NewTarget);
    if (auto Err =
            verifyNewIncludeResolution(*NewWritten, EffectiveFile, NewTarget,
                                       HeaderSearchInfo, BuildDir, Renames,
                                       FS))
      return std::move(Err);
    if (*NewWritten == Inc.Written)
      continue;

    TextEdit Edit;
    Edit.range = Range{offsetToPosition(Code, OperandRange->first),
                       offsetToPosition(Code, OperandRange->second)};
    Edit.newText = std::move(*NewWritten);
    Result.push_back(std::move(Edit));
  }
  llvm::sort(Result, [](const TextEdit &L, const TextEdit &R) {
    return L.range < R.range;
  });
  for (size_t I = 1; I < Result.size(); ++I)
    if (!(Result[I - 1].range.end <= Result[I].range.start))
      return error("file rename produces overlapping edits in {0}", File);
  return Result;
}

} // namespace clangd
} // namespace clang
