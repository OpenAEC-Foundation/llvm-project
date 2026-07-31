//===-- Background.cpp - Build an index in a background thread ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "index/Background.h"
#include "Compiler.h"
#include "Config.h"
#include "FS.h"
#include "FileRename.h"
#include "Headers.h"
#include "SourceCode.h"
#include "URI.h"
#include "index/BackgroundIndexLoader.h"
#include "index/FileIndex.h"
#include "index/Index.h"
#include "index/IndexAction.h"
#include "index/MemIndex.h"
#include "index/Ref.h"
#include "index/Relation.h"
#include "index/Serialization.h"
#include "index/Symbol.h"
#include "index/SymbolCollector.h"
#include "support/Context.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "support/Threading.h"
#include "support/ThreadsafeFS.h"
#include "support/Trace.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Stack.h"
#include "clang/Frontend/FrontendAction.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

// We cannot use vfs->makeAbsolute because Cmd.FileName is either absolute or
// relative to Cmd.Directory, which might not be the same as current working
// directory.
llvm::SmallString<128> getAbsolutePath(const tooling::CompileCommand &Cmd) {
  llvm::SmallString<128> AbsolutePath;
  if (llvm::sys::path::is_absolute(Cmd.Filename)) {
    AbsolutePath = Cmd.Filename;
  } else {
    AbsolutePath = Cmd.Directory;
    llvm::sys::path::append(AbsolutePath, Cmd.Filename);
    llvm::sys::path::remove_dots(AbsolutePath, true);
  }
  return AbsolutePath;
}

bool shardIsStale(const LoadedShard &LS, llvm::vfs::FileSystem *FS) {
  auto Buf = FS->getBufferForFile(LS.AbsolutePath);
  if (!Buf) {
    vlog("Background-index: Couldn't read {0} to validate stored index: {1}",
         LS.AbsolutePath, Buf.getError().message());
    // There is no point in indexing an unreadable file.
    return false;
  }
  return digest(Buf->get()->getBuffer()) != LS.Digest;
}

llvm::Expected<BackgroundIndex::IndexedFile>
indexedFile(PathRef File, PathRef DependentTU, const IncludeGraphNode &Source,
            PathRef HintPath) {
  BackgroundIndex::IndexedFile Result;
  Result.File = File.str();
  Result.DependentTU = DependentTU.str();
  Result.Digest = Source.Digest;
  Result.Flags = Source.Flags;
  for (llvm::StringRef IncludedURI : Source.DirectIncludes) {
    auto Included = URI::resolve(IncludedURI, HintPath);
    if (!Included)
      return error("cannot resolve include URI {0}: {1}", IncludedURI,
                   Included.takeError());
    Result.DirectIncludes.push_back(std::move(*Included));
  }
  return Result;
}

IncludeGraph cloneIncludeGraph(const IncludeGraph &Graph) {
  IncludeGraph Result;
  for (const auto &Source : Graph)
    Result.try_emplace(Source.first(), Source.getValue());
  for (auto &Source : Result) {
    Source.getValue().URI = Source.getKey();
    for (llvm::StringRef &Include : Source.getValue().DirectIncludes) {
      auto Target = Result.find(Include);
      assert(Target != Result.end() && "include graph edge has no node");
      Include = Target->getKey();
    }
  }
  return Result;
}

} // namespace

BackgroundIndex::BackgroundIndex(
    const ThreadsafeFS &TFS, const GlobalCompilationDatabase &CDB,
    BackgroundIndexStorage::Factory IndexStorageFactory, Options Opts)
    : SwapIndex(std::make_unique<MemIndex>()), TFS(TFS), CDB(CDB),
      IndexingPriority(Opts.IndexingPriority),
      ContextProvider(std::move(Opts.ContextProvider)),
      IndexedSymbols(IndexContents::All, Opts.SupportContainedRefs),
      Rebuilder(this, &IndexedSymbols, Opts.ThreadPoolSize),
      IndexStorageFactory(std::move(IndexStorageFactory)),
      Queue(std::move(Opts.OnProgress)),
      CommandsChanged(
          CDB.watch([&](const std::vector<std::string> &ChangedFiles) {
            enqueue(ChangedFiles);
          })) {
  assert(Opts.ThreadPoolSize > 0 && "Thread pool size can't be zero.");
  assert(this->IndexStorageFactory && "Storage factory can not be null!");
  for (unsigned I = 0; I < Opts.ThreadPoolSize; ++I) {
    ThreadPool.runAsync("background-worker-" + llvm::Twine(I + 1),
                        [this, Ctx(Context::current().clone())]() mutable {
                          clang::noteBottomOfStack();
                          WithContext BGContext(std::move(Ctx));
                          Queue.work([&] { Rebuilder.idle(); });
                        });
  }
}

BackgroundIndex::~BackgroundIndex() {
  stop();
  ThreadPool.wait();
}

void BackgroundIndex::enqueue(const std::vector<std::string> &ChangedFiles) {
  uint64_t ScheduledRenameEpoch;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    ScheduledRenameEpoch = RenameEpoch;
    ++GraphGeneration;
    for (PathRef File : ChangedFiles) {
      Path Normalized = removeDots(File);
      KnownTUs.insert(Normalized);
      FreshlyIndexedTUs.erase(Normalized);
    }
  }
  Queue.push(changedFilesTask(ChangedFiles, ScheduledRenameEpoch));
}

llvm::Expected<BackgroundIndex::IncludeGraphSnapshot>
BackgroundIndex::includeGraphSnapshot() const {
  std::lock_guard<std::mutex> Lock(ShardVersionsMu);
  if (!IncludeGraphErrors.empty())
    return error("background include graph is incomplete for {0}: {1}",
                 IncludeGraphErrors.front().File,
                 IncludeGraphErrors.front().Message);
  if (!IndexFailures.empty())
    return error("background indexing failed for {0}: {1}",
                 IndexFailures.begin()->first(),
                 IndexFailures.begin()->getValue());
  for (llvm::StringRef TU : KnownTUs.keys()) {
    if (!FreshlyIndexedTUs.contains(TU))
      return error("background include graph has no translation unit {0}", TU);
  }
  IncludeGraphSnapshot Result;
  Result.Generation = GraphGeneration;
  Result.Files = IndexedFiles;
  Result.Commands = IndexedCommands;
  Result.CC1Commands = IndexedCC1Commands;
  Result.TranslationUnits.reserve(KnownTUs.size());
  for (llvm::StringRef TU : KnownTUs.keys())
    Result.TranslationUnits.push_back(TU.str());
  return Result;
}

void BackgroundIndex::ensureIncludeGraph() {
  std::vector<std::pair<Path, uint64_t>> Scheduled;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    for (PathRef TU : KnownTUs.keys()) {
      if (FreshlyIndexedTUs.contains(TU))
        continue;
      auto Pending = PendingIncludeGraphTUs.try_emplace(TU, RenameEpoch);
      if (Pending.second)
        Scheduled.emplace_back(TU.str(), RenameEpoch);
    }
    if (!Scheduled.empty())
      ++GraphGeneration;
  }
  std::vector<BackgroundQueue::Task> Tasks;
  Tasks.reserve(Scheduled.size());
  for (auto &[TU, Epoch] : Scheduled)
    Tasks.push_back(indexFileTask(std::move(TU),
                                  /*BypassDuplicateSuppression=*/true, Epoch));
  Queue.append(std::move(Tasks));
}

BackgroundQueue::Task
BackgroundIndex::changedFilesTask(const std::vector<std::string> &ChangedFiles,
                                  uint64_t RequiredRenameEpoch) {
  BackgroundQueue::Task T([this, ChangedFiles, RequiredRenameEpoch] {
    trace::Span Tracer("BackgroundIndexEnqueue");

    {
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      if (RequiredRenameEpoch != RenameEpoch)
        return;
    }

    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(/*Path=*/""));

    // We're doing this asynchronously, because we'll read shards here too.
    log("Enqueueing {0} commands for indexing", ChangedFiles.size());
    SPAN_ATTACH(Tracer, "files", int64_t(ChangedFiles.size()));

    auto NeedsReIndexing =
        loadProject(std::move(ChangedFiles), RequiredRenameEpoch);
    // Run indexing for files that need to be updated.
    std::shuffle(NeedsReIndexing.begin(), NeedsReIndexing.end(),
                 std::mt19937(std::random_device{}()));
    std::vector<BackgroundQueue::Task> Tasks;
    Tasks.reserve(NeedsReIndexing.size());
    for (const auto &File : NeedsReIndexing)
      Tasks.push_back(indexFileTask(std::move(File),
                                    /*BypassDuplicateSuppression=*/false,
                                    RequiredRenameEpoch));
    Queue.append(std::move(Tasks));
  });

  T.QueuePri = LoadShards;
  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

static llvm::StringRef filenameWithoutExtension(llvm::StringRef Path) {
  Path = llvm::sys::path::filename(Path);
  return Path.drop_back(llvm::sys::path::extension(Path).size());
}

BackgroundQueue::Task
BackgroundIndex::indexFileTask(std::string Path,
                               bool BypassDuplicateSuppression,
                               std::optional<uint64_t> RequiredRenameEpoch) {
  std::string Tag = filenameWithoutExtension(Path).str();
  uint64_t Key = BypassDuplicateSuppression ? 0 : llvm::xxh3_64bits(Path);
  BackgroundQueue::Task T([this, Path(std::move(Path)), RequiredRenameEpoch] {
    llvm::scope_exit ClearPending([&] {
      if (!RequiredRenameEpoch)
        return;
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      auto Pending = PendingIncludeGraphTUs.find(Path);
      if (Pending != PendingIncludeGraphTUs.end() &&
          Pending->getValue() == *RequiredRenameEpoch)
        PendingIncludeGraphTUs.erase(Pending);
    });
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(Path));
    uint64_t Generation;
    {
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      if (RequiredRenameEpoch && *RequiredRenameEpoch != RenameEpoch)
        return;
      Generation = RequiredRenameEpoch.value_or(RenameEpoch);
    }
    if (Config::current().Index.Background == Config::BackgroundPolicy::Skip) {
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      if (Generation == RenameEpoch) {
        IndexFailures[Path] =
            "background indexing is disabled by configuration";
        ++GraphGeneration;
      }
      return;
    }
    auto Cmd = CDB.getCompileCommand(Path);
    if (!Cmd) {
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      if (Generation == RenameEpoch) {
        IndexFailures[Path] = "no compilation command is available";
        ++GraphGeneration;
      }
      return;
    }
    if (auto Error = index(std::move(*Cmd), Generation)) {
      std::string Message = llvm::toString(std::move(Error));
      {
        std::lock_guard<std::mutex> Lock(ShardVersionsMu);
        if (Generation == RenameEpoch) {
          IndexFailures[Path] = Message;
          ++GraphGeneration;
        }
      }
      elog("Indexing {0} failed: {1}", Path, Message);
      return;
    }
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    if (Generation == RenameEpoch && IndexFailures.erase(Path))
      ++GraphGeneration;
  });
  T.QueuePri = IndexFile;
  T.ThreadPri = IndexingPriority;
  T.Tag = std::move(Tag);
  T.Key = Key;
  return T;
}

void BackgroundIndex::boostRelated(llvm::StringRef Path) {
  if (isHeaderFile(Path))
    Queue.boost(filenameWithoutExtension(Path), IndexBoostedFile);
}

/// Given index results from a TU, only update symbols coming from files that
/// are different or missing from than \p ShardVersionsSnapshot. Also stores new
/// index information on IndexStorage.
void BackgroundIndex::update(
    llvm::StringRef MainFile, IndexFileIn Index,
    const llvm::StringMap<ShardVersion> &ShardVersionsSnapshot, bool HadErrors,
    uint64_t Generation) {
  // Keys are URIs.
  llvm::StringMap<std::pair<Path, FileDigest>> FilesToUpdate;
  std::vector<IndexedFile> Contexts;
  llvm::StringMap<std::string> GraphErrors;
  assert(Index.Cmd && "background index result has no compile command");
  assert(Index.CC1CommandLine &&
         "background index result has no driver-derived command");
  tooling::CompileCommand Command = *Index.Cmd;
  std::vector<std::string> CC1Command = *Index.CC1CommandLine;
  IncludeGraph ContextSources = cloneIncludeGraph(*Index.Sources);
  auto FS = TFS.view(Command.Directory);
  // Note that sources do not contain any information regarding missing headers,
  // since we don't even know what absolute path they should fall in.
  for (const auto &IndexIt : *Index.Sources) {
    const auto &IGN = IndexIt.getValue();
    auto AbsPath = URI::resolve(IGN.URI, MainFile);
    if (!AbsPath) {
      GraphErrors[MainFile] = llvm::toString(AbsPath.takeError());
      continue;
    }
    auto Conditional = conditionalIncludeDirectives(*AbsPath, *FS);
    if (!Conditional) {
      GraphErrors[*AbsPath] = llvm::toString(Conditional.takeError());
    } else {
      auto ContextSource = ContextSources.find(IGN.URI);
      assert(ContextSource != ContextSources.end());
      if (!Conditional->empty())
        ContextSource->getValue().Flags |=
            IncludeGraphNode::SourceFlag::HasConditionalIncludes;
      auto File = indexedFile(*AbsPath, MainFile, ContextSource->getValue(),
                              MainFile);
      if (!File)
        GraphErrors[*AbsPath] = llvm::toString(File.takeError());
      else
        Contexts.push_back(std::move(*File));
    }
    const auto DigestIt = ShardVersionsSnapshot.find(*AbsPath);
    // File has different contents, or indexing was successful this time.
    if (DigestIt == ShardVersionsSnapshot.end() ||
        DigestIt->getValue().Digest != IGN.Digest ||
        (DigestIt->getValue().HadErrors && !HadErrors))
      FilesToUpdate[IGN.URI] = {std::move(*AbsPath), IGN.Digest};
  }

  auto MainURI = URI::create(MainFile).toString();
  auto MainSource = Index.Sources->find(MainURI);
  assert(MainSource != Index.Sources->end() &&
         "background index graph has no main-file source");
  FilesToUpdate[MainURI] = {MainFile.str(), MainSource->getValue().Digest};

  // Shard slabs into files.
  FileShardedIndex ShardedIndex(std::move(Index));

  std::lock_guard<std::mutex> RenameLock(RenameMu);
  std::lock_guard<std::mutex> Lock(ShardVersionsMu);
  if (Generation != RenameEpoch)
    return;
  llvm::erase_if(IndexedFiles, [&](const IndexedFile &File) {
    return File.DependentTU == MainFile;
  });
  IndexedCC1Commands.erase(MainFile);
  IndexedFiles.insert(IndexedFiles.end(),
                      std::make_move_iterator(Contexts.begin()),
                      std::make_move_iterator(Contexts.end()));
  IndexedCommands[MainFile] = std::move(Command);
  FreshlyIndexedTUs.insert(MainFile);
  llvm::erase_if(IncludeGraphErrors, [&](const IncludeGraphError &GraphError) {
    return GraphError.DependentTU == MainFile;
  });
  for (const auto &Entry : GraphErrors)
    IncludeGraphErrors.push_back(
        {Entry.first().str(), MainFile.str(), Entry.getValue()});

  // Build and store new slabs for each updated file.
  for (const auto &FileIt : FilesToUpdate) {
    auto Uri = FileIt.first();
    auto IF = ShardedIndex.getShard(Uri);
    assert(IF && "no shard for file in Index.Sources?");
    PathRef Path = FileIt.getValue().first;

    // Only store command line hash for main files of the TU, since our
    // current model keeps only one version of a header file.
    if (Path != MainFile)
      IF->Cmd.reset();
    if (Path != MainFile)
      IF->CC1CommandLine.reset();
    else if (GraphErrors.empty()) {
      IF->ContextSources = std::move(ContextSources);
      IndexedCC1Commands[MainFile] = std::move(CC1Command);
    }

    // We need to store shards before updating the index, since the latter
    // consumes slabs.
    // FIXME: Also skip serializing the shard if it is already up-to-date.
    if (auto Error = IndexStorageFactory(Path)->storeShard(Path, *IF))
      elog("Failed to write background-index shard for file {0}: {1}", Path,
           std::move(Error));

    const auto &Hash = FileIt.getValue().second;
    auto DigestIt = ShardVersions.try_emplace(Path);
    ShardVersion &SV = DigestIt.first->second;
    // Skip if file is already up to date, unless previous index was broken
    // and this one is not.
    if (!DigestIt.second && SV.Digest == Hash && SV.HadErrors && !HadErrors)
      continue;
    SV.Digest = Hash;
    SV.HadErrors = HadErrors;
    IndexedSymbols.update(
        Uri, std::make_unique<SymbolSlab>(std::move(*IF->Symbols)),
        std::make_unique<RefSlab>(std::move(*IF->Refs)),
        std::make_unique<RelationSlab>(std::move(*IF->Relations)),
        Path == MainFile);
  }
  ++GraphGeneration;
}

llvm::Error BackgroundIndex::index(tooling::CompileCommand Cmd,
                                   uint64_t Generation) {
  trace::Span Tracer("BackgroundIndex");
  SPAN_ATTACH(Tracer, "file", Cmd.Filename);
  auto AbsolutePath = getAbsolutePath(Cmd);

  auto FS = TFS.view(Cmd.Directory);
  auto Buf = FS->getBufferForFile(AbsolutePath);
  if (!Buf)
    return llvm::errorCodeToError(Buf.getError());
  auto Hash = digest(Buf->get()->getBuffer());

  // Take a snapshot of the versions to avoid locking for each file in the TU.
  llvm::StringMap<ShardVersion> ShardVersionsSnapshot;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    ShardVersionsSnapshot = ShardVersions;
  }

  vlog("Indexing {0} (digest:={1})", Cmd.Filename, llvm::toHex(Hash));
  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(Cmd);
  IgnoreDiagnostics IgnoreDiags;
  std::vector<std::string> CC1Args;
  bool HadConfigFile = false;
  auto CI =
      buildCompilerInvocation(Inputs, IgnoreDiags, &CC1Args, &HadConfigFile);
  if (!CI)
    return error("Couldn't build compiler invocation");

  auto Clang =
      prepareCompilerInstance(std::move(CI), /*Preamble=*/nullptr,
                              std::move(*Buf), std::move(FS), IgnoreDiags);
  if (!Clang)
    return error("Couldn't build compiler instance");

  SymbolCollector::Options IndexOpts;
  // Creates a filter to not collect index results from files with unchanged
  // digests.
  IndexOpts.FileFilter = [&ShardVersionsSnapshot,
                          &AbsolutePath](const SourceManager &SM, FileID FID) {
    const auto F = SM.getFileEntryRefForID(FID);
    if (!F)
      return false; // Skip invalid files.
    auto AbsPath = getCanonicalPath(*F, SM.getFileManager());
    if (!AbsPath)
      return false; // Skip files without absolute path.
    // The main shard also persists the exact TU context and is rewritten on
    // every successful index. Keep its declarations in that replacement even
    // when the source digest is unchanged.
    if (pathEqual(*AbsPath, AbsolutePath))
      return true;
    auto Digest = digestFile(SM, FID);
    if (!Digest)
      return false;
    auto D = ShardVersionsSnapshot.find(*AbsPath);
    if (D != ShardVersionsSnapshot.end() && D->second.Digest == Digest &&
        !D->second.HadErrors)
      return false; // Skip files that haven't changed, without errors.
    return true;
  };
  IndexOpts.CollectMainFileRefs = true;

  IndexFileIn Index;
  auto Action = createStaticIndexingAction(
      IndexOpts, [&](IndexFileIn Result) { Index = std::move(Result); });

  // We're going to run clang here, and it could potentially crash.
  // We could use CrashRecoveryContext to try to make indexing crashes nonfatal,
  // but the leaky "recovery" is pretty scary too in a long-running process.
  // If crashes are a real problem, maybe we should fork a child process.

  const FrontendInputFile &Input = Clang->getFrontendOpts().Inputs.front();
  if (!Action->BeginSourceFile(*Clang, Input))
    return error("BeginSourceFile() failed");
  if (llvm::Error Err = Action->Execute())
    return Err;

  Action->EndSourceFile();

  Index.CC1CommandLine = std::move(CC1Args);
  Inputs.CompileCommand.HadConfigFile = HadConfigFile;
  Index.Cmd = Inputs.CompileCommand;
  assert(Index.Symbols && Index.Refs && Index.Sources &&
         "Symbols, Refs and Sources must be set.");

  log("Indexed {0} ({1} symbols, {2} refs, {3} files)",
      Inputs.CompileCommand.Filename, Index.Symbols->size(),
      Index.Refs->numRefs(), Index.Sources->size());
  SPAN_ATTACH(Tracer, "symbols", int(Index.Symbols->size()));
  SPAN_ATTACH(Tracer, "refs", int(Index.Refs->numRefs()));
  SPAN_ATTACH(Tracer, "sources", int(Index.Sources->size()));

  bool HadErrors = Clang->hasDiagnostics() &&
                   Clang->getDiagnostics().hasUncompilableErrorOccurred();
  if (HadErrors) {
    log("Failed to compile {0}, index may be incomplete", AbsolutePath);
    for (auto &It : *Index.Sources)
      It.second.Flags |= IncludeGraphNode::SourceFlag::HadErrors;
  }
  update(AbsolutePath, std::move(Index), ShardVersionsSnapshot, HadErrors,
         Generation);

  Rebuilder.indexedTU();
  return llvm::Error::success();
}

// Restores shards for \p MainFiles from index storage. Then checks staleness of
// those shards and returns a list of TUs that needs to be indexed to update
// staleness.
std::vector<std::string>
BackgroundIndex::loadProject(std::vector<std::string> MainFiles,
                             uint64_t RequiredRenameEpoch) {
  // Drop files where background indexing is disabled in config.
  if (ContextProvider)
    llvm::erase_if(MainFiles, [&](const std::string &TU) {
      // Load the config for each TU, as indexing may be selectively enabled.
      WithContext WithProvidedContext(ContextProvider(TU));
      return Config::current().Index.Background ==
             Config::BackgroundPolicy::Skip;
    });
  Rebuilder.startLoading();
  // Load shards for all of the mainfiles.
  const std::vector<LoadedShard> Result =
      loadIndexShards(MainFiles, IndexStorageFactory, CDB);
  size_t LoadedShards = 0;
  llvm::StringSet<> LoadedTranslationUnits;
  llvm::StringSet<> LoadedExactTranslationUnits;
  auto FS = TFS.view(/*CWD=*/std::nullopt);
  {
    // Update in-memory state.
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    if (RequiredRenameEpoch != RenameEpoch) {
      Rebuilder.loadedShard(0);
      Rebuilder.doneLoading();
      return {};
    }
    for (auto &LS : Result) {
      if (!LS.Shard)
        continue;
      auto SS =
          LS.Shard->Symbols
              ? std::make_unique<SymbolSlab>(std::move(*LS.Shard->Symbols))
              : nullptr;
      auto RS = LS.Shard->Refs
                    ? std::make_unique<RefSlab>(std::move(*LS.Shard->Refs))
                    : nullptr;
      auto RelS =
          LS.Shard->Relations
              ? std::make_unique<RelationSlab>(std::move(*LS.Shard->Relations))
              : nullptr;
      ShardVersion &SV = ShardVersions[LS.AbsolutePath];
      SV.Digest = LS.Digest;
      SV.HadErrors = LS.HadErrors;
      if (LS.CountReferences && LS.AbsolutePath == LS.DependentTU)
        LoadedTranslationUnits.insert(LS.AbsolutePath);
      ++LoadedShards;

      if (LS.CountReferences && LS.AbsolutePath == LS.DependentTU) {
        llvm::erase_if(IndexedFiles, [&](const IndexedFile &File) {
          return File.DependentTU == LS.AbsolutePath;
        });
        IndexedCommands.erase(LS.AbsolutePath);
        IndexedCC1Commands.erase(LS.AbsolutePath);
        FreshlyIndexedTUs.erase(LS.AbsolutePath);
      }
      if (LS.CountReferences && LS.AbsolutePath == LS.DependentTU &&
          LS.Shard->ContextSources && LS.Shard->Cmd &&
          LS.Shard->CC1CommandLine) {
        tooling::CompileCommand Command = *LS.Shard->Cmd;
        Command.Filename = LS.AbsolutePath;
        std::vector<IndexedFile> Contexts;
        bool Complete = true;
        const std::string MainURI = URI::create(LS.AbsolutePath).toString();
        if (auto Err = validateContextIncludeGraph(*LS.Shard->ContextSources,
                                                   MainURI, &LS.Digest)) {
          vlog("Rejecting malformed context cache for {0}: {1}",
               LS.AbsolutePath, std::move(Err));
          Complete = false;
        }
        auto MainSource = LS.Shard->Sources ? LS.Shard->Sources->find(MainURI)
                                            : IncludeGraph::const_iterator();
        if (!LS.Shard->Sources || MainSource == LS.Shard->Sources->end() ||
            MainSource->getValue().Digest != LS.Digest ||
            !(MainSource->getValue().Flags &
              IncludeGraphNode::SourceFlag::IsTU) ||
            Command.Directory.empty() || Command.CommandLine.empty() ||
            LS.Shard->CC1CommandLine->empty() ||
            LS.Shard->CC1CommandLine->front() != "-cc1")
          Complete = false;
        llvm::StringSet<> CanonicalContextPaths;
        for (const auto &Source : *LS.Shard->ContextSources) {
          auto AbsPath = URI::resolve(Source.first(), LS.AbsolutePath);
          if (!AbsPath) {
            Complete = false;
            break;
          }
          Path Canonical = removeDots(*AbsPath);
          llvm::SmallString<256> Real;
          if (!FS->getRealPath(Canonical, Real))
            Canonical = Real.str().str();
          if (!CanonicalContextPaths.insert(maybeCaseFoldPath(Canonical))
                   .second) {
            Complete = false;
            break;
          }
          auto Context = indexedFile(*AbsPath, LS.AbsolutePath,
                                     Source.getValue(), LS.AbsolutePath);
          if (!Context) {
            Complete = false;
            break;
          }
          Contexts.push_back(std::move(*Context));
        }
        if (Complete && llvm::any_of(Contexts, [&](const IndexedFile &File) {
              return File.File == LS.AbsolutePath;
            })) {
          IndexedFiles.insert(IndexedFiles.end(),
                              std::make_move_iterator(Contexts.begin()),
                              std::make_move_iterator(Contexts.end()));
          IndexedCommands[LS.AbsolutePath] = std::move(Command);
          IndexedCC1Commands[LS.AbsolutePath] =
              std::move(*LS.Shard->CC1CommandLine);
          FreshlyIndexedTUs.insert(LS.AbsolutePath);
          LoadedExactTranslationUnits.insert(LS.AbsolutePath);
        }
      }

      if (LS.Shard->Sources) {
        std::string FileURI = URI::create(LS.AbsolutePath).toString();
        auto Source = LS.Shard->Sources->find(FileURI);
        if (Source == LS.Shard->Sources->end())
          IncludeGraphErrors.push_back(
              {LS.AbsolutePath, LS.DependentTU,
               llvm::formatv("missing loaded source node for {0}",
                             LS.AbsolutePath)
                   .str()});
      }

      IndexedSymbols.update(URI::create(LS.AbsolutePath).toString(),
                            std::move(SS), std::move(RS), std::move(RelS),
                            LS.CountReferences);
    }
  }
  Rebuilder.loadedShard(LoadedShards);
  Rebuilder.doneLoading();

  llvm::DenseSet<PathRef> TUsToIndex;
  // A missing or malformed main-file shard cannot be accepted as a cache hit.
  // Index it so an unreadable TU becomes an explicit graph failure and a
  // readable TU can repair its shard.
  for (PathRef TU : MainFiles)
    if (!LoadedTranslationUnits.contains(TU) ||
        !LoadedExactTranslationUnits.contains(TU))
      TUsToIndex.insert(TU);
  // We'll accept data from stale shards, but ensure the files get reindexed
  // soon.
  for (auto &LS : Result) {
    if (!shardIsStale(LS, FS.get()))
      continue;
    PathRef TUForFile = LS.DependentTU;
    assert(!TUForFile.empty() && "File without a TU!");

    // FIXME: Currently, we simply schedule indexing on a TU whenever any of
    // its dependencies needs re-indexing. We might do it smarter by figuring
    // out a minimal set of TUs that will cover all the stale dependencies.
    // FIXME: Try looking at other TUs if no compile commands are available
    // for this TU, i.e TU was deleted after we performed indexing.
    TUsToIndex.insert(TUForFile);
  }

  return {TUsToIndex.begin(), TUsToIndex.end()};
}

void BackgroundIndex::profile(MemoryTree &MT) const {
  IndexedSymbols.profile(MT.child("slabs"));
  // We don't want to mix memory used by index and symbols, so call base class.
  MT.child("index").addUsage(SwapIndex::estimateMemoryUsage());
}
} // namespace clangd
} // namespace clang
