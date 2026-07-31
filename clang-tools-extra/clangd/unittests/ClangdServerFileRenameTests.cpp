//===-- ClangdServerFileRenameTests.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "CompileCommands.h"
#include "GlobalCompilationDatabase.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "support/Path.h"
#include "support/Threading.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <utility>

namespace clang {
namespace clangd {
namespace {

using ::testing::SizeIs;

llvm::Expected<WorkspaceEdit>
runPrepareFileRename(ClangdServer &Server,
                     llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  std::optional<llvm::Expected<WorkspaceEdit>> Result;
  Notification Done;
  Server.prepareFileRename(Renames, [&](llvm::Expected<WorkspaceEdit> Value) {
    Result.emplace(std::move(Value));
    Done.notify();
  });
  Done.wait();
  assert(Result && "file rename callback was not called");
  return std::move(*Result);
}

tooling::CompileCommand commandFor(PathRef File) {
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = File.str();
  Command.CommandLine = {"clang++", "-c", File.str()};
  return Command;
}

class CountingWorkspaceFS : public MockFS {
  class View : public llvm::vfs::ProxyFileSystem {
  public:
    View(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base,
         std::atomic<unsigned> &RootReads)
        : ProxyFileSystem(std::move(Base)), RootReads(RootReads) {}

    llvm::vfs::directory_iterator dir_begin(const llvm::Twine &Directory,
                                            std::error_code &EC) override {
      if (Directory.str() == testRoot())
        ++RootReads;
      return ProxyFileSystem::dir_begin(Directory, EC);
    }

  private:
    std::atomic<unsigned> &RootReads;
  };

public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    return new View(MockFS::viewImpl(), RootDirectoryReads);
  }

  mutable std::atomic<unsigned> RootDirectoryReads{0};
};

TEST(ClangdServerFileRename, ReturnsVersionedDocumentChanges) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  runAddDocument(Server, Main, FS.Files.lookup(Main), "7");

  auto Result = runPrepareFileRename(Server, {{{Old}, {New}}});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_TRUE(Result->documentChanges);
  ASSERT_THAT(*Result->documentChanges, SizeIs(1));
  const TextDocumentEdit &Edit = Result->documentChanges->front();
  EXPECT_EQ(Edit.textDocument.uri.file(), Main);
  EXPECT_EQ(Edit.textDocument.version, 7);
  ASSERT_THAT(Edit.edits, SizeIs(1));
  EXPECT_EQ(Edit.edits.front().newText, "\"new.h\"");
  EXPECT_FALSE(Result->changes);
}

TEST(ClangdServerFileRename, RejectsMetadataOnlyDirectoryBeforeCommands) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old");
  const Path New = testPath("new");
  FS.Files[Main] = "int main_value;\n";
  FS.Files[testPath("old/.git/object")] = "metadata";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  auto Command = commandFor(Main);
  Command.CommandLine.insert(Command.CommandLine.end() - 1, {"-I", Old});
  CDB.setCompileCommand(Main, Command);
  runAddDocument(Server, Main, FS.Files.lookup(Main), "1");

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr("unscanned metadata root")));
}

TEST(ClangdServerFileRename, RejectsRenamesIntersectingPrunedMetadata) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Metadata = testPath("nested/.git");
  FS.Files[Main] = "int main_value;\n";
  FS.Files[testPath("nested/.git/generated.h")] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  for (const auto &Rename :
       {std::pair<Path, Path>{Metadata, testPath("metadata")},
        std::pair<Path, Path>{testPath("nested"), testPath("renamed")}}) {
    EXPECT_THAT_EXPECTED(
        runPrepareFileRename(Server, {{Rename.first, Rename.second}}),
        llvm::FailedWithMessage(
            testing::HasSubstr("unscanned metadata root")));
  }
}

TEST(ClangdServerFileRename, ReusesWorkspaceInventoryAcrossPreparations) {
  CountingWorkspaceFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  ASSERT_THAT_EXPECTED(runPrepareFileRename(Server, {{{Old}, {New}}}),
                       llvm::Succeeded());
  const unsigned AfterFirst = FS.RootDirectoryReads;
  ASSERT_GT(AfterFirst, 0U);
  ASSERT_THAT_EXPECTED(runPrepareFileRename(Server, {{{Old}, {New}}}),
                       llvm::Succeeded());
  EXPECT_EQ(FS.RootDirectoryReads, AfterFirst);
}

TEST(ClangdServerFileRename, UpdatesIncludeInsideOrdinaryHeaderGuard) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Guarded = testPath("guarded.h");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"guarded.h\"\n";
  FS.Files[Guarded] = R"cpp(
#ifndef GUARDED_H
#define GUARDED_H
#include "old.h"
#endif
)cpp";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  auto Result = runPrepareFileRename(Server, {{{Old}, {New}}});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_TRUE(Result->documentChanges);
  ASSERT_THAT(*Result->documentChanges, SizeIs(1));
  EXPECT_EQ(Result->documentChanges->front().textDocument.uri.file(), Guarded);
  EXPECT_EQ(Result->documentChanges->front().edits.front().newText,
            "\"new.h\"");
}

TEST(ClangdServerFileRename, UpdatesIncludeInsideInactiveHeaderGuard) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Guarded = testPath("guarded.h");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"guarded.h\"\n";
  FS.Files[Guarded] = R"cpp(
#ifndef GUARDED_H
#define GUARDED_H
#include "old.h"
#endif
)cpp";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  auto Command = commandFor(Main);
  Command.CommandLine.insert(Command.CommandLine.begin() + 1, "-DGUARDED_H");
  CDB.setCompileCommand(Main, std::move(Command));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  auto Result = runPrepareFileRename(Server, {{{Old}, {New}}});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_TRUE(Result->documentChanges);
  ASSERT_THAT(*Result->documentChanges, SizeIs(1));
  EXPECT_EQ(Result->documentChanges->front().textDocument.uri.file(), Guarded);
  EXPECT_EQ(Result->documentChanges->front().edits.front().newText,
            "\"new.h\"");
}

TEST(ClangdServerFileRename, UpdatesInactiveConditionalInclude) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = R"cpp(
#if ENABLED
#include "old.h"
#endif
int main() { return 0; }
)cpp";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  auto Command = commandFor(Main);
  Command.CommandLine.insert(Command.CommandLine.begin() + 1, "-DENABLED=0");
  CDB.setCompileCommand(Main, std::move(Command));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  auto Result = runPrepareFileRename(Server, {{{Old}, {New}}});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_TRUE(Result->documentChanges);
  ASSERT_THAT(*Result->documentChanges, SizeIs(1));
  EXPECT_EQ(Result->documentChanges->front().textDocument.uri.file(), Main);
  ASSERT_THAT(Result->documentChanges->front().edits, SizeIs(1));
  EXPECT_EQ(Result->documentChanges->front().edits.front().newText,
            "\"new.h\"");
}

TEST(ClangdServerFileRename, RejectsDestinationOnlyIncludeShadow) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("first/unrelated.h");
  const Path New = testPath("first/current.h");
  FS.Files[Main] = "#include <current.h>\n";
  FS.Files[Old] = "";
  FS.Files[testPath("second/current.h")] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  auto Command = commandFor(Main);
  Command.CommandLine.insert(Command.CommandLine.end() - 1,
                             {"-I", testPath("first"), "-I",
                              testPath("second")});
  CDB.setCompileCommand(Main, std::move(Command));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr("instead of")));
}

TEST(ClangdServerFileRename, RejectsOrphanWorkspaceSource) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Orphan = testPath("orphan.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Orphan] = "int orphan;\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(runPrepareFileRename(Server, {{{Old}, {New}}}),
                       llvm::FailedWithMessage(testing::HasSubstr(
                           "not represented in the background include graph")));
}

TEST(ClangdServerFileRename, RejectsOrphanWorkspaceIncludeFragment) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Fragment = testPath("generated.inc");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "int main_value;\n";
  FS.Files[Fragment] = "#include \"old.h\"\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(runPrepareFileRename(Server, {{{Old}, {New}}}),
                       llvm::FailedWithMessage(testing::HasSubstr(
                           "not represented in the background include graph")));
}

TEST(ClangdServerFileRename, RejectsStaleRepresentedClosedFile) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Other = testPath("other.h");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"other.h\"\n";
  FS.Files[Other] = "";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  FS.Files[Other] = "#include \"old.h\"\n";
  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr("include graph is stale")));
}

TEST(ClangdServerFileRename, RejectsAffectedExternalIncluder) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  const Path External = "/outside/vendor.h";
  FS.Files[Main] = "#include \"/outside/vendor.h\"\n";
  FS.Files[External] = "#include \"/clangd-test/old.h\"\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(
          testing::HasSubstr("includer outside the workspace")));
}

TEST(ClangdServerFileRename, RejectsStaleExternalGraphNode) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  const Path External = "/outside/vendor.h";
  FS.Files[Main] = "#include \"/outside/vendor.h\"\n";
  FS.Files[External] = "";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  FS.Files[External] = "#include \"/clangd-test/old.h\"\n";
  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr("frozen digest")));
}

TEST(ClangdServerFileRename, RejectsUnresolvedConditionalIncludes) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Other = testPath("other.h");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"old.h\"\n#include \"other.h\"\n";
  FS.Files[Other] = R"cpp(
#if ENABLE_EXTRA
#include EXTRA_HEADER
#include_next "unrelated.h"
#endif
)cpp";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  auto Command = commandFor(Main);
  Command.CommandLine.insert(Command.CommandLine.begin() + 1,
                             "-DENABLE_EXTRA=0");
  CDB.setCompileCommand(Main, std::move(Command));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  auto Result = runPrepareFileRename(Server, {{{Old}, {New}}});
  EXPECT_THAT_EXPECTED(
      Result,
      llvm::FailedWithMessage(testing::AnyOf(
          testing::HasSubstr("macro-generated conditional include"),
          testing::HasSubstr("conditional #include_next"))));
}

TEST(ClangdServerFileRename, RejectsUneditableAssemblyDependency) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = R"cpp(
#include "old.h"
asm(".incbin \"payload.bin\"");
)cpp";
  FS.Files[Old] = "";
  FS.Files[testPath("payload.bin")] = "payload";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(
          testing::HasSubstr("inline assembly dependency")));
}

TEST(ClangdServerFileRename, RejectsIncludeAliasContext) {
  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Header = testPath("header.h");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = R"cpp(
#pragma include_alias("alias.h", "old.h")
#include "header.h"
)cpp";
  FS.Files[Header] = "#include \"alias.h\"\n";
  FS.Files[Old] = "";
  FS.Files[testPath("alias.h")] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr("include_alias state")));
}

TEST(ClangdServerFileRename, RejectsCrossDirectoryConfigurationScope) {
  MockFS FS;
  const Path Main = testPath("a/main.cpp");
  const Path New = testPath("b/main.cpp");
  FS.Files[Main] = "int value;\n";
  FS.Files[testPath("b/.keep")] = "";
  MockCompilationDatabase Base(testRoot());
  OverlayCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Main}, {New}}}),
      llvm::FailedWithMessage(testing::HasSubstr(
          "configuration or compilation-database directories")));
}

TEST(ClangdServerFileRename, RejectsFilenameSpecificDestinationConfig) {
  MockFS FS;
  const Path Old = testPath("old.cpp");
  const Path New = testPath("new.cpp");
  FS.Files[Old] = "int value;\n";
  auto ContextForFile = [](PathRef File) {
    Config C;
    std::string Define = File.ends_with("new.cpp") ? "-DDESTINATION"
                                                    : "-DSOURCE";
    C.CompileFlags.Edits.push_back(
        [Define = std::move(Define)](std::vector<std::string> &Argv) {
          Argv = tooling::getInsertArgumentAdjuster(Define.c_str())(Argv, "");
        });
    return Context::current().derive(Config::Key, std::move(C));
  };
  MockCompilationDatabase Base(testRoot());
  auto Mangler = CommandMangler::forTests();
  OverlayCDB CDB(
      &Base, {},
      [Mangler = std::move(Mangler)](tooling::CompileCommand &Command,
                                     PathRef File) {
        Mangler(Command, File);
      });
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  Opts.ContextProvider = ContextForFile;
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Old, commandFor(Old));
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  EXPECT_THAT_EXPECTED(
      runPrepareFileRename(Server, {{{Old}, {New}}}),
      llvm::FailedWithMessage(
          testing::HasSubstr("compilation command or configuration changes")));
}

TEST(ClangdServerFileRename, RejectsDraftChangeDuringPreparation) {
  class BlockingCDB : public OverlayCDB {
  public:
    explicit BlockingCDB(const GlobalCompilationDatabase *Base)
        : OverlayCDB(Base) {}

    bool blockUntilIdle(Deadline) const override {
      std::unique_lock<std::mutex> Lock(Mu);
      if (!ShouldBlock)
        return true;
      Entered = true;
      CV.notify_all();
      CV.wait(Lock, [&] { return Released; });
      return true;
    }

    void startBlocking() {
      std::lock_guard<std::mutex> Lock(Mu);
      ShouldBlock = true;
    }

    void waitUntilBlocked() const {
      std::unique_lock<std::mutex> Lock(Mu);
      ASSERT_TRUE(
          CV.wait_for(Lock, std::chrono::seconds(10), [&] { return Entered; }));
    }

    void release() const {
      {
        std::lock_guard<std::mutex> Lock(Mu);
        Released = true;
      }
      CV.notify_all();
    }

  private:
    mutable std::mutex Mu;
    mutable std::condition_variable CV;
    mutable bool ShouldBlock = false;
    mutable bool Entered = false;
    mutable bool Released = false;
  };

  MockFS FS;
  const Path Main = testPath("main.cpp");
  const Path Old = testPath("old.h");
  const Path New = testPath("new.h");
  FS.Files[Main] = "#include \"old.h\"\n";
  FS.Files[Old] = "";
  MockCompilationDatabase Base(testRoot());
  BlockingCDB CDB(&Base);
  auto Opts = ClangdServer::optsForTest();
  Opts.BackgroundIndex = true;
  Opts.WorkspaceRoot = testRoot();
  ClangdServer Server(CDB, FS, Opts);
  CDB.setCompileCommand(Main, commandFor(Main));
  runAddDocument(Server, Main, FS.Files.lookup(Main), "1");

  CDB.startBlocking();
  std::optional<llvm::Expected<WorkspaceEdit>> Result;
  Notification Done;
  Server.prepareFileRename({{{Old}, {New}}},
                           [&](llvm::Expected<WorkspaceEdit> Value) {
                             Result.emplace(std::move(Value));
                             Done.notify();
                           });
  CDB.waitUntilBlocked();
  Server.addDocument(Main, "#include \"old.h\"\n// changed\n", "2");
  CDB.release();
  Done.wait();
  ASSERT_TRUE(Result);
  ASSERT_FALSE(static_cast<bool>(*Result));
  bool WasContentModified = false;
  llvm::handleAllErrors(
      Result->takeError(),
      [&](const LSPError &E) {
        WasContentModified = E.Code == ErrorCode::ContentModified;
      },
      [&](const llvm::ErrorInfoBase &E) {
        ADD_FAILURE() << "unexpected error: " << E.message();
      });
  EXPECT_TRUE(WasContentModified);
}

} // namespace
} // namespace clangd
} // namespace clang
