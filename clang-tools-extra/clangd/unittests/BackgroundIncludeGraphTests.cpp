//===-- BackgroundIncludeGraphTests.cpp --------------------------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BackgroundIndexTestUtils.h"
#include "CompileCommands.h"
#include "Headers.h"
#include "TestFS.h"
#include "index/Background.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

namespace clang {
namespace clangd {

MATCHER_P(symbolNamed, N, "") { return arg.Name == N; }

TEST_F(BackgroundIndexTest, IncludeGraphPreservesTranslationUnitContexts) {
  MockFS FS;
  const Path Common = testPath("root/common.h");
  const Path A = testPath("root/a.cpp");
  const Path B = testPath("root/b.cpp");
  const Path ADep = testPath("root/a/dep.h");
  const Path BDep = testPath("root/b/dep.h");
  FS.Files[Common] = "#include <dep.h>\n";
  FS.Files[A] = "#include \"common.h\"\n";
  FS.Files[B] = "#include \"common.h\"\n";
  FS.Files[ADep] = "struct FromA {};\n";
  FS.Files[BDep] = "struct FromB {};\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Directory = testPath("root");
  Cmd.Filename = A;
  Cmd.CommandLine = {"clang++", "-I", testPath("root/a"), A};
  CDB.setCompileCommand(A, Cmd);
  Cmd.Filename = B;
  Cmd.CommandLine = {"clang++", "-I", testPath("root/b"), B};
  CDB.setCompileCommand(B, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_EQ(Graph->Commands.lookup(A).CommandLine[2], testPath("root/a"));
  EXPECT_EQ(Graph->Commands.lookup(B).CommandLine[2], testPath("root/b"));
  EXPECT_THAT(
      Graph->Files,
      testing::AllOf(
          Contains(testing::AllOf(
              testing::Field(&BackgroundIndex::IndexedFile::File, Common),
              testing::Field(&BackgroundIndex::IndexedFile::DependentTU, A),
              testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                             ElementsAre(ADep)))),
          Contains(testing::AllOf(
              testing::Field(&BackgroundIndex::IndexedFile::File, Common),
              testing::Field(&BackgroundIndex::IndexedFile::DependentTU, B),
              testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                             ElementsAre(BDep))))));
}

TEST_F(BackgroundIndexTest, IncludeGraphMarksConditionalIncludes) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Header = testPath("root/conditional.h");
  FS.Files[Main] = R"cpp(
#include "conditional.h"
#if ENABLE_HEADER
#include "conditional.h"
#endif
)cpp";
  FS.Files[Header] = R"cpp(
#if ENABLE_OTHER
#include OTHER_HEADER
#endif
)cpp";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Directory = testPath("root");
  Cmd.Filename = Main;
  Cmd.CommandLine = {"clang++", "-DENABLE_HEADER=0", Main};
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(
      Graph->Files,
      Contains(testing::AllOf(
          testing::Field(&BackgroundIndex::IndexedFile::File, Main),
          testing::Field(
              &BackgroundIndex::IndexedFile::Flags,
              testing::Truly([](IncludeGraphNode::SourceFlag Flags) {
                return Flags & IncludeGraphNode::SourceFlag::HasConditionalIncludes;
              })))));
  EXPECT_THAT(
      Graph->Files,
      Contains(testing::AllOf(
          testing::Field(&BackgroundIndex::IndexedFile::File, Header),
          testing::Field(
              &BackgroundIndex::IndexedFile::Flags,
              testing::Truly([](IncludeGraphNode::SourceFlag Flags) {
                return Flags & IncludeGraphNode::SourceFlag::HasConditionalIncludes;
              })))));
}

TEST_F(BackgroundIndexTest, IncludeGraphRejectsMissingTranslationUnit) {
  MockFS FS;
  const Path Missing = testPath("root/missing.cpp");
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Filename = Missing;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", Missing};
  CDB.setCompileCommand(Missing, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(Idx.includeGraphSnapshot(),
                       llvm::FailedWithMessage(
                           testing::HasSubstr("background indexing failed")));
}

TEST_F(BackgroundIndexTest, IncludeGraphRecoversFromMalformedLoadedShard) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  FS.Files[Main] = "int value;\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  tooling::CompileCommand Cmd;
  Cmd.Filename = Main;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", Main};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(Main, Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  auto Corrupt = readIndexFile(Storage.lookup(Main), SymbolOrigin::Background);
  ASSERT_THAT_EXPECTED(Corrupt, llvm::Succeeded());
  ASSERT_TRUE(Corrupt->Sources);
  Corrupt->Sources->erase(URI::create(Main).toString());
  Storage[Main] = llvm::to_string(IndexFileOut(*Corrupt));

  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(Graph->Files, Contains(testing::Field(
                                &BackgroundIndex::IndexedFile::File, Main)));
}

TEST_F(BackgroundIndexTest, IncludeGraphLoadsExactContextsFromCache) {
  CountingMockFS FS;
  const Path MainA = testPath("root/a.cpp");
  const Path MainB = testPath("root/b.cpp");
  const Path Common = testPath("root/common.h");
  const Path ADep = testPath("root/a/dep.h");
  const Path BDep = testPath("root/b/dep.h");
  FS.Files[MainA] = "#include \"common.h\"\n";
  FS.Files[MainB] = "#include \"common.h\"\n";
  FS.Files[Common] = "#include <dep.h>\n";
  FS.Files[ADep] = "struct ADep {};\n";
  FS.Files[BDep] = "struct BDep {};\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  auto Command = [](PathRef File, PathRef Include) {
    tooling::CompileCommand Cmd;
    Cmd.Filename = File.str();
    Cmd.Directory = testPath("root");
    Cmd.CommandLine = {"clang++", "-I", Include.str(), File.str()};
    return Cmd;
  };
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(MainA, Command(MainA, testPath("root/a")));
    CDB.setCompileCommand(MainB, Command(MainB, testPath("root/b")));
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  FS.resetReads();
  MSS.StoredPaths.clear();

  std::atomic<unsigned> Enqueued{0};
  BackgroundIndex::Options Opts;
  Opts.OnProgress = [&](BackgroundQueue::Stats S) { Enqueued = S.Enqueued; };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  CDB.setCompileCommand(MainA, Command(MainA, testPath("root/a")));
  CDB.setCompileCommand(MainB, Command(MainB, testPath("root/b")));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_TRUE(MSS.StoredPaths.empty());
  EXPECT_EQ(FS.reads(Common), 2U)
      << "one shard staleness read for each translation-unit context";
  const unsigned BeforeEnsure = Enqueued;
  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(Graph->TranslationUnits, UnorderedElementsAre(MainA, MainB));
  EXPECT_THAT(
      Graph->Files,
      testing::AllOf(
          Contains(testing::AllOf(
              testing::Field(&BackgroundIndex::IndexedFile::File, Common),
              testing::Field(&BackgroundIndex::IndexedFile::DependentTU, MainA),
              testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                             ElementsAre(ADep)))),
          Contains(testing::AllOf(
              testing::Field(&BackgroundIndex::IndexedFile::File, Common),
              testing::Field(&BackgroundIndex::IndexedFile::DependentTU, MainB),
              testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                             ElementsAre(BDep))))));

  Idx.ensureIncludeGraph();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_EQ(Enqueued, BeforeEnsure);
}

TEST_F(BackgroundIndexTest,
       IncludeGraphLoadsConditionalStateWhenSourceIsMissing) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  FS.Files[Main] = "#if ENABLED\n#include \"inactive.h\"\n#endif\nint value;\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  tooling::CompileCommand Cmd;
  Cmd.Filename = Main;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", Main};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(Main, Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  auto Cached = readIndexFile(Storage.lookup(Main), SymbolOrigin::Background);
  ASSERT_THAT_EXPECTED(Cached, llvm::Succeeded());
  ASSERT_TRUE(Cached->ContextSources);
  auto MainSource = Cached->ContextSources->find(URI::create(Main).toString());
  ASSERT_NE(MainSource, Cached->ContextSources->end());
  EXPECT_TRUE(MainSource->getValue().Flags &
              IncludeGraphNode::SourceFlag::HasConditionalIncludes);

  FS.Files.erase(Main);
  MSS.StoredPaths.clear();
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_TRUE(MSS.StoredPaths.empty());
  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(
      Graph->Files,
      Contains(testing::AllOf(
          testing::Field(&BackgroundIndex::IndexedFile::File, Main),
          testing::Field(
              &BackgroundIndex::IndexedFile::Flags,
              testing::Truly([](IncludeGraphNode::SourceFlag Flags) {
                return Flags & IncludeGraphNode::SourceFlag::HasConditionalIncludes;
              })))));
}

TEST_F(BackgroundIndexTest, RebuildsMissingOrMalformedCachedContextGraph) {
  for (bool Malformed : {false, true}) {
    MockFS FS;
    const Path Main = testPath("root/main.cpp");
    FS.Files[Main] = "int value;\n";
    llvm::StringMap<std::string> Storage;
    size_t CacheHits = 0;
    MemoryShardStorage MSS(Storage, CacheHits);
    tooling::CompileCommand Cmd;
    Cmd.Filename = Main;
    Cmd.Directory = testPath("root");
    Cmd.CommandLine = {"clang++", Main};
    {
      OverlayCDB CDB(/*Base=*/nullptr);
      BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                          /*Opts=*/{});
      CDB.setCompileCommand(Main, Cmd);
      ASSERT_TRUE(Idx.blockUntilIdleForTest());
    }

    auto Cached = contextCacheWithState(Storage.lookup(Main), Malformed);
    ASSERT_THAT_EXPECTED(Cached, llvm::Succeeded());
    Storage[Main] = std::move(*Cached);

    MSS.StoredPaths.clear();
    MSS.AllowLoadErrors = Malformed;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(Main, Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    EXPECT_TRUE(MSS.StoredPaths.contains(Main));
    auto Rewritten =
        readIndexFile(Storage.lookup(Main), SymbolOrigin::Background);
    ASSERT_THAT_EXPECTED(Rewritten, llvm::Succeeded());
    EXPECT_TRUE(Rewritten->ContextSources);
    ASSERT_TRUE(Rewritten->Symbols);
    EXPECT_THAT(*Rewritten->Symbols, Contains(symbolNamed("value")));
    ASSERT_THAT_EXPECTED(Idx.includeGraphSnapshot(), llvm::Succeeded());
  }
}

TEST_F(BackgroundIndexTest, RestoresCommandInputsAndDerivedCommandFromCache) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Forced = testPath("root/forced.h");
  FS.Files[Main] = "int value;\n";
  FS.Files[Forced] = "struct Forced {};\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  tooling::CompileCommand Cmd;
  Cmd.Filename = Main;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "-include", Forced, Main};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    CDB.setCompileCommand(Main, Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  MSS.StoredPaths.clear();
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_TRUE(MSS.StoredPaths.empty());
  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(
      Graph->Files,
      Contains(testing::AllOf(
          testing::Field(&BackgroundIndex::IndexedFile::File, Forced),
          testing::Field(&BackgroundIndex::IndexedFile::Flags,
                         IncludeGraphNode::SourceFlag::IsCommandInput))));
  ASSERT_TRUE(Graph->CC1Commands.contains(Main));
  ASSERT_FALSE(Graph->CC1Commands.lookup(Main).empty());
  EXPECT_EQ(Graph->CC1Commands.lookup(Main).front(), "-cc1");
}

TEST_F(BackgroundIndexTest, RebuildsInconsistentCachedContextState) {
  enum class Corruption {
    CC1Marker,
    MainSourceFlag,
    MainDigest,
    EmptyCommand,
    CanonicalCollision
  };
  for (Corruption Kind : {Corruption::CC1Marker, Corruption::MainSourceFlag,
                          Corruption::MainDigest, Corruption::EmptyCommand,
                          Corruption::CanonicalCollision}) {
    MockFS FS;
    const Path Main = testPath("root/main.cpp");
    FS.Files[Main] = "int value;\n";
    llvm::StringMap<std::string> Storage;
    size_t CacheHits = 0;
    MemoryShardStorage MSS(Storage, CacheHits);
    tooling::CompileCommand Cmd;
    Cmd.Filename = Main;
    Cmd.Directory = testPath("root");
    Cmd.CommandLine = {"clang++", Main};
    {
      OverlayCDB CDB(/*Base=*/nullptr);
      BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
      CDB.setCompileCommand(Main, Cmd);
      ASSERT_TRUE(Idx.blockUntilIdleForTest());
    }
    auto Cached = readIndexFile(Storage.lookup(Main), SymbolOrigin::Background);
    ASSERT_THAT_EXPECTED(Cached, llvm::Succeeded());
    const std::string MainURI = URI::create(Main).toString();
    std::string AliasURI;
    switch (Kind) {
    case Corruption::CC1Marker:
      ASSERT_TRUE(Cached->CC1CommandLine);
      Cached->CC1CommandLine->front() = "-not-cc1";
      break;
    case Corruption::MainSourceFlag:
      ASSERT_TRUE(Cached->Sources);
      Cached->Sources->find(MainURI)->getValue().Flags =
          IncludeGraphNode::SourceFlag::None;
      break;
    case Corruption::MainDigest:
      ASSERT_TRUE(Cached->ContextSources);
      Cached->ContextSources->find(MainURI)->getValue().Digest =
          digest("different");
      break;
    case Corruption::EmptyCommand:
      ASSERT_TRUE(Cached->Cmd);
      Cached->Cmd->CommandLine.clear();
      break;
    case Corruption::CanonicalCollision: {
      ASSERT_TRUE(Cached->ContextSources);
      IncludeGraphNode Alias;
      AliasURI = URI::create(testPath("root/sub/../main.cpp")).toString();
      ASSERT_NE(AliasURI, MainURI);
      Alias.URI = AliasURI;
      Alias.Digest = digest(FS.Files.lookup(Main));
      Alias.Flags = IncludeGraphNode::SourceFlag::IsCommandInput;
      ASSERT_THAT_ERROR(addSerializedIncludeGraphNode(*Cached->ContextSources,
                                                      std::move(Alias),
                                                      /*ExactContext=*/true),
                        llvm::Succeeded());
      break;
    }
    }
    Storage[Main] = llvm::to_string(IndexFileOut(*Cached));
    MSS.StoredPaths.clear();
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    CDB.setCompileCommand(Main, Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    EXPECT_TRUE(MSS.StoredPaths.contains(Main));
    if (Kind == Corruption::CanonicalCollision) {
      auto Rewritten =
          readIndexFile(Storage.lookup(Main), SymbolOrigin::Background);
      ASSERT_THAT_EXPECTED(Rewritten, llvm::Succeeded());
      ASSERT_TRUE(Rewritten->ContextSources);
      EXPECT_FALSE(Rewritten->ContextSources->contains(AliasURI));
    }
    ASSERT_THAT_EXPECTED(Idx.includeGraphSnapshot(), llvm::Succeeded());
  }
}

} // namespace clangd
} // namespace clang
