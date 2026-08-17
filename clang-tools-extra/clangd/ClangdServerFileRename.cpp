//===--- ClangdServerFileRename.cpp - File rename requests ------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "Compiler.h"
#include "CompilerInvocation.h"
#include "Diagnostics.h"
#include "FileRename.h"
#include "FileRenameInternal.h"
#include "Format.h"
#include "Headers.h"
#include "ParsedAST.h"
#include "SourceCode.h"
#include "URI.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "support/ThreadsafeFS.h"
#include "clang/Format/Format.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

/// A filesystem view whose drafts are fixed at construction. This is used by
/// multi-file operations that must not mix document versions.
class FrozenDraftFS : public ThreadsafeFS {
public:
  FrozenDraftFS(const ThreadsafeFS &Base,
                llvm::ArrayRef<std::pair<Path, DraftStore::Draft>> Drafts)
      : Base(Base),
        FrozenDrafts(
            llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>()) {
    for (const auto &[File, Draft] : Drafts) {
      assert(Draft.Contents && "draft has no contents");
      bool Added = FrozenDrafts->addFile(
          File, /*ModificationTime=*/0,
          llvm::MemoryBuffer::getMemBufferCopy(*Draft.Contents, File));
      assert(Added && "duplicate draft path");
      (void)Added;
    }
  }

private:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    auto Result = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        Base.view(std::nullopt));
    Result->pushOverlay(FrozenDrafts);
    return Result;
  }

  const ThreadsafeFS &Base;
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> FrozenDrafts;
};

const DraftStore::Draft *
findDraft(llvm::ArrayRef<std::pair<Path, DraftStore::Draft>> Drafts,
          PathRef File) {
  auto It = llvm::find_if(
      Drafts, [&](const auto &Entry) { return pathEqual(Entry.first, File); });
  return It == Drafts.end() ? nullptr : &It->second;
}

bool draftsEqual(llvm::ArrayRef<std::pair<Path, DraftStore::Draft>> Left,
                 llvm::ArrayRef<std::pair<Path, DraftStore::Draft>> Right) {
  if (Left.size() != Right.size())
    return false;
  return llvm::all_of(Left, [&](const auto &Entry) {
    const DraftStore::Draft *Other = findDraft(Right, Entry.first);
    return Other && Entry.second.Version == Other->Version &&
           *Entry.second.Contents == *Other->Contents;
  });
}

llvm::Error validateReparsedIncludeEdges(PathRef File,
                                         const IncludeStructure &Reparsed,
                                         llvm::ArrayRef<Path> IndexedEdges,
                                         llvm::vfs::FileSystem &FS) {
  const auto &Actual = Reparsed.MainFileIncludes;
  if (Actual.size() != IndexedEdges.size())
    return error("standalone parse of {0} has {1} include edges, but its "
                 "indexed translation-unit context has {2}",
                 File, Actual.size(), IndexedEdges.size());
  for (size_t I = 0; I < Actual.size(); ++I) {
    if (Actual[I].Resolved.empty())
      return error("standalone parse of {0} has an unresolved include at "
                   "offset {1}",
                   File, Actual[I].HashOffset);
    auto ActualStatus = FS.status(Actual[I].Resolved);
    if (!ActualStatus)
      return error("cannot inspect reparsed include {0} from {1}: {2}",
                   Actual[I].Resolved, File, ActualStatus.getError().message());
    auto IndexedStatus = FS.status(IndexedEdges[I]);
    if (!IndexedStatus)
      return error("cannot inspect indexed include {0} from {1}: {2}",
                   IndexedEdges[I], File, IndexedStatus.getError().message());
    if (ActualStatus->getUniqueID() != IndexedStatus->getUniqueID())
      return error("standalone parse of {0} resolves include {1} to {2}, but "
                   "its indexed translation-unit context resolves it to {3}",
                   File, Actual[I].Written, Actual[I].Resolved,
                   IndexedEdges[I]);
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<int64_t>>
draftVersion(const DraftStore::Draft *Draft, PathRef File) {
  if (!Draft)
    return std::optional<int64_t>();
  int64_t Version;
  if (!llvm::to_integer(Draft->Version, Version))
    return error("open document {0} has invalid version {1}", File,
                 Draft->Version);
  return std::optional<int64_t>(Version);
}

std::vector<std::pair<Path, Path>>
effectiveRenames(llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  std::vector<std::pair<Path, Path>> Result;
  for (const auto &[Old, New] : Renames) {
    Path NormalizedOld = removeDots(Old);
    Path NormalizedNew = removeDots(New);
    if (!pathEqual(NormalizedOld, NormalizedNew))
      Result.emplace_back(std::move(NormalizedOld), std::move(NormalizedNew));
  }
  return Result;
}

} // namespace

void ClangdServer::prepareFileRename(
    llvm::ArrayRef<std::pair<Path, Path>> Renames, Callback<WorkspaceEdit> CB) {
  std::vector<std::pair<Path, Path>> OwnedRenames = effectiveRenames(Renames);
  if (OwnedRenames.empty()) {
    WorkspaceEdit Result;
    Result.documentChanges.emplace();
    return CB(std::move(Result));
  }
  if (!BackgroundIdx)
    return CB(error("file rename requires background indexing"));
  if (!WorkspaceRoot)
    return CB(error("file rename requires a workspace root"));
  WorkScheduler->run(
      "FileRename", /*Path=*/"",
      [this, Renames = std::move(OwnedRenames), CB = std::move(CB)]() mutable {
        auto Drafts = DraftMgr.getDrafts();
        FrozenDraftFS SnapshotFS(TFS, Drafts);
        auto FS = SnapshotFS.view(std::nullopt);
        auto Workspace = FileRenameWorkspace->snapshot(*FS);
        if (!Workspace)
          return CB(Workspace.takeError());
        const auto &WorkspaceFiles = Workspace->Sources;
        for (const auto &Rename : Renames) {
          Path Old = removeDots(Rename.first);
          Path New = removeDots(Rename.second);
          static constexpr llvm::StringLiteral MetadataFiles[] = {
              ".clangd", "compile_commands.json", "compile_flags.txt"};
          for (llvm::StringLiteral Metadata : MetadataFiles)
            if (pathEqual(llvm::sys::path::filename(Old), Metadata) ||
                pathEqual(llvm::sys::path::filename(New), Metadata))
              return CB(error("cannot rename configuration or compilation "
                              "database file {0}",
                              Old));
          for (PathRef Pruned : Workspace->PrunedMetadataRoots.keys())
            if (pathStartsWith(Old, Pruned) || pathStartsWith(Pruned, Old) ||
                pathStartsWith(New, Pruned) || pathStartsWith(Pruned, New))
              return CB(error("file rename intersects unscanned metadata root "
                              "{0}",
                              Pruned));
        }
        auto Mappings = expandFileRenames(Renames, *WorkspaceRoot, *FS);
        if (!Mappings)
          return CB(Mappings.takeError());
        // Every standalone source must trigger project discovery. The CDB
        // broadcaster is asynchronous, so wait for it before waiting for the
        // indexing work that its notification creates.
        for (const WorkspaceSourceFile &File : WorkspaceFiles)
          if (!File.IsHeader)
            (void)CDB.getCompileCommand(File.File);
        if (!CDB.blockUntilIdle(timeoutSeconds(30)))
          return CB(error("compilation database did not become ready"));
        if (!BackgroundIdx->blockUntilIdle(/*TimeoutSeconds=*/30))
          return CB(error("background index did not become ready"));
        BackgroundIdx->ensureIncludeGraph();
        if (!BackgroundIdx->blockUntilIdle(/*TimeoutSeconds=*/30))
          return CB(error("background include graph did not become ready"));

        auto Graph = BackgroundIdx->includeGraphSnapshot();
        if (!Graph)
          return CB(Graph.takeError());
        if (Graph->Files.empty())
          return CB(error("background include graph is empty"));

        llvm::StringMap<FileDigest> GraphDigests;
        for (const auto &Node : Graph->Files) {
          auto [It, Inserted] =
              GraphDigests.try_emplace(Node.File, Node.Digest);
          if (!Inserted && It->getValue() != Node.Digest)
            return CB(error("background contexts disagree on the digest for "
                            "{0}",
                            Node.File));
        }

        FileRenameDirectiveCache DirectiveScans(*FS);
        auto ScanDirectives = [&](PathRef File)
            -> llvm::Expected<const FileRenameDirectiveScan *> {
          FileDigest ExpectedDigest;
          if (const DraftStore::Draft *Draft = findDraft(Drafts, File)) {
            ExpectedDigest = digest(*Draft->Contents);
          } else {
            auto Expected = Workspace->Digests.find(File);
            if (Expected != Workspace->Digests.end())
              ExpectedDigest = Expected->getValue();
            else {
              auto Indexed = GraphDigests.find(File);
              if (Indexed == GraphDigests.end())
                return error("no frozen digest is available for {0}", File);
              ExpectedDigest = Indexed->getValue();
            }
          }
          return DirectiveScans.scan(File, ExpectedDigest);
        };

        llvm::StringSet<> TranslationUnits;
        for (PathRef TU : Graph->TranslationUnits)
          TranslationUnits.insert(TU);
        for (const WorkspaceSourceFile &File : WorkspaceFiles) {
          bool Represented =
              File.IsHeader
                  ? llvm::any_of(Graph->Files,
                                 [&](const auto &N) {
                                   return pathEqual(N.File, File.File);
                                 })
                  : TranslationUnits.contains(File.File);
          if (!Represented && File.IsHeader) {
            auto Scan = ScanDirectives(File.File);
            if (!Scan)
              return CB(Scan.takeError());
            if (!(*Scan)->HasIncludeDirectives)
              continue;
          }
          if (!Represented)
            return CB(error("workspace source is not represented in the "
                            "background include graph: {0}",
                            File.File));
        }

        std::vector<FileRenameCompileCommand> ValidatedCommands;
        ValidatedCommands.reserve(Graph->Commands.size());
        for (const auto &Command : Graph->Commands) {
          auto CC1 = Graph->CC1Commands.find(Command.first());
          if (CC1 == Graph->CC1Commands.end())
            return CB(error("no driver-derived compilation command is "
                            "available for {0}",
                            Command.first()));
          if (auto Err = validateCompileCommandForRenames(
                  Command.getValue(), Renames, *Mappings, FS.get(),
                  CC1->getValue()))
            return CB(std::move(Err));
          ValidatedCommands.push_back(
              {Command.first().str(), Command.getValue()});
        }

        if (auto Err = CDB.prepareFileRenames(Renames, ValidatedCommands))
          return CB(std::move(Err));
        llvm::scope_exit DiscardCDBPlan(
            [&] { CDB.discardPreparedFileRenames(); });
        for (const auto &Command : Graph->Commands) {
          auto NewTU = mapPathAfterRenames(Command.first(), Renames);
          if (!NewTU)
            return CB(NewTU.takeError());
          if (pathEqual(*NewTU, Command.first()))
            continue;
          WithContext DestinationContext(ContextProvider
                                             ? ContextProvider(*NewTU)
                                             : Context::current().clone());
          auto Future = CDB.getCompileCommandAfterPreparedFileRenames(*NewTU);
          if (!Future)
            return CB(error("no post-rename compilation command is available "
                            "for {0}",
                            *NewTU));
          auto Expected =
              projectCompileCommandAfterRenames(Command.getValue(), Renames);
          if (!Expected)
            return CB(Expected.takeError());
          if (*Future != *Expected)
            return CB(error("compilation command or configuration changes "
                            "when {0} is renamed to {1}",
                            Command.first(), *NewTU));
        }

        auto InWorkspace = [&](PathRef File) {
          return pathEqual(File, *WorkspaceRoot) ||
                 pathStartsWith(*WorkspaceRoot, File);
        };
        for (const auto &Node : Graph->Files) {
          if (!InWorkspace(Node.File))
            continue;
          auto Inventory = Workspace->Files.find(Node.File);
          bool Pruned = llvm::any_of(
              Workspace->PrunedMetadataRoots.keys(),
              [&](PathRef Root) { return pathStartsWith(Root, Node.File); });
          if (Pruned || (Inventory != Workspace->Files.end() &&
                         (Inventory->getValue().Classification ==
                              WorkspaceFileClassification::Binary ||
                          Inventory->getValue().Classification ==
                              WorkspaceFileClassification::Metadata)))
            return CB(error("background include graph represents a workspace "
                            "file excluded from source inventory: {0}",
                            Node.File));
          if (Node.Flags & IncludeGraphNode::SourceFlag::HadErrors)
            return CB(
                error("background index contains errors for {0}", Node.File));
        }

        // Every represented closed file contributes to the proof, even when
        // its old graph had no includes and it would not otherwise become an
        // edit candidate.
        llvm::StringMap<FileDigest> IndexedDigests;
        for (const auto &Entry : GraphDigests) {
          if (!InWorkspace(Entry.first()) || findDraft(Drafts, Entry.first()))
            continue;
          IndexedDigests.try_emplace(Entry.first(), Entry.getValue());
        }
        for (const auto &Entry : IndexedDigests) {
          auto Current = Workspace->Digests.find(Entry.first());
          if (Current == Workspace->Digests.end())
            return CB(error("indexed workspace file is missing from the "
                            "workspace inventory: {0}",
                            Entry.first()));
          if (Current->getValue() != Entry.getValue())
            return CB(error("background include graph is stale for {0}",
                            Entry.first()));
        }

        auto IsRenamed = [&](PathRef File) -> llvm::Expected<bool> {
          auto S = FS->status(File);
          if (!S)
            return error("cannot inspect include graph path {0}: {1}", File,
                         S.getError().message());
          return llvm::any_of(*Mappings, [&](const auto &Mapping) {
            return Mapping.OldIdentity == S->getUniqueID();
          });
        };

        auto ModuleMapDependencyAffected =
            [&](PathRef ModuleMap, const UneditableFileDependency &Dependency)
            -> llvm::Expected<bool> {
          llvm::StringRef Written = llvm::StringRef(Dependency.Written).trim();
          if (Written.size() < 2 ||
              !((Written.front() == '"' && Written.back() == '"') ||
                (Written.front() == '<' && Written.back() == '>')))
            return error("cannot interpret module-map dependency in {0}: {1}",
                         ModuleMap, Written);
          Written = Written.drop_front().drop_back();
          if (Written.empty() || Written.contains('\\'))
            return error("cannot interpret module-map dependency in {0}: {1}",
                         ModuleMap, Written);
          llvm::SmallString<256> Absolute(Written);
          if (!llvm::sys::path::is_absolute(Absolute)) {
            Absolute = llvm::sys::path::parent_path(ModuleMap);
            llvm::sys::path::append(Absolute, Written);
          }
          llvm::sys::path::remove_dots(Absolute,
                                       /*remove_dot_dot=*/true);
          if (Dependency.IncludesDescendants)
            for (const auto &Rename : Renames)
              if (pathStartsWith(Absolute, Rename.first) ||
                  pathStartsWith(Absolute, Rename.second))
                return true;
          auto Mapped = mapPathAfterRenames(Absolute, Renames);
          if (!Mapped)
            return Mapped.takeError();
          if (!pathEqual(Absolute, *Mapped))
            return true;
          for (const auto &Mapping : *Mappings) {
            if (pathEqual(Absolute, Mapping.NewPath))
              return true;
            if (Dependency.IncludesDescendants &&
                (pathStartsWith(Absolute, Mapping.OldPath) ||
                 pathStartsWith(Absolute, Mapping.NewPath)))
              return true;
          }
          auto Canonical = fileRenameCanonicalPath(Absolute, *FS);
          if (!Canonical)
            return Canonical.takeError();
          for (const auto &Mapping : *Mappings) {
            auto CanonicalNew = fileRenameCanonicalPath(Mapping.NewPath, *FS);
            if (!CanonicalNew)
              return CanonicalNew.takeError();
            if (pathEqual(*Canonical, *CanonicalNew) ||
                (Dependency.IncludesDescendants &&
                 (pathStartsWith(*Canonical, *CanonicalNew) ||
                  pathStartsWith(*CanonicalNew, *Canonical))))
              return true;
          }
          auto Renamed = IsRenamed(Absolute);
          if (!Renamed)
            return Renamed.takeError();
          return *Renamed;
        };

        auto ValidateScannedDependencies =
            [&](PathRef File, const FileRenameDirectiveScan &Scan,
                bool External) -> llvm::Error {
          for (const auto &Dependency : Scan.UneditableDependencies) {
            if (Dependency.Kind == "module-map dependency") {
              auto Affected = ModuleMapDependencyAffected(File, Dependency);
              if (!Affected)
                return Affected.takeError();
              if (!*Affected)
                continue;
            }
            return error(External
                             ? "cannot prove file rename with {0} outside the "
                               "workspace in {1}: {2}"
                             : "cannot prove file rename with {0} in {1}: {2}",
                         Dependency.Kind, File,
                         Dependency.Written.empty()
                             ? llvm::StringRef("<unresolved>")
                             : llvm::StringRef(Dependency.Written));
          }
          return llvm::Error::success();
        };

        bool HasIncludeAliasState = false;
        llvm::StringMap<FileDigest> ExternalDigests;
        WorkspaceSourceLimits ProofLimits;
        uint64_t ProofFiles = 0;
        uint64_t ProofBytes = 0;
        auto AccountDirectiveScan =
            [&](PathRef File,
                const FileRenameDirectiveScan &Scan) -> llvm::Error {
          if (ProofFiles == ProofLimits.MaxFiles)
            return error("file-rename dependency proof exceeds the file-count "
                         "limit of {0}",
                         ProofLimits.MaxFiles);
          ++ProofFiles;
          uint64_t Size = Scan.Contents.size();
          if (Size > ProofLimits.MaxTextFileBytes ||
              ProofBytes > ProofLimits.MaxDirectiveScanBytes - Size)
            return error("file-rename dependency proof exceeds its scan-byte "
                         "limit while reading {0}",
                         File);
          ProofBytes += Size;
          return llvm::Error::success();
        };
        for (const auto &Entry : Workspace->Digests) {
          auto Scan = ScanDirectives(Entry.first());
          if (!Scan)
            return CB(Scan.takeError());
          if (auto Err = AccountDirectiveScan(Entry.first(), **Scan))
            return CB(std::move(Err));
          HasIncludeAliasState |= (*Scan)->HasIncludeAliasPragma;
          if (auto Err =
                  ValidateScannedDependencies(Entry.first(), **Scan, false))
            return CB(std::move(Err));
        }
        for (const auto &Node : Graph->Files) {
          HasIncludeAliasState |=
              Node.Flags & IncludeGraphNode::SourceFlag::HasIncludeAliasState;
          if (Node.Flags & IncludeGraphNode::SourceFlag::HasFileQuery)
            return CB(error("cannot prove file rename with active "
                            "preprocessor file query in {0}",
                            Node.File));
          if (InWorkspace(Node.File))
            continue;
          auto [DigestIt, Inserted] =
              ExternalDigests.try_emplace(Node.File, Node.Digest);
          if (!Inserted && DigestIt->getValue() != Node.Digest)
            return CB(error("background contexts disagree on the digest for "
                            "external file {0}",
                            Node.File));
          if (Inserted && ProofFiles == ProofLimits.MaxFiles)
            return CB(error("file-rename dependency proof exceeds the "
                            "file-count limit of {0}",
                            ProofLimits.MaxFiles));
          uint64_t RemainingBytes =
              ProofLimits.MaxDirectiveScanBytes - ProofBytes;
          uint64_t ScanLimit =
              std::min(ProofLimits.MaxTextFileBytes, RemainingBytes);
          auto Scan = DirectiveScans.scan(Node.File, Node.Digest, ScanLimit);
          if (!Scan)
            return CB(Scan.takeError());
          if (Inserted)
            if (auto Err = AccountDirectiveScan(Node.File, **Scan))
              return CB(std::move(Err));
          HasIncludeAliasState |= (*Scan)->HasIncludeAliasPragma;
          if (auto Err = ValidateScannedDependencies(Node.File, **Scan, true))
            return CB(std::move(Err));
        }
        if (HasIncludeAliasState)
          return CB(error("cannot prove file rename with #pragma "
                          "include_alias state"));

        llvm::StringMap<std::vector<const BackgroundIndex::IndexedFile *>>
            Candidates;
        for (const auto &Node : Graph->Files) {
          auto Scan = ScanDirectives(Node.File);
          if (!Scan)
            return CB(Scan.takeError());
          auto Affected = IsRenamed(Node.File);
          if (!Affected)
            return CB(Affected.takeError());
          if (*Affected &&
              (Node.Flags & IncludeGraphNode::SourceFlag::IsCommandInput))
            return CB(error("file rename moves compiler command input {0}",
                            Node.File));
          for (PathRef Included : Node.DirectIncludes) {
            if (*Affected)
              break;
            auto IncludedAffected = IsRenamed(Included);
            if (!IncludedAffected)
              return CB(IncludedAffected.takeError());
            *Affected = *IncludedAffected;
          }
          bool DestinationMayChangeLookup = llvm::any_of(
              (*Scan)->IncludeSpellings, [&](llvm::StringRef Written) {
                if (Written.size() < 2)
                  return true;
                llvm::StringRef Name = Written.drop_front().drop_back();
                return llvm::any_of(*Mappings, [&](const auto &Mapping) {
                  return pathEqual(llvm::sys::path::filename(Name),
                                   llvm::sys::path::filename(Mapping.NewPath));
                });
              });
          if (!InWorkspace(Node.File)) {
            if (*Affected || DestinationMayChangeLookup)
              return CB(error("file rename requires editing an includer "
                              "outside the workspace: {0}",
                              Node.File));
            continue;
          }
          bool HasConditionalInclude = false;
          if (Node.Flags &
              IncludeGraphNode::SourceFlag::HasConditionalIncludes) {
            HasConditionalInclude = !(*Scan)->ConditionalIncludes.empty();
          }
          if (*Affected || HasConditionalInclude || DestinationMayChangeLookup)
            Candidates[Node.File].push_back(&Node);
        }

        // Once one context makes a file relevant, all contexts must agree on
        // its edits.
        for (const auto &Node : Graph->Files) {
          auto Candidate = Candidates.find(Node.File);
          if (Candidate != Candidates.end() &&
              !llvm::is_contained(Candidate->second, &Node))
            Candidate->second.push_back(&Node);
        }

        // Open documents can differ from their persisted graph node, so parse
        // all of them under every known translation-unit context.
        for (const auto &[File, Draft] : Drafts) {
          if (!InWorkspace(File))
            continue;
          auto &Contexts = Candidates[File];
          for (const auto &Node : Graph->Files) {
            if (pathEqual(Node.File, File) &&
                !llvm::is_contained(Contexts, &Node))
              Contexts.push_back(&Node);
          }
          if (Contexts.empty())
            return CB(error("open document is not represented in the "
                            "background include graph: {0}",
                            File));
        }

        llvm::StringMap<std::string> Contents;
        llvm::StringMap<std::vector<ConditionalInclusion>> ConditionalIncludes;
        llvm::StringSet<> ClosedCandidates;
        for (const auto &Candidate : Candidates) {
          PathRef File = Candidate.first();
          auto Scan = ScanDirectives(File);
          if (!Scan)
            return CB(Scan.takeError());
          Contents[File] = (*Scan)->Contents;
          if (!findDraft(Drafts, File))
            ClosedCandidates.insert(File);
          std::vector<ConditionalInclusion> Conditional =
              (*Scan)->ConditionalIncludes;
          if (!Conditional.empty() && (*Scan)->HasIncludeAliasPragma)
            return CB(error("cannot resolve conditional includes in {0} "
                            "with #pragma include_alias state",
                            File));
          ConditionalIncludes[File] = std::move(Conditional);
          for (const auto *Context : Candidate.getValue())
            if (!findDraft(Drafts, File) &&
                digest(Contents.lookup(File)) != Context->Digest)
              return CB(
                  error("background include graph is stale for {0}", File));
        }

        llvm::StringMap<std::vector<TextEdit>> ProposedEdits;
        for (const auto &Candidate : Candidates) {
          PathRef File = Candidate.first();
          for (const BackgroundIndex::IndexedFile *Context :
               Candidate.getValue()) {
            auto DestinationFile = mapPathAfterRenames(File, Renames);
            if (!DestinationFile)
              return CB(DestinationFile.takeError());
            WithContext FileContext(ContextProvider
                                        ? ContextProvider(*DestinationFile)
                                        : Context::current().clone());
            auto Command = Graph->Commands.find(Context->DependentTU);
            if (Command == Graph->Commands.end())
              return CB(error("no indexed compilation command is available "
                              "for {0}",
                              Context->DependentTU));
            tooling::CompileCommand ParseCommand = Command->getValue();
            if (!pathEqual(File, Context->DependentTU))
              ParseCommand = tooling::transferCompileCommand(
                  std::move(ParseCommand), File);
            tooling::CompileCommand ProbeCommand = ParseCommand;
            ParseInputs Inputs{std::move(ParseCommand), &SnapshotFS,
                               Contents.lookup(File)};
            Inputs.Index = Index;
            Inputs.FeatureModules = FeatureModules;
            Inputs.ModulesManager = ModulesManager;
            Inputs.Opts.ImportInsertions = ImportInsertions;
            adjustParseInputs(Inputs, File);
            StoreDiags Diags;
            auto CI = buildCompilerInvocation(Inputs, Diags);
            if (!CI)
              return CB(
                  error("cannot build compiler invocation for {0}", File));
            auto AST = ParsedAST::build(File, Inputs, std::move(CI),
                                        Diags.take(), /*Preamble=*/nullptr);
            if (!AST)
              return CB(error("cannot parse includer {0}", File));
            if (llvm::any_of(AST->getDiagnostics(), [](const Diag &D) {
                  return D.Severity >= DiagnosticsEngine::Error;
                }))
              return CB(error("cannot safely rename files while {0} has parse "
                              "errors",
                              File));

            IncludeStructure Includes = AST->getIncludeStructure();
            FileDigest ParsedDigest = digest(Inputs.Contents);
            if (!pathEqual(File, Context->DependentTU) &&
                ParsedDigest != Context->Digest)
              return CB(error("cannot prove translation-unit context for "
                              "modified open header {0}",
                              File));
            if (ParsedDigest == Context->Digest)
              if (auto Err = validateReparsedIncludeEdges(
                      File, Includes, Context->DirectIncludes, *FS))
                return CB(std::move(Err));
            const auto &Conditional = ConditionalIncludes.lookup(File);
            for (const ConditionalInclusion &Inclusion : Conditional) {
              if (Inclusion.Written.empty())
                return CB(error("cannot resolve macro-generated conditional "
                                "include in {0}",
                                File));
              llvm::StringRef Keyword;
              switch (Inclusion.Directive) {
              case tok::pp_include:
                Keyword = "include";
                break;
              case tok::pp_import:
                Keyword = "import";
                break;
              case tok::pp_include_next:
                return CB(error("cannot resolve conditional #include_next "
                                "in {0}",
                                File));
              default:
                llvm_unreachable("unexpected conditional inclusion kind");
              }
              std::string ProbeCode =
                  ("#" + Keyword + " " + Inclusion.Written + "\n").str();
              ParseInputs ProbeInputs{ProbeCommand, &SnapshotFS,
                                      std::move(ProbeCode)};
              ProbeInputs.Index = Index;
              ProbeInputs.FeatureModules = FeatureModules;
              ProbeInputs.ModulesManager = ModulesManager;
              ProbeInputs.Opts.ImportInsertions = ImportInsertions;
              adjustParseInputs(ProbeInputs, File);
              StoreDiags ProbeDiags;
              auto ProbeCI = buildCompilerInvocation(ProbeInputs, ProbeDiags);
              if (!ProbeCI)
                return CB(error("cannot build conditional include probe for "
                                "{0}",
                                File));
              auto ProbeAST = ParsedAST::build(
                  File, ProbeInputs, std::move(ProbeCI), ProbeDiags.take(),
                  /*Preamble=*/nullptr);
              if (!ProbeAST)
                return CB(error("cannot parse conditional include probe for "
                                "{0}",
                                File));
              const auto &Resolved =
                  ProbeAST->getIncludeStructure().MainFileIncludes;
              if (Resolved.size() != 1 ||
                  Resolved.front().Written != Inclusion.Written ||
                  Resolved.front().Resolved.empty())
                return CB(error("cannot exhaustively resolve conditional "
                                "include {0} in {1}",
                                Inclusion.Written, File));
              if (llvm::any_of(Includes.MainFileIncludes,
                               [&](const clangd::Inclusion &Existing) {
                                 return Existing.HashOffset ==
                                        Inclusion.HashOffset;
                               }))
                continue;
              clangd::Inclusion Synthesized = Resolved.front();
              Synthesized.HashOffset = Inclusion.HashOffset;
              Synthesized.HashLine = Inclusion.HashLine;
              Includes.MainFileIncludes.push_back(std::move(Synthesized));
            }

            auto Style =
                getFormatStyleForFile(File, Inputs.Contents, SnapshotFS, false);
            auto Edits = renameIncludeDirectives(
                File, Inputs.Contents, Includes,
                AST->getPreprocessor().getHeaderSearchInfo(),
                AST->getSourceManager(), AST->getLangOpts(),
                Inputs.CompileCommand.Directory, *Mappings, Style, *FS);
            if (!Edits)
              return CB(Edits.takeError());
            auto [It, Inserted] = ProposedEdits.try_emplace(File, *Edits);
            if (!Inserted)
              if (auto Err = validateCompatibleFileRenameEdits(File, It->second,
                                                               *Edits))
                return CB(std::move(Err));
          }
        }

        // Closed documents can change independently of DraftStore. Re-read
        // only the affected closed files and reject a mixed snapshot.
        for (PathRef File : ClosedCandidates.keys()) {
          auto Buffer = FS->getBufferForFile(File);
          if (!Buffer)
            return CB(error("cannot revalidate includer {0}: {1}", File,
                            Buffer.getError().message()));
          if (Buffer.get()->getBuffer() != Contents.lookup(File))
            return CB(llvm::make_error<LSPError>(
                "file contents changed while preparing file rename",
                ErrorCode::ContentModified));
        }
        if (!draftsEqual(Drafts, DraftMgr.getDrafts()))
          return CB(llvm::make_error<LSPError>(
              "open documents changed while preparing file rename",
              ErrorCode::ContentModified));
        FileRenameDirectiveCache ExternalRecheck(*FS);
        for (const auto &Entry : ExternalDigests) {
          auto Scan = ExternalRecheck.scan(Entry.first(), Entry.getValue());
          if (!Scan) {
            llvm::consumeError(Scan.takeError());
            return CB(llvm::make_error<LSPError>(
                "external dependency changed while preparing file rename",
                ErrorCode::ContentModified));
          }
        }
        auto CurrentWorkspace = FileRenameWorkspace->snapshot(*FS);
        if (!CurrentWorkspace)
          return CB(CurrentWorkspace.takeError());
        if (!(*CurrentWorkspace == *Workspace))
          return CB(llvm::make_error<LSPError>(
              "workspace inventory changed while preparing file rename",
              ErrorCode::ContentModified));
        auto CurrentGraph = BackgroundIdx->includeGraphSnapshot();
        if (!CurrentGraph || CurrentGraph->Generation != Graph->Generation) {
          if (!CurrentGraph)
            llvm::consumeError(CurrentGraph.takeError());
          return CB(llvm::make_error<LSPError>(
              "background include graph changed while preparing file rename",
              ErrorCode::ContentModified));
        }

        // External processes can still mutate files after this final
        // validation and before the client applies the edit. Portable
        // filesystems offer no transaction spanning that protocol boundary.
        WorkspaceEdit Result;
        Result.documentChanges.emplace();
        for (auto &Entry : ProposedEdits) {
          if (Entry.getValue().empty())
            continue;
          const DraftStore::Draft *Draft = findDraft(Drafts, Entry.first());
          auto Version = draftVersion(Draft, Entry.first());
          if (!Version)
            return CB(Version.takeError());
          TextDocumentEdit Edit;
          Edit.textDocument = VersionedTextDocumentIdentifier{
              {URIForFile::canonicalize(Entry.first(), Entry.first())},
              *Version};
          Edit.edits = std::move(Entry.getValue());
          Result.documentChanges->push_back(std::move(Edit));
        }
        DiscardCDBPlan.release();
        return CB(std::move(Result));
      });
}

void ClangdServer::didRenameFiles(
    llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  std::vector<std::pair<Path, Path>> EffectiveRenames =
      effectiveRenames(Renames);
  if (EffectiveRenames.empty())
    return;
  Renames = EffectiveRenames;
  if (FileRenameWorkspace)
    for (const auto &[OldPath, NewPath] : Renames) {
      FileRenameWorkspace->invalidate(OldPath);
      FileRenameWorkspace->invalidate(NewPath);
    }
  struct MovedDraft {
    Path OldPath;
    Path NewPath;
    DraftStore::Draft Draft;
  };
  std::vector<MovedDraft> MovedDrafts;
  for (const Path &OpenFile : DraftMgr.getActiveFiles()) {
    auto NewPath = mapPathAfterRenames(OpenFile, Renames);
    if (!NewPath) {
      elog("Failed to map open file after rename: {0}", NewPath.takeError());
      return;
    }
    if (pathEqual(OpenFile, *NewPath))
      continue;
    if (DraftMgr.getDraft(*NewPath)) {
      elog("Cannot migrate open file {0}: destination {1} is already open",
           OpenFile, *NewPath);
      return;
    }
    auto Draft = DraftMgr.getDraft(OpenFile);
    assert(Draft && "active draft disappeared");
    MovedDrafts.push_back({OpenFile, std::move(*NewPath), std::move(*Draft)});
  }
  if (auto Err = CDB.filesRenamed(Renames)) {
    elog("Failed to migrate compilation commands after file rename: {0}",
         std::move(Err));
    return;
  }
  for (const MovedDraft &Moved : MovedDrafts)
    removeDocument(Moved.OldPath);
  for (const MovedDraft &Moved : MovedDrafts)
    addDocument(Moved.NewPath, *Moved.Draft.Contents, Moved.Draft.Version,
                WantDiagnostics::Auto);
  reparseOpenFilesIfNeeded([](llvm::StringRef) { return true; });
  if (!BackgroundIdx)
    return;
  if (auto Err = BackgroundIdx->filesRenamed(Renames))
    elog("Failed to migrate background index after file rename: {0}",
         std::move(Err));
}

} // namespace clangd
} // namespace clang
