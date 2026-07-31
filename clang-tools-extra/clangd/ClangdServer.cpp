//===--- ClangdServer.cpp - Main clangd server code --------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===-------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "CodeComplete.h"
#include "Config.h"
#include "Diagnostics.h"
#include "DumpAST.h"
#include "FileRename.h"
#include "FindSymbols.h"
#include "Format.h"
#include "HeaderSourceSwitch.h"
#include "InlayHints.h"
#include "ParsedAST.h"
#include "Preamble.h"
#include "Protocol.h"
#include "SemanticHighlighting.h"
#include "SemanticSelection.h"
#include "SourceCode.h"
#include "TUScheduler.h"
#include "URI.h"
#include "XRefs.h"
#include "clang-include-cleaner/Record.h"
#include "index/FileIndex.h"
#include "index/Merge.h"
#include "index/StdLib.h"
#include "refactor/Rename.h"
#include "refactor/Tweak.h"
#include "support/Cancellation.h"
#include "support/Context.h"
#include "support/Logger.h"
#include "support/MemoryTree.h"
#include "support/ThreadsafeFS.h"
#include "support/Trace.h"
#include "clang/Basic/Stack.h"
#include "clang/Format/Format.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

// Tracks number of times a tweak has been offered.
static constexpr trace::Metric TweakAvailable(
    "tweak_available", trace::Metric::Counter, "tweak_id");

// Update the FileIndex with new ASTs and plumb the diagnostics responses.
struct UpdateIndexCallbacks : public ParsingCallbacks {
  UpdateIndexCallbacks(FileIndex *FIndex,
                       ClangdServer::Callbacks *ServerCallbacks,
                       const ThreadsafeFS &TFS, AsyncTaskRunner *Tasks,
                       bool CollectInactiveRegions)
      : FIndex(FIndex), ServerCallbacks(ServerCallbacks), TFS(TFS),
        Stdlib{std::make_shared<StdLibSet>()}, Tasks(Tasks),
        CollectInactiveRegions(CollectInactiveRegions) {}

  void onPreambleAST(
      PathRef Path, llvm::StringRef Version, CapturedASTCtx ASTCtx,
      std::shared_ptr<const include_cleaner::PragmaIncludes> PI) override {

    if (!FIndex)
      return;

    auto &PP = ASTCtx.getPreprocessor();
    auto &CI = ASTCtx.getCompilerInvocation();
    if (auto Loc = Stdlib->add(CI.getLangOpts(), PP.getHeaderSearchInfo()))
      indexStdlib(CI, std::move(*Loc));

    // FIndex outlives the UpdateIndexCallbacks.
    auto Task = [FIndex(FIndex), Path(Path.str()), Version(Version.str()),
                 ASTCtx(std::move(ASTCtx)), PI(std::move(PI))]() mutable {
      trace::Span Tracer("PreambleIndexing");
      FIndex->updatePreamble(Path, Version, ASTCtx.getASTContext(),
                             ASTCtx.getPreprocessor(), *PI);
    };

    if (Tasks) {
      Tasks->runAsync("Preamble indexing for:" + Path + Version,
                      std::move(Task));
    } else
      Task();
  }

  void indexStdlib(const CompilerInvocation &CI, StdLibLocation Loc) {
    // This task is owned by Tasks, which outlives the TUScheduler and
    // therefore the UpdateIndexCallbacks.
    // We must be careful that the references we capture outlive TUScheduler.
    auto Task = [LO(CI.getLangOpts()), Loc(std::move(Loc)),
                 CI(std::make_unique<CompilerInvocation>(CI)),
                 // External values that outlive ClangdServer
                 TFS(&TFS),
                 // Index outlives TUScheduler (declared first)
                 FIndex(FIndex),
                 // shared_ptr extends lifetime
                 Stdlib(Stdlib),
                 // We have some FS implementations that rely on information in
                 // the context.
                 Ctx(Context::current().clone())]() mutable {
      // Make sure we install the context into current thread.
      WithContext C(std::move(Ctx));
      clang::noteBottomOfStack();
      IndexFileIn IF;
      IF.Symbols = indexStandardLibrary(std::move(CI), Loc, *TFS);
      if (Stdlib->isBest(LO))
        FIndex->updatePreamble(std::move(IF));
    };
    if (Tasks)
      // This doesn't have a semaphore to enforce -j, but it's rare.
      Tasks->runAsync("IndexStdlib", std::move(Task));
    else
      Task();
  }

  void onMainAST(PathRef Path, ParsedAST &AST, PublishFn Publish) override {
    if (FIndex)
      FIndex->updateMain(Path, AST);

    if (ServerCallbacks)
      Publish([&]() {
        ServerCallbacks->onDiagnosticsReady(Path, AST.version(),
                                            AST.getDiagnostics());
        if (CollectInactiveRegions) {
          ServerCallbacks->onInactiveRegionsReady(Path,
                                                  getInactiveRegions(AST));
        }
      });
  }

  void onFailedAST(PathRef Path, llvm::StringRef Version,
                   std::vector<Diag> Diags, PublishFn Publish) override {
    if (ServerCallbacks)
      Publish(
          [&]() { ServerCallbacks->onDiagnosticsReady(Path, Version, Diags); });
  }

  void onFileUpdated(PathRef File, const TUStatus &Status) override {
    if (ServerCallbacks)
      ServerCallbacks->onFileUpdated(File, Status);
  }

  void onPreamblePublished(PathRef File) override {
    if (ServerCallbacks)
      ServerCallbacks->onSemanticsMaybeChanged(File);
  }

private:
  FileIndex *FIndex;
  ClangdServer::Callbacks *ServerCallbacks;
  const ThreadsafeFS &TFS;
  std::shared_ptr<StdLibSet> Stdlib;
  AsyncTaskRunner *Tasks;
  bool CollectInactiveRegions;
};

class DraftStoreFS : public ThreadsafeFS {
public:
  DraftStoreFS(const ThreadsafeFS &Base, const DraftStore &Drafts)
      : Base(Base), DirtyFiles(Drafts) {}

private:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    auto OFS = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        Base.view(std::nullopt));
    OFS->pushOverlay(DirtyFiles.asVFS());
    return OFS;
  }

  const ThreadsafeFS &Base;
  const DraftStore &DirtyFiles;
};

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
      Drafts, [&](const auto &Entry) { return Entry.first == File; });
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

} // namespace

ClangdServer::Options ClangdServer::optsForTest() {
  ClangdServer::Options Opts;
  Opts.UpdateDebounce = DebouncePolicy::fixed(/*zero*/ {});
  Opts.StorePreamblesInMemory = true;
  Opts.AsyncThreadsCount = 4; // Consistent!
  return Opts;
}

ClangdServer::Options::operator TUScheduler::Options() const {
  TUScheduler::Options Opts;
  Opts.AsyncThreadsCount = AsyncThreadsCount;
  Opts.RetentionPolicy = RetentionPolicy;
  Opts.StorePreamblesInMemory = StorePreamblesInMemory;
  Opts.UpdateDebounce = UpdateDebounce;
  Opts.ContextProvider = ContextProvider;
  Opts.PreambleThrottler = PreambleThrottler;
  return Opts;
}

ClangdServer::ClangdServer(const GlobalCompilationDatabase &CDB,
                           const ThreadsafeFS &TFS, const Options &Opts,
                           Callbacks *Callbacks)
    : FeatureModules(Opts.FeatureModules), CDB(CDB), TFS(TFS),
      DynamicIdx(Opts.BuildDynamicSymbolIndex
                     ? new FileIndex(Opts.EnableOutgoingCalls)
                     : nullptr),
      ModulesManager(Opts.ModulesManager),
      ClangTidyProvider(Opts.ClangTidyProvider),
      UseDirtyHeaders(Opts.UseDirtyHeaders),
      LineFoldingOnly(Opts.LineFoldingOnly),
      PreambleParseForwardingFunctions(Opts.PreambleParseForwardingFunctions),
      SkipPreambleBuild(Opts.SkipPreambleBuild),
      ImportInsertions(Opts.ImportInsertions),
      PublishInactiveRegions(Opts.PublishInactiveRegions),
      WorkspaceRoot(Opts.WorkspaceRoot),
      FileRenameWorkspace(
          WorkspaceRoot ? std::make_unique<WorkspaceSourceCache>(*WorkspaceRoot)
                        : nullptr),
      Transient(Opts.ImplicitCancellation ? TUScheduler::InvalidateOnUpdate
                                          : TUScheduler::NoInvalidation),
      DirtyFS(std::make_unique<DraftStoreFS>(TFS, DraftMgr)),
      ContextProvider(Opts.ContextProvider) {
  if (Opts.AsyncThreadsCount != 0)
    IndexTasks.emplace();
  // Pass a callback into `WorkScheduler` to extract symbols from a newly
  // parsed file and rebuild the file index synchronously each time an AST
  // is parsed.
  WorkScheduler.emplace(CDB, TUScheduler::Options(Opts),
                        std::make_unique<UpdateIndexCallbacks>(
                            DynamicIdx.get(), Callbacks, TFS,
                            IndexTasks ? &*IndexTasks : nullptr,
                            PublishInactiveRegions));
  // Adds an index to the stack, at higher priority than existing indexes.
  auto AddIndex = [&](SymbolIndex *Idx) {
    if (this->Index != nullptr) {
      MergedIdx.push_back(std::make_unique<MergedIndex>(Idx, this->Index));
      this->Index = MergedIdx.back().get();
    } else {
      this->Index = Idx;
    }
  };
  if (Opts.StaticIndex)
    AddIndex(Opts.StaticIndex);
  if (Opts.BackgroundIndex) {
    BackgroundIndex::Options BGOpts;
    BGOpts.ThreadPoolSize = std::max(Opts.AsyncThreadsCount, 1u);
    BGOpts.OnProgress = [Callbacks](BackgroundQueue::Stats S) {
      if (Callbacks)
        Callbacks->onBackgroundIndexProgress(S);
    };
    BGOpts.ContextProvider = Opts.ContextProvider;
    BGOpts.SupportContainedRefs = Opts.EnableOutgoingCalls;
    BackgroundIdx = std::make_unique<BackgroundIndex>(
        TFS, CDB,
        BackgroundIndexStorage::createDiskBackedStorageFactory(
            [&CDB](llvm::StringRef File) { return CDB.getProjectInfo(File); }),
        std::move(BGOpts));
    AddIndex(BackgroundIdx.get());
  }
  if (DynamicIdx)
    AddIndex(DynamicIdx.get());

  if (Opts.FeatureModules) {
    FeatureModule::Facilities F{
        *this->WorkScheduler,
        this->Index,
        this->TFS,
    };
    for (auto &Mod : *Opts.FeatureModules)
      Mod.initialize(F);
  }
}

ClangdServer::~ClangdServer() {
  // Destroying TUScheduler first shuts down request threads that might
  // otherwise access members concurrently.
  // (Nobody can be using TUScheduler because we're on the main thread).
  WorkScheduler.reset();
  // Now requests have stopped, we can shut down feature modules.
  if (FeatureModules) {
    for (auto &Mod : *FeatureModules)
      Mod.stop();
    for (auto &Mod : *FeatureModules)
      Mod.blockUntilIdle(Deadline::infinity());
  }
}

void ClangdServer::addDocument(PathRef File, llvm::StringRef Contents,
                               llvm::StringRef Version,
                               WantDiagnostics WantDiags, bool ForceRebuild) {
  if (FileRenameWorkspace)
    FileRenameWorkspace->invalidate(File);
  std::string ActualVersion = DraftMgr.addDraft(File, Version, Contents);
  ParseOptions Opts;
  Opts.PreambleParseForwardingFunctions = PreambleParseForwardingFunctions;
  Opts.ImportInsertions = ImportInsertions;

  // Compile command is set asynchronously during update, as it can be slow.
  ParseInputs Inputs;
  Inputs.TFS = &getHeaderFS();
  Inputs.Contents = std::string(Contents);
  Inputs.Version = std::move(ActualVersion);
  Inputs.ForceRebuild = ForceRebuild;
  Inputs.Opts = std::move(Opts);
  Inputs.Index = Index;
  Inputs.ClangTidyProvider = ClangTidyProvider;
  Inputs.FeatureModules = FeatureModules;
  Inputs.ModulesManager = ModulesManager;
  adjustParseInputs(Inputs, File);
  bool NewFile = WorkScheduler->update(File, Inputs, WantDiags);
  // If we loaded Foo.h, we want to make sure Foo.cpp is indexed.
  if (NewFile && BackgroundIdx)
    BackgroundIdx->boostRelated(File);
}

void ClangdServer::reparseOpenFilesIfNeeded(
    llvm::function_ref<bool(llvm::StringRef File)> Filter) {
  // Reparse only opened files that were modified.
  for (const Path &FilePath : DraftMgr.getActiveFiles())
    if (Filter(FilePath))
      if (auto Draft = DraftMgr.getDraft(FilePath)) // else disappeared in race?
        addDocument(FilePath, *Draft->Contents, Draft->Version,
                    WantDiagnostics::Auto);
}

std::shared_ptr<const std::string> ClangdServer::getDraft(PathRef File) const {
  auto Draft = DraftMgr.getDraft(File);
  if (!Draft)
    return nullptr;
  return std::move(Draft->Contents);
}

std::function<Context(PathRef)>
ClangdServer::createConfiguredContextProvider(const config::Provider *Provider,
                                              Callbacks *Publish) {
  if (!Provider)
    return [](llvm::StringRef) { return Context::current().clone(); };

  struct Impl {
    const config::Provider *Provider;
    ClangdServer::Callbacks *Publish;
    std::mutex PublishMu;

    Impl(const config::Provider *Provider, ClangdServer::Callbacks *Publish)
        : Provider(Provider), Publish(Publish) {}

    Context operator()(llvm::StringRef File) {
      config::Params Params;
      // Don't reread config files excessively often.
      // FIXME: when we see a config file change event, use the event timestamp?
      Params.FreshTime =
          std::chrono::steady_clock::now() - std::chrono::seconds(5);
      llvm::SmallString<256> PosixPath;
      if (!File.empty()) {
        assert(llvm::sys::path::is_absolute(File));
        llvm::sys::path::native(File, PosixPath, llvm::sys::path::Style::posix);
        Params.Path = PosixPath.str();
      }

      llvm::StringMap<std::vector<Diag>> ReportableDiagnostics;
      Config C = Provider->getConfig(Params, [&](const llvm::SMDiagnostic &D) {
        // Create the map entry even for note diagnostics we don't report.
        // This means that when the file is parsed with no warnings, we
        // publish an empty set of diagnostics, clearing any the client has.
        handleDiagnostic(D, !Publish || D.getFilename().empty()
                                ? nullptr
                                : &ReportableDiagnostics[D.getFilename()]);
      });
      // Blindly publish diagnostics for the (unopened) parsed config files.
      // We must avoid reporting diagnostics for *the same file* concurrently.
      // Source diags are published elsewhere, but those are different files.
      if (!ReportableDiagnostics.empty()) {
        std::lock_guard<std::mutex> Lock(PublishMu);
        for (auto &Entry : ReportableDiagnostics)
          Publish->onDiagnosticsReady(Entry.first(), /*Version=*/"",
                                      Entry.second);
      }
      return Context::current().derive(Config::Key, std::move(C));
    }

    void handleDiagnostic(const llvm::SMDiagnostic &D,
                          std::vector<Diag> *ClientDiagnostics) {
      switch (D.getKind()) {
      case llvm::SourceMgr::DK_Error:
        elog("config error at {0}:{1}:{2}: {3}", D.getFilename(), D.getLineNo(),
             D.getColumnNo(), D.getMessage());
        break;
      case llvm::SourceMgr::DK_Warning:
        log("config warning at {0}:{1}:{2}: {3}", D.getFilename(),
            D.getLineNo(), D.getColumnNo(), D.getMessage());
        break;
      case llvm::SourceMgr::DK_Note:
      case llvm::SourceMgr::DK_Remark:
        vlog("config note at {0}:{1}:{2}: {3}", D.getFilename(), D.getLineNo(),
             D.getColumnNo(), D.getMessage());
        ClientDiagnostics = nullptr; // Don't emit notes as LSP diagnostics.
        break;
      }
      if (ClientDiagnostics)
        ClientDiagnostics->push_back(toDiag(D, Diag::ClangdConfig));
    }
  };

  // Copyable wrapper.
  return [I(std::make_shared<Impl>(Provider, Publish))](llvm::StringRef Path) {
    return (*I)(Path);
  };
}

void ClangdServer::removeDocument(PathRef File) {
  if (FileRenameWorkspace)
    FileRenameWorkspace->invalidate(File);
  DraftMgr.removeDraft(File);
  WorkScheduler->remove(File);
}

void ClangdServer::codeComplete(PathRef File, Position Pos,
                                const clangd::CodeCompleteOptions &Opts,
                                Callback<CodeCompleteResult> CB) {
  // Copy completion options for passing them to async task handler.
  auto CodeCompleteOpts = Opts;
  if (!CodeCompleteOpts.Index) // Respect overridden index.
    CodeCompleteOpts.Index = Index;

  auto Task = [Pos, CodeCompleteOpts, File = File.str(), CB = std::move(CB),
               this](llvm::Expected<InputsAndPreamble> IP) mutable {
    if (!IP)
      return CB(IP.takeError());
    if (auto Reason = isCancelled())
      return CB(llvm::make_error<CancelledError>(Reason));

    std::optional<SpeculativeFuzzyFind> SpecFuzzyFind;
    if (!IP->Preamble) {
      // No speculation in Fallback mode, as it's supposed to be much faster
      // without compiling.
      vlog("Build for file {0} is not ready. Enter fallback mode.", File);
    } else if (CodeCompleteOpts.Index) {
      SpecFuzzyFind.emplace();
      {
        std::lock_guard<std::mutex> Lock(CachedCompletionFuzzyFindRequestMutex);
        SpecFuzzyFind->CachedReq = CachedCompletionFuzzyFindRequestByFile[File];
      }
    }
    ParseInputs ParseInput{IP->Command, &getHeaderFS(), IP->Contents.str()};
    // FIXME: Add traling new line if there is none at eof, workaround a crash,
    // see https://github.com/clangd/clangd/issues/332
    if (!IP->Contents.ends_with("\n"))
      ParseInput.Contents.append("\n");
    ParseInput.Index = Index;

    CodeCompleteOpts.MainFileSignals = IP->Signals;
    CodeCompleteOpts.AllScopes = Config::current().Completion.AllScopes;
    CodeCompleteOpts.ArgumentLists = Config::current().Completion.ArgumentLists;
    CodeCompleteOpts.InsertIncludes =
        Config::current().Completion.HeaderInsertion;
    CodeCompleteOpts.CodePatterns = Config::current().Completion.CodePatterns;
    CodeCompleteOpts.MacroFilter = Config::current().Completion.MacroFilter;
    adjustParseInputs(ParseInput, File);
    // FIXME(ibiryukov): even if Preamble is non-null, we may want to check
    // both the old and the new version in case only one of them matches.
    CodeCompleteResult Result = clangd::codeComplete(
        File, Pos, IP->Preamble, ParseInput, CodeCompleteOpts,
        SpecFuzzyFind ? &*SpecFuzzyFind : nullptr);
    // We don't want `codeComplete` to wait for the async call if it doesn't use
    // the result (e.g. non-index completion, speculation fails), so that `CB`
    // is called as soon as results are available.
    {
      clang::clangd::trace::Span Tracer("Completion results callback");
      CB(std::move(Result));
    }
    if (!SpecFuzzyFind)
      return;
    if (SpecFuzzyFind->NewReq) {
      std::lock_guard<std::mutex> Lock(CachedCompletionFuzzyFindRequestMutex);
      CachedCompletionFuzzyFindRequestByFile[File] = *SpecFuzzyFind->NewReq;
    }
    // Explicitly block until async task completes, this is fine as we've
    // already provided reply to the client and running as a preamble task
    // (i.e. won't block other preamble tasks).
    if (SpecFuzzyFind->Result.valid())
      SpecFuzzyFind->Result.wait();
  };

  // We use a potentially-stale preamble because latency is critical here.
  WorkScheduler->runWithPreamble(
      "CodeComplete", File,
      (Opts.RunParser == CodeCompleteOptions::AlwaysParse)
          ? TUScheduler::Stale
          : TUScheduler::StaleOrAbsent,
      std::move(Task));
}

void ClangdServer::signatureHelp(PathRef File, Position Pos,
                                 MarkupKind DocumentationFormat,
                                 Callback<SignatureHelp> CB) {

  auto Action = [Pos, File = File.str(), CB = std::move(CB),
                 DocumentationFormat,
                 this](llvm::Expected<InputsAndPreamble> IP) mutable {
    if (!IP)
      return CB(IP.takeError());

    const auto *PreambleData = IP->Preamble;
    if (!PreambleData)
      return CB(error("Failed to parse includes"));

    ParseInputs ParseInput{IP->Command, &getHeaderFS(), IP->Contents.str()};
    // FIXME: Add traling new line if there is none at eof, workaround a crash,
    // see https://github.com/clangd/clangd/issues/332
    if (!IP->Contents.ends_with("\n"))
      ParseInput.Contents.append("\n");
    ParseInput.Index = Index;
    CB(clangd::signatureHelp(File, Pos, *PreambleData, ParseInput,
                             DocumentationFormat));
  };

  // Unlike code completion, we wait for a preamble here.
  WorkScheduler->runWithPreamble("SignatureHelp", File, TUScheduler::Stale,
                                 std::move(Action));
}

void ClangdServer::formatFile(PathRef File, const std::vector<Range> &Rngs,
                              Callback<tooling::Replacements> CB) {
  auto Code = getDraft(File);
  if (!Code)
    return CB(llvm::make_error<LSPError>("trying to format non-added document",
                                         ErrorCode::InvalidParams));
  std::vector<tooling::Range> RequestedRanges;
  if (!Rngs.empty()) {
    RequestedRanges.reserve(Rngs.size());
    for (const auto &Rng : Rngs) {
      llvm::Expected<size_t> Begin = positionToOffset(*Code, Rng.start);
      if (!Begin)
        return CB(Begin.takeError());
      llvm::Expected<size_t> End = positionToOffset(*Code, Rng.end);
      if (!End)
        return CB(End.takeError());
      RequestedRanges.emplace_back(*Begin, *End - *Begin);
    }
  } else {
    RequestedRanges = {tooling::Range(0, Code->size())};
  }

  // Call clang-format.
  auto Action = [File = File.str(), Code = std::move(*Code),
                 Ranges = std::move(RequestedRanges), CB = std::move(CB),
                 this]() mutable {
    format::FormatStyle Style = getFormatStyleForFile(File, Code, TFS, true);
    tooling::Replacements IncludeReplaces =
        format::sortIncludes(Style, Code, Ranges, File);
    auto Changed = tooling::applyAllReplacements(Code, IncludeReplaces);
    if (!Changed)
      return CB(Changed.takeError());

    CB(IncludeReplaces.merge(format::reformat(
        Style, *Changed,
        tooling::calculateRangesAfterReplacements(IncludeReplaces, Ranges),
        File)));
  };
  WorkScheduler->runQuick("Format", File, std::move(Action));
}

void ClangdServer::formatOnType(PathRef File, Position Pos,
                                StringRef TriggerText,
                                Callback<std::vector<TextEdit>> CB) {
  auto Code = getDraft(File);
  if (!Code)
    return CB(llvm::make_error<LSPError>("trying to format non-added document",
                                         ErrorCode::InvalidParams));
  llvm::Expected<size_t> CursorPos = positionToOffset(*Code, Pos);
  if (!CursorPos)
    return CB(CursorPos.takeError());
  auto Action = [File = File.str(), Code = std::move(*Code),
                 TriggerText = TriggerText.str(), CursorPos = *CursorPos,
                 CB = std::move(CB), this]() mutable {
    auto Style = getFormatStyleForFile(File, Code, TFS, false);
    std::vector<TextEdit> Result;
    for (const tooling::Replacement &R :
         formatIncremental(Code, CursorPos, TriggerText, Style))
      Result.push_back(replacementToEdit(Code, R));
    return CB(Result);
  };
  WorkScheduler->runQuick("FormatOnType", File, std::move(Action));
}

void ClangdServer::prepareRename(PathRef File, Position Pos,
                                 std::optional<std::string> NewName,
                                 const RenameOptions &RenameOpts,
                                 Callback<RenameResult> CB) {
  auto Action = [Pos, File = File.str(), CB = std::move(CB),
                 NewName = std::move(NewName),
                 RenameOpts](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    // prepareRename is latency-sensitive: we don't query the index, as we
    // only need main-file references
    auto Results =
        clangd::rename({Pos, NewName.value_or("__clangd_rename_placeholder"),
                        InpAST->AST, File, /*FS=*/nullptr,
                        /*Index=*/nullptr, RenameOpts});
    if (!Results) {
      // LSP says to return null on failure, but that will result in a generic
      // failure message. If we send an LSP error response, clients can surface
      // the message to users (VSCode does).
      return CB(Results.takeError());
    }
    return CB(*Results);
  };
  WorkScheduler->runWithAST("PrepareRename", File, std::move(Action));
}

void ClangdServer::rename(PathRef File, Position Pos, llvm::StringRef NewName,
                          const RenameOptions &Opts,
                          Callback<RenameResult> CB) {
  auto Action = [File = File.str(), NewName = NewName.str(), Pos, Opts,
                 CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    // Tracks number of files edited per invocation.
    static constexpr trace::Metric RenameFiles("rename_files",
                                               trace::Metric::Distribution);
    if (!InpAST)
      return CB(InpAST.takeError());
    auto R = clangd::rename({Pos, NewName, InpAST->AST, File,
                             DirtyFS->view(std::nullopt), Index, Opts});
    if (!R)
      return CB(R.takeError());

    if (Opts.WantFormat) {
      auto Style = getFormatStyleForFile(File, InpAST->Inputs.Contents,
                                         *InpAST->Inputs.TFS, false);
      llvm::Error Err = llvm::Error::success();
      for (auto &E : R->GlobalChanges)
        Err =
            llvm::joinErrors(reformatEdit(E.getValue(), Style), std::move(Err));

      if (Err)
        return CB(std::move(Err));
    }
    RenameFiles.record(R->GlobalChanges.size());
    return CB(*R);
  };
  WorkScheduler->runWithAST("Rename", File, std::move(Action));
}

void ClangdServer::prepareFileRename(
    llvm::ArrayRef<std::pair<Path, Path>> Renames, Callback<WorkspaceEdit> CB) {
  if (!BackgroundIdx)
    return CB(error("file rename requires background indexing"));
  if (!WorkspaceRoot)
    return CB(error("file rename requires a workspace root"));

  std::vector<std::pair<Path, Path>> OwnedRenames(Renames.begin(),
                                                  Renames.end());
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
        auto Mappings = expandFileRenames(Renames, *WorkspaceRoot, *FS);
        if (!Mappings)
          return CB(Mappings.takeError());
        if (Mappings->empty()) {
          WorkspaceEdit Result;
          Result.documentChanges.emplace();
          return CB(std::move(Result));
        }

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

        llvm::StringSet<> TranslationUnits;
        for (PathRef TU : Graph->TranslationUnits)
          TranslationUnits.insert(TU);
        for (const WorkspaceSourceFile &File : WorkspaceFiles) {
          bool Represented = File.IsHeader
                                 ? llvm::any_of(Graph->Files,
                                                [&](const auto &N) {
                                                  return N.File == File.File;
                                                })
                                 : TranslationUnits.contains(File.File);
          if (!Represented && File.IsHeader) {
            auto HasIncludes = hasIncludeDirectives(File.File, *FS);
            if (!HasIncludes)
              return CB(HasIncludes.takeError());
            if (!*HasIncludes)
              continue;
          }
          if (!Represented)
            return CB(error("workspace source is not represented in the "
                            "background include graph: {0}",
                            File.File));
        }

        for (const auto &Command : Graph->Commands)
          if (auto Err = validateCompileCommandForRenames(
                  Command.getValue(), Renames, *Mappings, FS.get()))
            return CB(std::move(Err));

        auto InWorkspace = [&](PathRef File) {
          return File == *WorkspaceRoot || pathStartsWith(*WorkspaceRoot, File);
        };
        for (const auto &Node : Graph->Files) {
          if (!InWorkspace(Node.File))
            continue;
          if (Node.Flags & IncludeGraphNode::SourceFlag::HadErrors)
            return CB(
                error("background index contains errors for {0}", Node.File));
        }

        // Every represented closed file contributes to the proof, even when
        // its old graph had no includes and it would not otherwise become an
        // edit candidate.
        llvm::StringMap<FileDigest> IndexedDigests;
        for (const auto &Node : Graph->Files) {
          if (!InWorkspace(Node.File) || findDraft(Drafts, Node.File))
            continue;
          auto [It, Inserted] =
              IndexedDigests.try_emplace(Node.File, Node.Digest);
          if (!Inserted && It->getValue() != Node.Digest)
            return CB(error("background contexts disagree on the digest for "
                            "{0}",
                            Node.File));
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

        llvm::StringMap<std::vector<const BackgroundIndex::IndexedFile *>>
            Candidates;
        llvm::StringSet<> ActiveCandidates;
        for (const auto &Node : Graph->Files) {
          if (!InWorkspace(Node.File))
            continue;
          auto Affected = IsRenamed(Node.File);
          if (!Affected)
            return CB(Affected.takeError());
          for (PathRef Included : Node.DirectIncludes) {
            if (*Affected)
              break;
            auto IncludedAffected = IsRenamed(Included);
            if (!IncludedAffected)
              return CB(IncludedAffected.takeError());
            *Affected = *IncludedAffected;
          }
          bool HasLiteralConditionalInclude = false;
          if (Node.HasConditionalIncludes) {
            auto Conditional = conditionalIncludeDirectives(Node.File, *FS);
            if (!Conditional)
              return CB(Conditional.takeError());
            HasLiteralConditionalInclude =
                llvm::any_of(*Conditional, [](const auto &Inclusion) {
                  return !Inclusion.Written.empty() &&
                         Inclusion.Directive != tok::pp_include_next;
                });
          }
          if (*Affected)
            ActiveCandidates.insert(Node.File);
          if (*Affected || HasLiteralConditionalInclude)
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
            if (Node.File == File && !llvm::is_contained(Contexts, &Node))
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
          if (const DraftStore::Draft *Draft = findDraft(Drafts, File)) {
            Contents[File] = *Draft->Contents;
          } else {
            auto Buffer = FS->getBufferForFile(File);
            if (!Buffer)
              return CB(error("cannot read includer {0}: {1}", File,
                              Buffer.getError().message()));
            Contents[File] = Buffer->get()->getBuffer().str();
            ClosedCandidates.insert(File);
          }
          auto Conditional = conditionalIncludeDirectives(File, *FS);
          if (!Conditional)
            return CB(Conditional.takeError());
          if (!ActiveCandidates.contains(File))
            llvm::erase_if(*Conditional, [](const auto &Inclusion) {
              return Inclusion.Written.empty() ||
                     Inclusion.Directive == tok::pp_include_next;
            });
          ConditionalIncludes[File] = std::move(*Conditional);
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
            auto Command = Graph->Commands.find(Context->DependentTU);
            if (Command == Graph->Commands.end())
              return CB(error("no indexed compilation command is available "
                              "for {0}",
                              Context->DependentTU));
            tooling::CompileCommand ParseCommand = Command->getValue();
            if (File != Context->DependentTU)
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
            const auto &Conditional = ConditionalIncludes.lookup(File);
            if (!Conditional.empty()) {
              std::string ProbeCode;
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
                ProbeCode += "#";
                ProbeCode += Keyword;
                ProbeCode += " ";
                ProbeCode += Inclusion.Written;
                ProbeCode += "\n";
              }
              ParseInputs ProbeInputs{std::move(ProbeCommand), &SnapshotFS,
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
              if (Resolved.size() != Conditional.size())
                return CB(error("cannot exhaustively resolve conditional "
                                "includes in {0}",
                                File));
              for (size_t I = 0; I < Conditional.size(); ++I) {
                if (Resolved[I].Written != Conditional[I].Written ||
                    Resolved[I].Resolved.empty())
                  return CB(error("cannot exhaustively resolve conditional "
                                  "include {0} in {1}",
                                  Conditional[I].Written, File));
                if (llvm::any_of(Includes.MainFileIncludes,
                                 [&](const Inclusion &Existing) {
                                   return Existing.HashOffset ==
                                          Conditional[I].HashOffset;
                                 }))
                  continue;
                Inclusion Synthesized = Resolved[I];
                Synthesized.HashOffset = Conditional[I].HashOffset;
                Synthesized.HashLine = Conditional[I].HashLine;
                Includes.MainFileIncludes.push_back(std::move(Synthesized));
              }
            }

            auto Style =
                getFormatStyleForFile(File, Inputs.Contents, SnapshotFS, false);
            auto Edits = renameIncludeDirectives(
                File, Inputs.Contents, Includes,
                AST->getPreprocessor().getHeaderSearchInfo(),
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
        auto CurrentGraph = BackgroundIdx->includeGraphSnapshot();
        if (!CurrentGraph || CurrentGraph->Generation != Graph->Generation) {
          if (!CurrentGraph)
            llvm::consumeError(CurrentGraph.takeError());
          return CB(llvm::make_error<LSPError>(
              "background include graph changed while preparing file rename",
              ErrorCode::ContentModified));
        }

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
        return CB(std::move(Result));
      });
}

void ClangdServer::didRenameFiles(
    llvm::ArrayRef<std::pair<Path, Path>> Renames) {
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
    if (OpenFile == *NewPath)
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

namespace {
// May generate several candidate selections, due to SelectionTree ambiguity.
// vector of pointers because GCC doesn't like non-copyable Selection.
llvm::Expected<std::vector<std::unique_ptr<Tweak::Selection>>>
tweakSelection(const Range &Sel, const InputsAndAST &AST,
               llvm::vfs::FileSystem *FS) {
  auto Begin = positionToOffset(AST.Inputs.Contents, Sel.start);
  if (!Begin)
    return Begin.takeError();
  auto End = positionToOffset(AST.Inputs.Contents, Sel.end);
  if (!End)
    return End.takeError();
  std::vector<std::unique_ptr<Tweak::Selection>> Result;
  SelectionTree::createEach(
      AST.AST.getASTContext(), AST.AST.getTokens(), *Begin, *End,
      [&](SelectionTree T) {
        Result.push_back(std::make_unique<Tweak::Selection>(
            AST.Inputs.Index, AST.AST, *Begin, *End, std::move(T), FS));
        return false;
      });
  assert(!Result.empty() && "Expected at least one SelectionTree");
  return std::move(Result);
}

// Some fixes may perform local renaming, we want to convert those to clangd
// rename commands, such that we can leverage the index for more accurate
// results.
std::optional<ClangdServer::CodeActionResult::Rename>
tryConvertToRename(const Diag *Diag, const Fix &Fix) {
  bool IsClangTidyRename = Diag->Source == Diag::ClangTidy &&
                           Diag->Name == "readability-identifier-naming" &&
                           !Fix.Edits.empty();
  if (IsClangTidyRename && Diag->InsideMainFile) {
    ClangdServer::CodeActionResult::Rename R;
    R.NewName = Fix.Edits.front().newText;
    R.FixMessage = Fix.Message;
    R.Diag = {Diag->Range, Diag->Message};
    return R;
  }

  return std::nullopt;
}

} // namespace

void ClangdServer::codeAction(const CodeActionInputs &Params,
                              Callback<CodeActionResult> CB) {
  auto Action = [Params, CB = std::move(CB),
                 FeatureModules(this->FeatureModules)](
                    Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    auto KindAllowed =
        [Only(Params.RequestedActionKinds)](llvm::StringRef Kind) {
          if (Only.empty())
            return true;
          return llvm::any_of(Only, [&](llvm::StringRef Base) {
            return Kind.consume_front(Base) &&
                   (Kind.empty() || Kind.starts_with("."));
          });
        };

    CodeActionResult Result;
    Result.Version = InpAST->AST.version().str();
    if (KindAllowed(CodeAction::QUICKFIX_KIND)) {
      auto FindMatchedDiag = [&InpAST](const DiagRef &DR) -> const Diag * {
        for (const auto &Diag : InpAST->AST.getDiagnostics())
          if (Diag.Range == DR.Range && Diag.Message == DR.Message)
            return &Diag;
        return nullptr;
      };
      for (const auto &DiagRef : Params.Diagnostics) {
        if (const auto *Diag = FindMatchedDiag(DiagRef))
          for (const auto &Fix : Diag->Fixes) {
            if (auto Rename = tryConvertToRename(Diag, Fix)) {
              Result.Renames.emplace_back(std::move(*Rename));
            } else {
              Result.QuickFixes.push_back({DiagRef, Fix});
            }
          }
      }
    }

    // Collect Tweaks
    auto Selections = tweakSelection(Params.Selection, *InpAST, /*FS=*/nullptr);
    if (!Selections)
      return CB(Selections.takeError());
    // Don't allow a tweak to fire more than once across ambiguous selections.
    llvm::DenseSet<llvm::StringRef> PreparedTweaks;
    auto DeduplicatingFilter = [&](const Tweak &T) {
      return KindAllowed(T.kind()) && Params.TweakFilter(T) &&
             !PreparedTweaks.count(T.id());
    };
    for (const auto &Sel : *Selections) {
      for (auto &T : prepareTweaks(*Sel, DeduplicatingFilter, FeatureModules)) {
        Result.TweakRefs.push_back(TweakRef{T->id(), T->title(), T->kind()});
        PreparedTweaks.insert(T->id());
        TweakAvailable.record(1, T->id());
      }
    }
    CB(std::move(Result));
  };

  WorkScheduler->runWithAST("codeAction", Params.File, std::move(Action),
                            Transient);
}

void ClangdServer::applyTweak(PathRef File, Range Sel, StringRef TweakID,
                              Callback<Tweak::Effect> CB) {
  // Tracks number of times a tweak has been attempted.
  static constexpr trace::Metric TweakAttempt(
      "tweak_attempt", trace::Metric::Counter, "tweak_id");
  // Tracks number of times a tweak has failed to produce edits.
  static constexpr trace::Metric TweakFailed(
      "tweak_failed", trace::Metric::Counter, "tweak_id");
  TweakAttempt.record(1, TweakID);
  auto Action = [File = File.str(), Sel, TweakID = TweakID.str(),
                 CB = std::move(CB),
                 this](Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    auto FS = DirtyFS->view(std::nullopt);
    auto Selections = tweakSelection(Sel, *InpAST, FS.get());
    if (!Selections)
      return CB(Selections.takeError());
    std::optional<llvm::Expected<Tweak::Effect>> Effect;
    // Try each selection, take the first one that prepare()s.
    // If they all fail, Effect will hold get the last error.
    for (const auto &Selection : *Selections) {
      auto T = prepareTweak(TweakID, *Selection, FeatureModules);
      if (T) {
        Effect = (*T)->apply(*Selection);
        break;
      }
      Effect = T.takeError();
    }
    assert(Effect && "Expected at least one selection");
    if (*Effect && (*Effect)->FormatEdits) {
      // Format tweaks that require it centrally here.
      for (auto &It : (*Effect)->ApplyEdits) {
        Edit &E = It.second;
        format::FormatStyle Style =
            getFormatStyleForFile(File, E.InitialCode, TFS, false);
        if (llvm::Error Err = reformatEdit(E, Style))
          elog("Failed to format {0}: {1}", It.first(), std::move(Err));
      }
    } else {
      TweakFailed.record(1, TweakID);
    }
    return CB(std::move(*Effect));
  };
  WorkScheduler->runWithAST("ApplyTweak", File, std::move(Action));
}

void ClangdServer::locateSymbolAt(PathRef File, Position Pos,
                                  Callback<std::vector<LocatedSymbol>> CB) {
  auto Action = [Pos, CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::locateSymbolAt(InpAST->AST, Pos, Index));
  };

  WorkScheduler->runWithAST("Definitions", File, std::move(Action));
}

void ClangdServer::switchSourceHeader(
    PathRef Path, Callback<std::optional<clangd::Path>> CB) {
  // We want to return the result as fast as possible, strategy is:
  //  1) use the file-only heuristic, it requires some IO but it is much
  //     faster than building AST, but it only works when .h/.cc files are in
  //     the same directory.
  //  2) if 1) fails, we use the AST&Index approach, it is slower but supports
  //     different code layout.
  if (auto CorrespondingFile =
          getCorrespondingHeaderOrSource(Path, TFS.view(std::nullopt)))
    return CB(std::move(CorrespondingFile));
  auto Action = [Path = Path.str(), CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(getCorrespondingHeaderOrSource(Path, InpAST->AST, Index));
  };
  WorkScheduler->runWithAST("SwitchHeaderSource", Path, std::move(Action));
}

void ClangdServer::findDocumentHighlights(
    PathRef File, Position Pos, Callback<std::vector<DocumentHighlight>> CB) {
  auto Action =
      [Pos, CB = std::move(CB)](llvm::Expected<InputsAndAST> InpAST) mutable {
        if (!InpAST)
          return CB(InpAST.takeError());
        CB(clangd::findDocumentHighlights(InpAST->AST, Pos));
      };

  WorkScheduler->runWithAST("Highlights", File, std::move(Action), Transient);
}

void ClangdServer::findHover(PathRef File, Position Pos,
                             Callback<std::optional<HoverInfo>> CB) {
  auto Action = [File = File.str(), Pos, CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    format::FormatStyle Style = getFormatStyleForFile(
        File, InpAST->Inputs.Contents, *InpAST->Inputs.TFS, false);
    CB(clangd::getHover(InpAST->AST, Pos, std::move(Style), Index));
  };

  WorkScheduler->runWithAST("Hover", File, std::move(Action), Transient);
}

void ClangdServer::typeHierarchy(PathRef File, Position Pos, int Resolve,
                                 TypeHierarchyDirection Direction,
                                 Callback<std::vector<TypeHierarchyItem>> CB) {
  auto Action = [File = File.str(), Pos, Resolve, Direction, CB = std::move(CB),
                 this](Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::getTypeHierarchy(InpAST->AST, Pos, Resolve, Direction, Index,
                                File));
  };

  WorkScheduler->runWithAST("TypeHierarchy", File, std::move(Action));
}

void ClangdServer::superTypes(
    const TypeHierarchyItem &Item,
    Callback<std::optional<std::vector<TypeHierarchyItem>>> CB) {
  WorkScheduler->run("typeHierarchy/superTypes", /*Path=*/"",
                     [=, CB = std::move(CB)]() mutable {
                       CB(clangd::superTypes(Item, Index));
                     });
}

void ClangdServer::subTypes(const TypeHierarchyItem &Item,
                            Callback<std::vector<TypeHierarchyItem>> CB) {
  WorkScheduler->run(
      "typeHierarchy/subTypes", /*Path=*/"",
      [=, CB = std::move(CB)]() mutable { CB(clangd::subTypes(Item, Index)); });
}

void ClangdServer::resolveTypeHierarchy(
    TypeHierarchyItem Item, int Resolve, TypeHierarchyDirection Direction,
    Callback<std::optional<TypeHierarchyItem>> CB) {
  WorkScheduler->run(
      "Resolve Type Hierarchy", "", [=, CB = std::move(CB)]() mutable {
        clangd::resolveTypeHierarchy(Item, Resolve, Direction, Index);
        CB(Item);
      });
}

void ClangdServer::prepareCallHierarchy(
    PathRef File, Position Pos, Callback<std::vector<CallHierarchyItem>> CB) {
  auto Action = [File = File.str(), Pos,
                 CB = std::move(CB)](Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::prepareCallHierarchy(InpAST->AST, Pos, File));
  };
  WorkScheduler->runWithAST("CallHierarchy", File, std::move(Action));
}

void ClangdServer::incomingCalls(
    const CallHierarchyItem &Item,
    Callback<std::vector<CallHierarchyIncomingCall>> CB) {
  WorkScheduler->run("Incoming Calls", "",
                     [CB = std::move(CB), Item, this]() mutable {
                       CB(clangd::incomingCalls(Item, Index));
                     });
}

void ClangdServer::inlayHints(PathRef File, std::optional<Range> RestrictRange,
                              Callback<std::vector<InlayHint>> CB) {
  auto Action = [RestrictRange(std::move(RestrictRange)),
                 CB = std::move(CB)](Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::inlayHints(InpAST->AST, std::move(RestrictRange)));
  };
  WorkScheduler->runWithAST("InlayHints", File, std::move(Action), Transient);
}

void ClangdServer::outgoingCalls(
    const CallHierarchyItem &Item,
    Callback<std::vector<CallHierarchyOutgoingCall>> CB) {
  WorkScheduler->run("Outgoing Calls", "",
                     [CB = std::move(CB), Item, this]() mutable {
                       CB(clangd::outgoingCalls(Item, Index));
                     });
}

void ClangdServer::onFileEvent(const DidChangeWatchedFilesParams &Params) {
  if (!FileRenameWorkspace)
    return;
  for (const FileEvent &Change : Params.changes)
    FileRenameWorkspace->invalidate(Change.uri.file());
}

void ClangdServer::workspaceSymbols(
    llvm::StringRef Query, int Limit,
    Callback<std::vector<SymbolInformation>> CB) {
  WorkScheduler->run(
      "getWorkspaceSymbols", /*Path=*/"",
      [Query = Query.str(), Limit, CB = std::move(CB), this]() mutable {
        CB(clangd::getWorkspaceSymbols(Query, Limit, Index,
                                       WorkspaceRoot.value_or("")));
      });
}

void ClangdServer::documentSymbols(llvm::StringRef File,
                                   Callback<std::vector<DocumentSymbol>> CB) {
  auto Action =
      [CB = std::move(CB)](llvm::Expected<InputsAndAST> InpAST) mutable {
        if (!InpAST)
          return CB(InpAST.takeError());
        CB(clangd::getDocumentSymbols(InpAST->AST));
      };
  WorkScheduler->runWithAST("DocumentSymbols", File, std::move(Action),
                            Transient);
}

void ClangdServer::foldingRanges(llvm::StringRef File,
                                 Callback<std::vector<FoldingRange>> CB) {
  auto Code = getDraft(File);
  if (!Code)
    return CB(llvm::make_error<LSPError>(
        "trying to compute folding ranges for non-added document",
        ErrorCode::InvalidParams));
  auto Action = [LineFoldingOnly = LineFoldingOnly, CB = std::move(CB),
                 Code = std::move(*Code)]() mutable {
    CB(clangd::getFoldingRanges(Code, LineFoldingOnly));
  };
  // We want to make sure folding ranges are always available for all the open
  // files, hence prefer runQuick to not wait for operations on other files.
  WorkScheduler->runQuick("FoldingRanges", File, std::move(Action));
}

void ClangdServer::findType(llvm::StringRef File, Position Pos,
                            Callback<std::vector<LocatedSymbol>> CB) {
  auto Action = [Pos, CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::findType(InpAST->AST, Pos, Index));
  };
  WorkScheduler->runWithAST("FindType", File, std::move(Action));
}

void ClangdServer::findImplementations(
    PathRef File, Position Pos, Callback<std::vector<LocatedSymbol>> CB) {
  auto Action = [Pos, CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::findImplementations(InpAST->AST, Pos, Index));
  };

  WorkScheduler->runWithAST("Implementations", File, std::move(Action));
}

void ClangdServer::findReferences(PathRef File, Position Pos, uint32_t Limit,
                                  bool AddContainer,
                                  Callback<ReferencesResult> CB) {
  auto Action = [Pos, Limit, AddContainer, CB = std::move(CB),
                 this](llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    CB(clangd::findReferences(InpAST->AST, Pos, Limit, Index, AddContainer));
  };

  WorkScheduler->runWithAST("References", File, std::move(Action));
}

void ClangdServer::symbolInfo(PathRef File, Position Pos,
                              Callback<std::vector<SymbolDetails>> CB) {
  auto Action =
      [Pos, CB = std::move(CB)](llvm::Expected<InputsAndAST> InpAST) mutable {
        if (!InpAST)
          return CB(InpAST.takeError());
        CB(clangd::getSymbolInfo(InpAST->AST, Pos));
      };

  WorkScheduler->runWithAST("SymbolInfo", File, std::move(Action));
}

void ClangdServer::semanticRanges(PathRef File,
                                  const std::vector<Position> &Positions,
                                  Callback<std::vector<SelectionRange>> CB) {
  auto Action = [Positions, CB = std::move(CB)](
                    llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    std::vector<SelectionRange> Result;
    for (const auto &Pos : Positions) {
      if (auto Range = clangd::getSemanticRanges(InpAST->AST, Pos))
        Result.push_back(std::move(*Range));
      else
        return CB(Range.takeError());
    }
    CB(std::move(Result));
  };
  WorkScheduler->runWithAST("SemanticRanges", File, std::move(Action));
}

void ClangdServer::documentLinks(PathRef File,
                                 Callback<std::vector<DocumentLink>> CB) {
  auto Action =
      [CB = std::move(CB)](llvm::Expected<InputsAndAST> InpAST) mutable {
        if (!InpAST)
          return CB(InpAST.takeError());
        CB(clangd::getDocumentLinks(InpAST->AST));
      };
  WorkScheduler->runWithAST("DocumentLinks", File, std::move(Action),
                            Transient);
}

void ClangdServer::semanticHighlights(
    PathRef File, Callback<std::vector<HighlightingToken>> CB) {

  auto Action = [CB = std::move(CB),
                 PublishInactiveRegions = PublishInactiveRegions](
                    llvm::Expected<InputsAndAST> InpAST) mutable {
    if (!InpAST)
      return CB(InpAST.takeError());
    // Include inactive regions in semantic highlighting tokens only if the
    // client doesn't support a dedicated protocol for being informed about
    // them.
    CB(clangd::getSemanticHighlightings(InpAST->AST, !PublishInactiveRegions));
  };
  WorkScheduler->runWithAST("SemanticHighlights", File, std::move(Action),
                            Transient);
}

void ClangdServer::getAST(PathRef File, std::optional<Range> R,
                          Callback<std::optional<ASTNode>> CB) {
  auto Action =
      [R, CB(std::move(CB))](llvm::Expected<InputsAndAST> Inputs) mutable {
        if (!Inputs)
          return CB(Inputs.takeError());
        if (!R) {
          // It's safe to pass in the TU, as dumpAST() does not
          // deserialize the preamble.
          auto Node = DynTypedNode::create(
              *Inputs->AST.getASTContext().getTranslationUnitDecl());
          return CB(dumpAST(Node, Inputs->AST.getTokens(),
                            Inputs->AST.getASTContext()));
        }
        unsigned Start, End;
        if (auto Offset = positionToOffset(Inputs->Inputs.Contents, R->start))
          Start = *Offset;
        else
          return CB(Offset.takeError());
        if (auto Offset = positionToOffset(Inputs->Inputs.Contents, R->end))
          End = *Offset;
        else
          return CB(Offset.takeError());
        bool Success = SelectionTree::createEach(
            Inputs->AST.getASTContext(), Inputs->AST.getTokens(), Start, End,
            [&](SelectionTree T) {
              if (const SelectionTree::Node *N = T.commonAncestor()) {
                CB(dumpAST(N->ASTNode, Inputs->AST.getTokens(),
                           Inputs->AST.getASTContext()));
                return true;
              }
              return false;
            });
        if (!Success)
          CB(std::nullopt);
      };
  WorkScheduler->runWithAST("GetAST", File, std::move(Action));
}

void ClangdServer::customAction(PathRef File, llvm::StringRef Name,
                                Callback<InputsAndAST> Action) {
  WorkScheduler->runWithAST(Name, File, std::move(Action));
}

void ClangdServer::diagnostics(PathRef File, Callback<std::vector<Diag>> CB) {
  auto Action =
      [CB = std::move(CB)](llvm::Expected<InputsAndAST> InpAST) mutable {
        if (!InpAST)
          return CB(InpAST.takeError());
        return CB(InpAST->AST.getDiagnostics());
      };

  WorkScheduler->runWithAST("Diagnostics", File, std::move(Action));
}

llvm::StringMap<TUScheduler::FileStats> ClangdServer::fileStats() const {
  return WorkScheduler->fileStats();
}

[[nodiscard]] bool
ClangdServer::blockUntilIdleForTest(std::optional<double> TimeoutSeconds) {
  // Order is important here: we don't want to block on A and then B,
  // if B might schedule work on A.

#if defined(__has_feature) &&                                                  \
    (__has_feature(address_sanitizer) || __has_feature(hwaddress_sanitizer) || \
     __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer))
  if (TimeoutSeconds.has_value())
    (*TimeoutSeconds) *= 10;
#endif

  // Nothing else can schedule work on TUScheduler, because it's not threadsafe
  // and we're blocking the main thread.
  if (!WorkScheduler->blockUntilIdle(timeoutSeconds(TimeoutSeconds)))
    return false;
  // TUScheduler is the only thing that starts background indexing work.
  if (IndexTasks && !IndexTasks->wait(timeoutSeconds(TimeoutSeconds)))
    return false;

  // Unfortunately we don't have strict topological order between the rest of
  // the components. E.g. CDB broadcast triggers backrgound indexing.
  // This queries the CDB which may discover new work if disk has changed.
  //
  // So try each one a few times in a loop.
  // If there are no tricky interactions then all after the first are no-ops.
  // Then on the last iteration, verify they're idle without waiting.
  //
  // There's a small chance they're juggling work and we didn't catch them :-(
  for (std::optional<double> Timeout :
       {TimeoutSeconds, TimeoutSeconds, std::optional<double>(0)}) {
    if (!CDB.blockUntilIdle(timeoutSeconds(Timeout)))
      return false;
    if (BackgroundIdx && !BackgroundIdx->blockUntilIdleForTest(Timeout))
      return false;
    if (FeatureModules && llvm::any_of(*FeatureModules, [&](FeatureModule &M) {
          return !M.blockUntilIdle(timeoutSeconds(Timeout));
        }))
      return false;
  }

  assert(WorkScheduler->blockUntilIdle(Deadline::zero()) &&
         "Something scheduled work while we're blocking the main thread!");
  return true;
}

void ClangdServer::profile(MemoryTree &MT) const {
  if (DynamicIdx)
    DynamicIdx->profile(MT.child("dynamic_index"));
  if (BackgroundIdx)
    BackgroundIdx->profile(MT.child("background_index"));
  WorkScheduler->profile(MT.child("tuscheduler"));
}

void ClangdServer::adjustParseInputs(ParseInputs &Inputs, PathRef File) const {
  // FIXME: Don't perform optimization when the TU requires C++20
  // named modules. Mixing PCH and modules may cause different issues (incorrect
  // diagnostics, crashes) due to instability of such scenario support in the
  // clang.
  auto HasRequiredModules = [this, File]() {
    if (!ModulesManager)
      return false;
    // Required modules check uses compile commands extracted from the
    // compilation database.
    // We use context provider here to make command mangler to use compile
    // command adjustments from the config.
    WithContext Ctx(ContextProvider ? ContextProvider(File)
                                    : Context::current().clone());
    return ModulesManager->hasRequiredModules(File);
  };
  Inputs.Opts.SkipPreambleBuild = SkipPreambleBuild || HasRequiredModules();
}

} // namespace clangd
} // namespace clang
