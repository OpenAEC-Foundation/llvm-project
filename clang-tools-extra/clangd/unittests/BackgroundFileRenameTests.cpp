//===-- BackgroundFileRenameTests.cpp --------------------------------*- C++
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

using ::testing::Contains;
using ::testing::ElementsAre;

namespace clang {
namespace clangd {

TEST_F(BackgroundIndexTest, FileRenameMigratesIncludeGraphAndShard) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "inline int value() { return 1; }\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Filename = Main;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", Main};
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Before = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Before, llvm::Succeeded());
  ASSERT_TRUE(llvm::any_of(Before->Files, [&](const auto &File) {
    return pathEqual(File.File, Main) &&
           llvm::any_of(File.DirectIncludes, [&](PathRef Include) {
             return pathEqual(Include, Old);
           });
  }));

  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  FS.Files[Main] = "#include \"new.h\"\n";
  ASSERT_THAT_ERROR(Idx.filesRenamed({{Old, New}}), llvm::Succeeded());
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto After = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(After, llvm::Succeeded());
  EXPECT_TRUE(llvm::any_of(After->Files, [&](const auto &File) {
    return pathEqual(File.File, Main) &&
           llvm::any_of(File.DirectIncludes, [&](PathRef Include) {
             return pathEqual(Include, New);
           });
  }));
  EXPECT_FALSE(Storage.contains(Old));
  EXPECT_TRUE(Storage.contains(New));
}

TEST_F(BackgroundIndexTest, FileRenameMigratesCacheOnlyState) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "struct RenamedSymbol {};\n";
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

  MSS.StoredPaths.clear();
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_TRUE(MSS.StoredPaths.empty());

  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  FS.Files[Main] = "#include \"new.h\"\n";
  ASSERT_THAT_ERROR(CDB.prepareFileRenames({{Old, New}}, {{Main, Cmd}}),
                    llvm::Succeeded());
  ASSERT_THAT_ERROR(CDB.filesRenamed({{Old, New}}), llvm::Succeeded());
  ASSERT_THAT_ERROR(Idx.filesRenamed({{Old, New}}), llvm::Succeeded());
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(Graph->Files,
              Contains(testing::AllOf(
                  testing::Field(&BackgroundIndex::IndexedFile::File, Main),
                  testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                                 ElementsAre(New)))));
  EXPECT_FALSE(Storage.contains(Old));
  EXPECT_TRUE(Storage.contains(New));
  FuzzyFindRequest Request;
  Request.Query = "";
  Request.Scopes = {""};
  std::vector<std::string> SymbolFiles;
  Idx.fuzzyFind(Request, [&](const Symbol &S) {
    if (S.Name == "RenamedSymbol")
      SymbolFiles.emplace_back(S.CanonicalDeclaration.FileURI);
  });
  EXPECT_THAT(SymbolFiles, ElementsAre(URI::create(New).toString()));
}

TEST_F(BackgroundIndexTest, FileRenameOnlyReindexesAffectedContexts) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  const Path Unrelated = testPath("root/unrelated.cpp");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "";
  FS.Files[Unrelated] = "int unrelated;\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Directory = testPath("root");
  Cmd.Filename = Main;
  Cmd.CommandLine = {"clang++", Main};
  CDB.setCompileCommand(Main, Cmd);
  Cmd.Filename = Unrelated;
  Cmd.CommandLine = {"clang++", Unrelated};
  CDB.setCompileCommand(Unrelated, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  MSS.AccessedPaths.clear();
  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  FS.Files[Main] = "#include \"new.h\"\n";
  ASSERT_THAT_ERROR(Idx.filesRenamed({{Old, New}}), llvm::Succeeded());
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_FALSE(MSS.AccessedPaths.contains(Unrelated));
}

TEST_F(BackgroundIndexTest, FileRenameInvalidatesActiveIndexingCommit) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  std::mutex Mu;
  std::condition_variable CV;
  bool StoreEntered = false;
  bool ReleaseStore = false;
  MSS.BeforeStore = [&](PathRef Shard) {
    if (Shard != Old)
      return;
    std::unique_lock<std::mutex> Lock(Mu);
    StoreEntered = true;
    CV.notify_all();
    CV.wait(Lock, [&] { return ReleaseStore; });
  };

  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Directory = testPath("root");
  Cmd.Filename = Main;
  Cmd.CommandLine = {"clang++", Main};
  CDB.setCompileCommand(Main, Cmd);
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(CV.wait_for(Lock, std::chrono::seconds(10),
                            [&] { return StoreEntered; }));
  }

  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  FS.Files[Main] = "#include \"new.h\"\n";
  auto Rename = std::async(std::launch::async,
                           [&] { return Idx.filesRenamed({{Old, New}}); });
  EXPECT_EQ(Rename.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  {
    std::lock_guard<std::mutex> Lock(Mu);
    ReleaseStore = true;
  }
  CV.notify_all();
  ASSERT_THAT_ERROR(Rename.get(), llvm::Succeeded());
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(Graph->Files,
              Contains(testing::AllOf(
                  testing::Field(&BackgroundIndex::IndexedFile::File, Main),
                  testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                                 ElementsAre(New)))));
  EXPECT_FALSE(Storage.contains(Old));
}

TEST_F(BackgroundIndexTest, MalformedCacheClearWaitsForActiveIndexingCommit) {
  MockFS FS;
  const Path Active = testPath("root/active.cpp");
  const Path Malformed = testPath("root/malformed.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  FS.Files[Active] = "#include \"old.h\"\n";
  FS.Files[Malformed] = "int malformed;\n";
  FS.Files[Old] = "struct Header {};\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  auto Command = [](PathRef File) {
    tooling::CompileCommand Cmd;
    Cmd.Directory = testPath("root");
    Cmd.Filename = File.str();
    Cmd.CommandLine = {"clang++", File.str()};
    return Cmd;
  };
  tooling::CompileCommand ActiveCmd = Command(Active);
  tooling::CompileCommand MalformedCmd = Command(Malformed);
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(Active, ActiveCmd);
    CDB.setCompileCommand(Malformed, MalformedCmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  auto Corrupt = contextCacheWithState(Storage.lookup(Malformed),
                                       /*Malformed=*/true);
  ASSERT_THAT_EXPECTED(Corrupt, llvm::Succeeded());
  Storage[Malformed] = std::move(*Corrupt);
  MSS.AllowLoadErrors = true;
  MSS.StoreCounts.clear();
  MSS.StoredPaths.clear();
  MSS.WasCleared = false;

  std::mutex Mu;
  std::condition_variable CV;
  unsigned MalformedContextCalls = 0;
  bool MalformedLoadEntered = false;
  bool ReleaseMalformedLoad = false;
  bool MalformedIndexEntered = false;
  bool ReleaseMalformedIndex = false;
  bool StoreEntered = false;
  bool ReleaseStore = false;
  MSS.BeforeStore = [&](PathRef Shard) {
    if (Shard != Old)
      return;
    std::unique_lock<std::mutex> Lock(Mu);
    StoreEntered = true;
    CV.notify_all();
    CV.wait(Lock, [&] { return ReleaseStore; });
  };

  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 2;
  Opts.ContextProvider = [&](PathRef File) {
    if (File != Malformed)
      return Context::current().clone();
    std::unique_lock<std::mutex> Lock(Mu);
    ++MalformedContextCalls;
    if (MalformedContextCalls == 1) {
      MalformedLoadEntered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return ReleaseMalformedLoad; });
    } else if (MalformedContextCalls == 2) {
      MalformedIndexEntered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return ReleaseMalformedIndex; });
    }
    return Context::current().clone();
  };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  FS.Files[Old] = "struct Header { int changed; };\n";
  ActiveCmd.CommandLine.push_back("-DACTIVE_CHANGED");
  CDB.setCompileCommand(Malformed, MalformedCmd);
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(CV.wait_for(Lock, std::chrono::seconds(10),
                            [&] { return MalformedLoadEntered; }));
  }
  CDB.setCompileCommand(Active, ActiveCmd);
  {
    std::lock_guard<std::mutex> Lock(Mu);
    ReleaseMalformedLoad = true;
  }
  CV.notify_all();
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(CV.wait_for(Lock, std::chrono::seconds(10), [&] {
      return StoreEntered && MalformedIndexEntered;
    }));
  }

  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  FS.Files[Active] = "#include \"new.h\"\n";
  auto Rename = std::async(std::launch::async,
                           [&] { return Idx.filesRenamed({{Old, New}}); });
  EXPECT_EQ(Rename.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  EXPECT_FALSE(MSS.WasCleared);
  {
    std::lock_guard<std::mutex> Lock(Mu);
    ReleaseStore = true;
  }
  CV.notify_all();
  ASSERT_THAT_ERROR(Rename.get(), llvm::Succeeded());
  {
    std::lock_guard<std::mutex> Lock(Mu);
    ReleaseMalformedIndex = true;
  }
  CV.notify_all();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_TRUE(MSS.WasCleared);
  EXPECT_EQ(MSS.StoreCounts.lookup(Malformed), 1U)
      << "the pre-rename malformed-cache task must not commit";
  EXPECT_FALSE(Storage.contains(Old));
  EXPECT_TRUE(Storage.contains(New));
  EXPECT_TRUE(Storage.contains(Active));
  EXPECT_TRUE(Storage.contains(Malformed));
  EXPECT_EQ(Storage.size(), 3U);
  auto Graph = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
  EXPECT_THAT(Graph->Files,
              Contains(testing::AllOf(
                  testing::Field(&BackgroundIndex::IndexedFile::File, Active),
                  testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                                 ElementsAre(New)))));
}

TEST_F(BackgroundIndexTest, FileRenameInvalidatesQueuedCommandChange) {
  MockFS FS;
  const Path Blocker = testPath("root/blocker.cpp");
  const Path Old = testPath("root/old.cpp");
  const Path New = testPath("root/new.cpp");
  FS.Files[Blocker] = "int blocker;\n";
  FS.Files[Old] = "int renamed;\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  std::mutex Mu;
  std::condition_variable CV;
  bool Block = true;
  bool Entered = false;
  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 1;
  Opts.ContextProvider = [&](PathRef File) {
    if (!File.empty())
      return Context::current().clone();
    std::unique_lock<std::mutex> Lock(Mu);
    if (Block) {
      Entered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return !Block; });
    }
    return Context::current().clone();
  };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  auto Command = [](PathRef File) {
    tooling::CompileCommand Cmd;
    Cmd.Filename = File.str();
    Cmd.Directory = testPath("root");
    Cmd.CommandLine = {"clang++", File.str()};
    return Cmd;
  };
  CDB.setCompileCommand(Blocker, Command(Blocker));
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(
        CV.wait_for(Lock, std::chrono::seconds(10), [&] { return Entered; }));
  }
  CDB.setCompileCommand(Old, Command(Old));
  MSS.AccessedPaths.clear();
  MSS.StoredPaths.clear();
  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  auto OldCommand = CDB.getCompileCommand(Old);
  auto BlockerCommand = CDB.getCompileCommand(Blocker);
  ASSERT_TRUE(OldCommand);
  ASSERT_TRUE(BlockerCommand);
  ASSERT_THAT_ERROR(
      CDB.prepareFileRenames({{Old, New}},
                             {{Old, *OldCommand}, {Blocker, *BlockerCommand}}),
      llvm::Succeeded());
  ASSERT_THAT_ERROR(CDB.filesRenamed({{Old, New}}), llvm::Succeeded());
  ASSERT_THAT_ERROR(Idx.filesRenamed({{Old, New}}), llvm::Succeeded());
  {
    std::lock_guard<std::mutex> Lock(Mu);
    Block = false;
  }
  CV.notify_all();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_FALSE(MSS.StoredPaths.contains(Old));
  EXPECT_TRUE(MSS.StoredPaths.contains(New));
}

TEST_F(BackgroundIndexTest, FileRenameRejectsIncompleteTUsAtSameDestination) {
  MockFS FS;
  const Path First = testPath("root/first.cpp");
  const Path Second = testPath("root/second.cpp");
  const Path Destination = testPath("root/destination.cpp");
  FS.Files[First] = "int first;\n";
  FS.Files[Second] = "int second;\n";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  std::mutex Mu;
  std::condition_variable CV;
  bool Entered = false;
  bool Release = false;
  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 1;
  Opts.ContextProvider = [&](PathRef File) {
    if (!File.empty())
      return Context::current().clone();
    std::unique_lock<std::mutex> Lock(Mu);
    if (!Entered) {
      Entered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return Release; });
    }
    return Context::current().clone();
  };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  auto command = [](PathRef File) {
    tooling::CompileCommand Cmd;
    Cmd.Directory = testPath("root");
    Cmd.Filename = File.str();
    Cmd.CommandLine = {"clang++", File.str()};
    return Cmd;
  };
  CDB.setCompileCommand(First, command(First));
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(
        CV.wait_for(Lock, std::chrono::seconds(10), [&] { return Entered; }));
  }
  CDB.setCompileCommand(Second, command(Second));

  EXPECT_THAT_ERROR(
      Idx.filesRenamed({{First, Destination}, {Second, Destination}}),
      llvm::FailedWithMessage(
          testing::HasSubstr("collides at translation unit")));
  {
    std::lock_guard<std::mutex> Lock(Mu);
    Release = true;
  }
  CV.notify_all();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
}

TEST_F(BackgroundIndexTest, DirectoryRenameInvalidatesBlockedCacheLoad) {
  for (int CacheState : {0, 1, 2}) {
    MockFS FS;
    const Path Main = testPath("root/main.cpp");
    const Path OldDirectory = testPath("root/old");
    const Path NewDirectory = testPath("root/new");
    const Path OldHeader = testPath("root/old/header.h");
    const Path NewHeader = testPath("root/new/header.h");
    const Path OldChild = testPath("root/old/sub/child.h");
    const Path NewChild = testPath("root/new/sub/child.h");
    FS.Files[Main] = "#include \"old/header.h\"\n";
    FS.Files[OldHeader] = "#include \"sub/child.h\"\nstruct CachedSymbol {};\n";
    FS.Files[OldChild] = "struct Child {};\n";
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
    ASSERT_TRUE(Storage.contains(OldHeader));
    ASSERT_TRUE(Storage.contains(OldChild));
    if (CacheState != 0) {
      auto Rewritten = contextCacheWithState(Storage.lookup(Main),
                                             /*Malformed=*/CacheState == 2);
      ASSERT_THAT_EXPECTED(Rewritten, llvm::Succeeded());
      Storage[Main] = std::move(*Rewritten);
      if (CacheState == 2)
        MSS.AllowLoadErrors = true;
    }

    std::mutex Mu;
    std::condition_variable CV;
    bool FirstMainLoad = true;
    bool LoadEntered = false;
    bool ReleaseLoad = false;
    MSS.BeforeLoad = [&](PathRef Shard) {
      if (Shard != Main)
        return;
      std::unique_lock<std::mutex> Lock(Mu);
      if (!FirstMainLoad)
        return;
      FirstMainLoad = false;
      LoadEntered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return ReleaseLoad; });
    };

    BackgroundIndex::Options Opts;
    Opts.ThreadPoolSize = 1;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(
        FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
    CDB.setCompileCommand(Main, Cmd);
    {
      std::unique_lock<std::mutex> Lock(Mu);
      ASSERT_TRUE(CV.wait_for(Lock, std::chrono::seconds(10),
                              [&] { return LoadEntered; }));
    }

    FS.Files[NewHeader] = FS.Files[OldHeader];
    FS.Files[NewChild] = FS.Files[OldChild];
    FS.Files.erase(OldHeader);
    FS.Files.erase(OldChild);
    FS.Files[Main] = "#include \"new/header.h\"\n";
    auto MainCommand = CDB.getCompileCommand(Main);
    ASSERT_TRUE(MainCommand);
    ASSERT_THAT_ERROR(CDB.prepareFileRenames({{OldDirectory, NewDirectory}},
                                             {{Main, *MainCommand}}),
                      llvm::Succeeded());
    ASSERT_THAT_ERROR(CDB.filesRenamed({{OldDirectory, NewDirectory}}),
                      llvm::Succeeded());
    ASSERT_THAT_ERROR(Idx.filesRenamed({{OldDirectory, NewDirectory}}),
                      llvm::Succeeded());
    {
      std::lock_guard<std::mutex> Lock(Mu);
      ReleaseLoad = true;
    }
    CV.notify_all();
    ASSERT_TRUE(Idx.blockUntilIdleForTest());

    EXPECT_FALSE(Storage.contains(OldHeader));
    EXPECT_FALSE(Storage.contains(OldChild));
    EXPECT_TRUE(Storage.contains(NewHeader));
    EXPECT_TRUE(Storage.contains(NewChild));
    EXPECT_EQ(MSS.WasCleared, CacheState == 2);
    auto Graph = Idx.includeGraphSnapshot();
    ASSERT_THAT_EXPECTED(Graph, llvm::Succeeded());
    EXPECT_THAT(
        Graph->Files,
        Contains(testing::AllOf(
            testing::Field(&BackgroundIndex::IndexedFile::File, Main),
            testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                           ElementsAre(NewHeader)))));
  }
}

TEST_F(BackgroundIndexTest, IncludeGraphBuildIsInvalidatedByRenameEpoch) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Blocker = testPath("root/blocker.cpp");
  const Path Old = testPath("root/old.h");
  const Path New = testPath("root/new.h");
  FS.Files[Main] = "int value;\n";
  FS.Files[Blocker] = "int blocker;\n";
  FS.Files[Old] = "";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  tooling::CompileCommand Cmd;
  Cmd.Filename = Main;
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", Main};
  tooling::CompileCommand BlockerCmd = Cmd;
  BlockerCmd.Filename = Blocker;
  BlockerCmd.CommandLine = {"clang++", Blocker};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(Main, Cmd);
    CDB.setCompileCommand(Blocker, BlockerCmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  std::mutex Mu;
  std::condition_variable CV;
  bool ShouldBlock = false;
  bool Entered = false;
  bool Released = false;
  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 1;
  Opts.ContextProvider = [&](PathRef File) {
    std::unique_lock<std::mutex> Lock(Mu);
    if (File.empty() && ShouldBlock) {
      Entered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return Released; });
    }
    return Context::current().clone();
  };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  CDB.setCompileCommand(Main, Cmd);
  CDB.setCompileCommand(Blocker, BlockerCmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  {
    std::lock_guard<std::mutex> Lock(Mu);
    ShouldBlock = true;
  }
  BlockerCmd.CommandLine.push_back("-DBLOCKER_CHANGED");
  CDB.setCompileCommand(Blocker, BlockerCmd);
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(
        CV.wait_for(Lock, std::chrono::seconds(10), [&] { return Entered; }));
  }
  Cmd.CommandLine.push_back("-DMAIN_CHANGED");
  CDB.setCompileCommand(Main, Cmd);
  Idx.ensureIncludeGraph();
  FS.Files[New] = FS.Files[Old];
  FS.Files.erase(Old);
  ASSERT_THAT_ERROR(Idx.filesRenamed({{Old, New}}), llvm::Succeeded());
  {
    std::lock_guard<std::mutex> Lock(Mu);
    ShouldBlock = false;
    Released = true;
  }
  CV.notify_all();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  ASSERT_THAT_EXPECTED(Idx.includeGraphSnapshot(), llvm::Succeeded());
}

TEST_F(BackgroundIndexTest, FailedFileRenameDoesNotMutateGraphOrStorage) {
  MockFS FS;
  const Path Main = testPath("root/main.cpp");
  const Path Header = testPath("root/header.h");
  const Path FirstTarget = testPath("root/first.cpp");
  const Path SecondTarget = testPath("root/second.cpp");
  FS.Files[Main] = "#include \"header.h\"\n";
  FS.Files[Header] = "struct Header {};\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  tooling::CompileCommand Cmd;
  Cmd.Directory = testPath("root");
  Cmd.Filename = Main;
  Cmd.CommandLine = {"clang++", Main};
  CDB.setCompileCommand(Main, Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Before = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(Before, llvm::Succeeded());
  ASSERT_TRUE(Storage.contains(Main));
  ASSERT_TRUE(Storage.contains(Header));
  const std::string MainShard = Storage.lookup(Main);
  const std::string HeaderShard = Storage.lookup(Header);

  ASSERT_THAT_ERROR(
      Idx.filesRenamed({{Main, FirstTarget}, {Main, SecondTarget}}),
      llvm::FailedWithMessage(testing::HasSubstr("overlapping file renames")));

  auto After = Idx.includeGraphSnapshot();
  ASSERT_THAT_EXPECTED(After, llvm::Succeeded());
  EXPECT_EQ(After->Generation, Before->Generation);
  EXPECT_THAT(After->TranslationUnits, ElementsAre(Main));
  EXPECT_THAT(After->Files,
              Contains(testing::AllOf(
                  testing::Field(&BackgroundIndex::IndexedFile::File, Main),
                  testing::Field(&BackgroundIndex::IndexedFile::DirectIncludes,
                                 ElementsAre(Header)))));
  EXPECT_EQ(Storage.size(), 2U);
  EXPECT_EQ(Storage.lookup(Main), MainShard);
  EXPECT_EQ(Storage.lookup(Header), HeaderShard);
  EXPECT_FALSE(MSS.WasCleared);
}

TEST_F(BackgroundIndexTest, FailedFileRenameDoesNotClearIncompleteCache) {
  MockFS FS;
  const Path Fresh = testPath("root/fresh.cpp");
  const Path Incomplete = testPath("root/incomplete.cpp");
  const Path FirstTarget = testPath("root/first.cpp");
  const Path SecondTarget = testPath("root/second.cpp");
  FS.Files[Fresh] = "int fresh;\n";
  FS.Files[Incomplete] = "int incomplete;\n";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  std::mutex Mu;
  std::condition_variable CV;
  bool BlockNextIncomplete = false;
  bool IncompleteEntered = false;
  bool ReleaseIncomplete = false;
  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 1;
  Opts.ContextProvider = [&](PathRef File) {
    std::unique_lock<std::mutex> Lock(Mu);
    if (File == Incomplete && BlockNextIncomplete) {
      BlockNextIncomplete = false;
      IncompleteEntered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return ReleaseIncomplete; });
    }
    return Context::current().clone();
  };
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  auto command = [](PathRef File) {
    tooling::CompileCommand Cmd;
    Cmd.Directory = testPath("root");
    Cmd.Filename = File.str();
    Cmd.CommandLine = {"clang++", File.str()};
    return Cmd;
  };
  CDB.setCompileCommand(Fresh, command(Fresh));
  CDB.setCompileCommand(Incomplete, command(Incomplete));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  {
    std::lock_guard<std::mutex> Lock(Mu);
    BlockNextIncomplete = true;
  }
  tooling::CompileCommand Changed = command(Incomplete);
  Changed.CommandLine.push_back("-DCHANGED");
  CDB.setCompileCommand(Incomplete, std::move(Changed));
  {
    std::unique_lock<std::mutex> Lock(Mu);
    ASSERT_TRUE(CV.wait_for(Lock, std::chrono::seconds(10),
                            [&] { return IncompleteEntered; }));
  }
  Storage.erase(Incomplete);
  ASSERT_TRUE(Storage.contains(Fresh));
  const std::string FreshShard = Storage.lookup(Fresh);

  ASSERT_THAT_ERROR(
      Idx.filesRenamed({{Fresh, FirstTarget}, {Fresh, SecondTarget}}),
      llvm::FailedWithMessage(testing::HasSubstr("overlapping file renames")));
  EXPECT_FALSE(MSS.WasCleared);
  EXPECT_EQ(Storage.lookup(Fresh), FreshShard);

  {
    std::lock_guard<std::mutex> Lock(Mu);
    ReleaseIncomplete = true;
  }
  CV.notify_all();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
}

} // namespace clangd
} // namespace clang
