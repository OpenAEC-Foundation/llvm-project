#include "Annotations.h"
#include "CompileCommands.h"
#include "Config.h"
#include "Headers.h"
#include "RIFF.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "TestTU.h"
#include "index/Background.h"
#include "index/BackgroundRebuild.h"
#include "index/MemIndex.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>

using ::testing::_;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

namespace clang {
namespace clangd {

MATCHER_P(named, N, "") { return arg.Name == N; }
MATCHER_P(qName, N, "") { return (arg.Scope + arg.Name).str() == N; }
MATCHER(declared, "") {
  return !StringRef(arg.CanonicalDeclaration.FileURI).empty();
}
MATCHER(defined, "") { return !StringRef(arg.Definition.FileURI).empty(); }
MATCHER_P(fileURI, F, "") { return StringRef(arg.Location.FileURI) == F; }
::testing::Matcher<const RefSlab &>
refsAre(std::vector<::testing::Matcher<Ref>> Matchers) {
  return ElementsAre(::testing::Pair(_, UnorderedElementsAreArray(Matchers)));
}
// URI cannot be empty since it references keys in the IncludeGraph.
MATCHER(emptyIncludeNode, "") {
  return arg.Flags == IncludeGraphNode::SourceFlag::None && !arg.URI.empty() &&
         arg.Digest == FileDigest{{0}} && arg.DirectIncludes.empty();
}

MATCHER(hadErrors, "") {
  return arg.Flags & IncludeGraphNode::SourceFlag::HadErrors;
}

MATCHER_P(numReferences, N, "") { return arg.References == N; }

llvm::Expected<std::string> contextCacheWithState(llvm::StringRef Shard,
                                                  bool Malformed) {
  auto Parsed = riff::readFile(Shard);
  if (!Parsed)
    return Parsed.takeError();
  auto Context = llvm::find_if(Parsed->Chunks, [](riff::Chunk Chunk) {
    return Chunk.ID == riff::fourCC("ctxs");
  });
  if (Context == Parsed->Chunks.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "index shard has no context-source chunk");
  if (!Malformed) {
    Parsed->Chunks.erase(Context);
    return llvm::to_string(*Parsed);
  }
  if (Context->Data.size() < 4)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "context-source chunk is too small");
  std::string CorruptContext = Context->Data.str();
  CorruptContext[0] = 2;
  Context->Data = CorruptContext;
  return llvm::to_string(*Parsed);
}

class MemoryShardStorage : public BackgroundIndexStorage {
  mutable std::mutex StorageMu;
  llvm::StringMap<std::string> &Storage;
  size_t &CacheHits;

public:
  MemoryShardStorage(llvm::StringMap<std::string> &Storage, size_t &CacheHits)
      : Storage(Storage), CacheHits(CacheHits) {}
  llvm::Error storeShard(llvm::StringRef ShardIdentifier,
                         IndexFileOut Shard) const override {
    if (BeforeStore)
      BeforeStore(ShardIdentifier);
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    StoredPaths.insert(ShardIdentifier);
    ++StoreCounts[ShardIdentifier];
    Storage[ShardIdentifier] = llvm::to_string(Shard);
    return llvm::Error::success();
  }
  llvm::Error removeShard(llvm::StringRef ShardIdentifier) const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    Storage.erase(ShardIdentifier);
    return llvm::Error::success();
  }
  llvm::Error clear() const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    Storage.clear();
    WasCleared = true;
    return llvm::Error::success();
  }
  std::unique_ptr<IndexFileIn>
  loadShard(llvm::StringRef ShardIdentifier) const override {
    if (BeforeLoad)
      BeforeLoad(ShardIdentifier);
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    if (!Storage.contains(ShardIdentifier)) {
      return nullptr;
    }
    auto IndexFile =
        readIndexFile(Storage[ShardIdentifier], SymbolOrigin::Background);
    if (!IndexFile) {
      if (AllowLoadErrors)
        llvm::consumeError(IndexFile.takeError());
      else
        ADD_FAILURE() << "Error while reading " << ShardIdentifier << ':'
                      << IndexFile.takeError();
      return nullptr;
    }
    CacheHits++;
    return std::make_unique<IndexFileIn>(std::move(*IndexFile));
  }

  mutable llvm::StringSet<> AccessedPaths;
  mutable llvm::StringSet<> StoredPaths;
  mutable llvm::StringMap<unsigned> StoreCounts;
  std::function<void(PathRef)> BeforeStore;
  std::function<void(PathRef)> BeforeLoad;
  bool AllowLoadErrors = false;
  mutable bool WasCleared = false;
};

class CountingMockFS : public MockFS {
  class CountingView : public llvm::vfs::ProxyFileSystem {
  public:
    CountingView(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base,
                 const CountingMockFS &Owner)
        : ProxyFileSystem(std::move(Base)), Owner(Owner) {}

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
    openFileForRead(const llvm::Twine &Path) override {
      {
        std::lock_guard<std::mutex> Lock(Owner.ReadMu);
        ++Owner.Reads[Path.str()];
      }
      return ProxyFileSystem::openFileForRead(Path);
    }

  private:
    const CountingMockFS &Owner;
  };

public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    return new CountingView(MockFS::viewImpl(), *this);
  }

  void resetReads() const {
    std::lock_guard<std::mutex> Lock(ReadMu);
    Reads.clear();
  }

  size_t reads(PathRef File) const {
    std::lock_guard<std::mutex> Lock(ReadMu);
    return Reads.lookup(File);
  }

private:
  mutable std::mutex ReadMu;
  mutable llvm::StringMap<size_t> Reads;
};

class BackgroundIndexTest : public ::testing::Test {
protected:
  BackgroundIndexTest() { BackgroundQueue::preventThreadStarvationInTests(); }
};

TEST_F(BackgroundIndexTest, NoCrashOnErrorFile) {
  MockFS FS;
  FS.Files[testPath("root/A.cc")] = "error file";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "-DA=1", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
}

TEST_F(BackgroundIndexTest, Config) {
  MockFS FS;
  // Set up two identical TUs, foo and bar.
  // They define foo::one and bar::one.
  std::vector<tooling::CompileCommand> Cmds;
  for (std::string Name : {"foo", "bar", "baz"}) {
    std::string Filename = Name + ".cpp";
    std::string Header = Name + ".h";
    FS.Files[Filename] = "#include \"" + Header + "\"";
    FS.Files[Header] = "namespace " + Name + " { int one; }";
    tooling::CompileCommand Cmd;
    Cmd.Filename = Filename;
    Cmd.Directory = testRoot();
    Cmd.CommandLine = {"clang++", Filename};
    Cmds.push_back(std::move(Cmd));
  }
  // Context provider that installs a configuration mutating foo's command.
  // This causes it to define foo::two instead of foo::one.
  // It also disables indexing of baz entirely.
  BackgroundIndex::Options Opts;
  Opts.ContextProvider = [](PathRef P) {
    Config C;
    if (P.ends_with("foo.cpp"))
      C.CompileFlags.Edits.push_back([](std::vector<std::string> &Argv) {
        Argv = tooling::getInsertArgumentAdjuster("-Done=two")(Argv, "");
      });
    if (P.ends_with("baz.cpp"))
      C.Index.Background = Config::BackgroundPolicy::Skip;
    return Context::current().derive(Config::Key, std::move(C));
  };
  // Create the background index.
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  // We need the CommandMangler, because that applies the config we're testing.
  OverlayCDB CDB(/*Base=*/nullptr, /*FallbackFlags=*/{},
                 CommandMangler::forTests());

  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  // Index the two files.
  for (auto &Cmd : Cmds) {
    std::string FullPath = testPath(Cmd.Filename);
    CDB.setCompileCommand(FullPath, std::move(Cmd));
  }
  // Wait for both files to be indexed.
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(qName("foo"), qName("foo::two"),
                                   qName("bar"), qName("bar::one")));

  Idx.ensureIncludeGraph();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT_EXPECTED(
      Idx.includeGraphSnapshot(),
      llvm::FailedWithMessage(testing::HasSubstr("disabled by configuration")));
}

TEST_F(BackgroundIndexTest, IndexTwoFiles) {
  MockFS FS;
  // a.h yields different symbols when included by A.cc vs B.cc.
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      #if A
        class A_CC {};
      #else
        class B_CC{};
      #endif
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nstatic void g() { (void)common; }";
  FS.Files[testPath("root/B.cc")] =
      R"cpp(
      #define A 0
      #include "A.h"
      void f_b() {
        (void)common;
        (void)common;
        (void)common;
        (void)common;
      })cpp";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "-DA=1", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(AllOf(named("common"), numReferences(1U)),
                                   AllOf(named("A_CC"), numReferences(0U)),
                                   AllOf(named("g"), numReferences(1U)),
                                   AllOf(named("f_b"), declared(),
                                         Not(defined()), numReferences(0U))));

  Cmd.Filename = testPath("root/B.cc");
  Cmd.CommandLine = {"clang++", Cmd.Filename};
  CDB.setCompileCommand(testPath("root/B.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  // B_CC is dropped as we don't collect symbols from A.h in this compilation.
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(AllOf(named("common"), numReferences(5U)),
                                   AllOf(named("A_CC"), numReferences(0U)),
                                   AllOf(named("g"), numReferences(1U)),
                                   AllOf(named("f_b"), declared(), defined(),
                                         numReferences(1U))));

  auto Syms = runFuzzyFind(Idx, "common");
  EXPECT_THAT(Syms, UnorderedElementsAre(named("common")));
  auto Common = *Syms.begin();
  EXPECT_THAT(getRefs(Idx, Common.ID),
              refsAre({fileURI("unittest:///root/A.h"),
                       fileURI("unittest:///root/A.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc")}));
}

TEST_F(BackgroundIndexTest, ConstructorForwarding) {
  Annotations Header(R"cpp(
    namespace std {
    template <class T> T &&forward(T &t);
    template <class T, class... Args> T *make_unique(Args &&...args) {
      return new T(std::forward<Args>(args)...);
    }
    }
    struct Test {
      [[Test]](){}
    };
  )cpp");
  Annotations Main(R"cpp(
    #include "header.hpp"
    int main() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");

  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  FS.Files[testPath("root/header.hpp")] = Header.code();
  FS.Files[testPath("root/test.cpp")] = Main.code();

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/test.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/test.cpp")};
  CDB.setCompileCommand(testPath("root/test.cpp"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Syms = runFuzzyFind(Idx, "Test");
  auto Constructor =
      std::find_if(Syms.begin(), Syms.end(), [](const Symbol &S) {
        return S.SymInfo.Kind == index::SymbolKind::Constructor;
      });
  ASSERT_TRUE(Constructor != Syms.end());
  EXPECT_THAT(getRefs(Idx, Constructor->ID),
              refsAre({fileURI("unittest:///root/header.hpp"),
                       fileURI("unittest:///root/test.cpp")}));
}

TEST_F(BackgroundIndexTest, ConstructorForwardingMultiFile) {
  // If a forwarding function like `make_unique` is defined in a header its body
  // used to be skipped on the second encounter. This meant in practise we could
  // only find constructors indirectly called by these type of functions in the
  // first indexed file (and all files that were indexed at the same time,
  // before a flag to skip it was set).
  Annotations Header(R"cpp(
    namespace std {
    template <class T> T &&forward(T &t);
    template <class T, class... Args> T *make_unique(Args &&...args) {
      return new T(std::forward<Args>(args)...);
    }
    }
    struct Test {
      [[Test]](){}
    };
  )cpp");
  Annotations First(R"cpp(
    #include "header.hpp"
    int main() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");
  Annotations Second(R"cpp(
    #include "header.hpp"
    void test() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");

  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  FS.Files[testPath("root/header.hpp")] = Header.code();
  FS.Files[testPath("root/first.cpp")] = First.code();
  FS.Files[testPath("root/second.cpp")] = Second.code();

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/first.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/first.cpp")};
  CDB.setCompileCommand(testPath("root/first.cpp"), Cmd);

  // Make sure the first file is done indexing to make sure the flag for the
  // header is set.
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  Cmd.Filename = testPath("root/second.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/second.cpp")};
  CDB.setCompileCommand(testPath("root/second.cpp"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Syms = runFuzzyFind(Idx, "Test");
  auto Constructor =
      std::find_if(Syms.begin(), Syms.end(), [](const Symbol &S) {
        return S.SymInfo.Kind == index::SymbolKind::Constructor;
      });
  ASSERT_TRUE(Constructor != Syms.end());
  EXPECT_THAT(getRefs(Idx, Constructor->ID),
              refsAre({fileURI("unittest:///root/header.hpp"),
                       fileURI("unittest:///root/first.cpp"),
                       fileURI("unittest:///root/second.cpp")}));
}

TEST_F(BackgroundIndexTest, MainFileRefs) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void header_sym();
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nstatic void main_sym() { (void)header_sym; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(
      runFuzzyFind(Idx, ""),
      UnorderedElementsAre(AllOf(named("header_sym"), numReferences(1U)),
                           AllOf(named("main_sym"), numReferences(1U))));
}

TEST_F(BackgroundIndexTest, ShardStorageTest) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] = R"cpp(
      #include "A.h"
      void g() { (void)common; }
      class B_CC : public A_CC {};
      )cpp";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check nothing is loaded from Storage, but A.cc and A.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 0U);
  EXPECT_EQ(Storage.size(), 2U);

  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.
  EXPECT_EQ(Storage.size(), 2U);

  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(
      *ShardHeader->Symbols,
      UnorderedElementsAre(named("common"), named("A_CC"),
                           AllOf(named("f_b"), declared(), Not(defined()))));
  for (const auto &Ref : *ShardHeader->Refs)
    EXPECT_THAT(Ref.second,
                UnorderedElementsAre(fileURI("unittest:///root/A.h")));

  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_NE(ShardSource, nullptr);
  EXPECT_THAT(*ShardSource->Symbols,
              UnorderedElementsAre(named("g"), named("B_CC")));
  for (const auto &Ref : *ShardSource->Refs)
    EXPECT_THAT(Ref.second,
                UnorderedElementsAre(fileURI("unittest:///root/A.cc")));

  // The BaseOf relationship between A_CC and B_CC is stored in both the file
  // containing the definition of the subject (A_CC) and the file containing
  // the definition of the object (B_CC).
  SymbolID A = findSymbol(*ShardHeader->Symbols, "A_CC").ID;
  SymbolID B = findSymbol(*ShardSource->Symbols, "B_CC").ID;
  EXPECT_THAT(*ShardHeader->Relations,
              UnorderedElementsAre(Relation{A, RelationKind::BaseOf, B}));
  EXPECT_THAT(*ShardSource->Relations,
              UnorderedElementsAre(Relation{A, RelationKind::BaseOf, B}));
}

TEST_F(BackgroundIndexTest, DirectIncludesTest) {
  MockFS FS;
  FS.Files[testPath("root/B.h")] = "";
  FS.Files[testPath("root/A.h")] = R"cpp(
      #include "B.h"
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_TRUE(ShardSource->Sources);
  EXPECT_EQ(ShardSource->Sources->size(), 2U); // A.cc, A.h
  EXPECT_THAT(
      ShardSource->Sources->lookup("unittest:///root/A.cc").DirectIncludes,
      UnorderedElementsAre("unittest:///root/A.h"));
  EXPECT_NE(ShardSource->Sources->lookup("unittest:///root/A.cc").Digest,
            FileDigest{{0}});
  EXPECT_THAT(ShardSource->Sources->lookup("unittest:///root/A.h"),
              emptyIncludeNode());

  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_TRUE(ShardHeader->Sources);
  EXPECT_EQ(ShardHeader->Sources->size(), 2U); // A.h, B.h
  EXPECT_THAT(
      ShardHeader->Sources->lookup("unittest:///root/A.h").DirectIncludes,
      UnorderedElementsAre("unittest:///root/B.h"));
  EXPECT_NE(ShardHeader->Sources->lookup("unittest:///root/A.h").Digest,
            FileDigest{{0}});
  EXPECT_THAT(ShardHeader->Sources->lookup("unittest:///root/B.h"),
              emptyIncludeNode());
}

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
  FS.Files[Main] = R"cpp(
#if ENABLE_HEADER
#include "conditional.h"
#endif
)cpp";
  FS.Files[testPath("root/conditional.h")] = "";

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
          testing::Field(&BackgroundIndex::IndexedFile::HasConditionalIncludes,
                         true))));
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
          testing::Field(&BackgroundIndex::IndexedFile::HasConditionalIncludes,
                         true))));
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
    EXPECT_THAT(*Rewritten->Symbols, Contains(named("value")));
    ASSERT_THAT_EXPECTED(Idx.includeGraphSnapshot(), llvm::Succeeded());
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
  EXPECT_THAT_EXPECTED(
      Idx.includeGraphSnapshot(),
      llvm::FailedWithMessage(testing::HasSubstr("no translation unit")));

  Idx.ensureIncludeGraph();
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  ASSERT_THAT_EXPECTED(Idx.includeGraphSnapshot(), llvm::Succeeded());
}

TEST_F(BackgroundIndexTest, ShardStorageLoad) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check nothing is loaded from Storage, but A.cc and A.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  // Change header.
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      class A_CCnew {};
      )cpp";
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.

  // Check if the new symbol has arrived.
  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols, Contains(named("A_CCnew")));

  // Change source.
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }\nvoid f_b() {}";
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.

  // Check if the new symbol has arrived.
  ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols, Contains(named("A_CCnew")));
  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_NE(ShardSource, nullptr);
  EXPECT_THAT(*ShardSource->Symbols,
              Contains(AllOf(named("f_b"), declared(), defined())));
}

TEST_F(BackgroundIndexTest, ShardStorageEmptyFile) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/B.h")] = R"cpp(
      #include "A.h"
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"B.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check that A.cc, A.h and B.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("root/A.cc"), testPath("root/A.h"),
                                   testPath("root/B.h")));
  auto ShardHeader = MSS.loadShard(testPath("root/B.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_TRUE(ShardHeader->Symbols->empty());

  // Check that A.cc, A.h and B.h has been loaded.
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 3U);

  // Update B.h to contain some symbols.
  FS.Files[testPath("root/B.h")] = R"cpp(
      #include "A.h"
      void new_func();
      )cpp";
  // Check that B.h has been stored with new contents.
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 3U);
  ShardHeader = MSS.loadShard(testPath("root/B.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols,
              Contains(AllOf(named("new_func"), declared(), Not(defined()))));
}

TEST_F(BackgroundIndexTest, NoDotsInAbsPath) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  tooling::CompileCommand Cmd;
  FS.Files[testPath("root/A.cc")] = "";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("root/build");
  Cmd.CommandLine = {"clang++", "../A.cc"};
  CDB.setCompileCommand(testPath("root/build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  FS.Files[testPath("root/B.cc")] = "";
  Cmd.Filename = "./B.cc";
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "./B.cc"};
  CDB.setCompileCommand(testPath("root/./B.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  for (llvm::StringRef AbsPath : MSS.AccessedPaths.keys()) {
    EXPECT_FALSE(AbsPath.contains("./")) << AbsPath;
    EXPECT_FALSE(AbsPath.contains("../")) << AbsPath;
  }
}

TEST_F(BackgroundIndexTest, UncompilableFiles) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  FS.Files[testPath("A.h")] = "void foo();";
  FS.Files[testPath("B.h")] = "#include \"C.h\"\nasdf;";
  FS.Files[testPath("C.h")] = "";
  FS.Files[testPath("A.cc")] = R"cpp(
  #include "A.h"
  #include "B.h"
  #include "not_found_header.h"

  void foo() {}
  )cpp";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc"};
  CDB.setCompileCommand(testPath("build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("A.cc"), testPath("A.h"),
                                   testPath("B.h"), testPath("C.h")));

  {
    auto Shard = MSS.loadShard(testPath("A.cc"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("foo")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///A.cc", "unittest:///A.h",
                                     "unittest:///B.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///A.cc"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("A.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("foo")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///A.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///A.h"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("B.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("asdf")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///B.h", "unittest:///C.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///B.h"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("C.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre());
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///C.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///C.h"), hadErrors());
  }
}

TEST_F(BackgroundIndexTest, CmdLineHash) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  FS.Files[testPath("A.cc")] = "#include \"A.h\"";
  FS.Files[testPath("A.h")] = "";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc", "-fsyntax-only"};
  CDB.setCompileCommand(testPath("build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("A.cc"), testPath("A.h")));
  // Make sure we only store the Cmd for main file.
  EXPECT_FALSE(MSS.loadShard(testPath("A.h"))->Cmd);

  tooling::CompileCommand CmdStored = *MSS.loadShard(testPath("A.cc"))->Cmd;
  EXPECT_EQ(CmdStored.CommandLine, Cmd.CommandLine);
  EXPECT_EQ(CmdStored.Directory, Cmd.Directory);
}

TEST_F(BackgroundIndexTest, Reindex) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  // Index a file.
  FS.Files[testPath("A.cc")] = "int theOldFunction();";
  tooling::CompileCommand Cmd;
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc", "-fsyntax-only"};
  CDB.setCompileCommand(testPath("A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  // Verify the result is indexed and stored.
  EXPECT_EQ(1u, runFuzzyFind(Idx, "theOldFunction").size());
  EXPECT_EQ(0u, runFuzzyFind(Idx, "theNewFunction").size());
  std::string OldShard = Storage.lookup(testPath("A.cc"));
  EXPECT_NE("", OldShard);

  // Change the content and command, and notify to reindex it.
  Cmd.CommandLine.push_back("-DFOO");
  FS.Files[testPath("A.cc")] = "int theNewFunction();";
  CDB.setCompileCommand(testPath("A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  // Currently, we will never index the same main file again.
  EXPECT_EQ(1u, runFuzzyFind(Idx, "theOldFunction").size());
  EXPECT_EQ(0u, runFuzzyFind(Idx, "theNewFunction").size());
  EXPECT_EQ(OldShard, Storage.lookup(testPath("A.cc")));
}

class BackgroundIndexRebuilderTest : public testing::Test {
protected:
  BackgroundIndexRebuilderTest()
      : Source(IndexContents::All, /*SupportContainedRefs=*/true),
        Target(std::make_unique<MemIndex>()),
        Rebuilder(&Target, &Source, /*Threads=*/10) {
    // Prepare FileSymbols with TestSymbol in it, for checkRebuild.
    TestSymbol.ID = SymbolID("foo");
  }

  // Perform Action and determine whether it rebuilt the index or not.
  bool checkRebuild(std::function<void()> Action) {
    // Update name so we can tell if the index updates.
    VersionStorage.push_back("Sym" + std::to_string(++VersionCounter));
    TestSymbol.Name = VersionStorage.back();
    SymbolSlab::Builder SB;
    SB.insert(TestSymbol);
    Source.update("", std::make_unique<SymbolSlab>(std::move(SB).build()),
                  nullptr, nullptr, false);
    // Now maybe update the index.
    Action();
    // Now query the index to get the name count.
    std::string ReadName;
    LookupRequest Req;
    Req.IDs.insert(TestSymbol.ID);
    Target.lookup(Req,
                  [&](const Symbol &S) { ReadName = std::string(S.Name); });
    // The index was rebuild if the name is up to date.
    return ReadName == VersionStorage.back();
  }

  Symbol TestSymbol;
  FileSymbols Source;
  SwapIndex Target;
  BackgroundIndexRebuilder Rebuilder;

  unsigned VersionCounter = 0;
  std::deque<std::string> VersionStorage;
};

TEST_F(BackgroundIndexRebuilderTest, IndexingTUs) {
  for (unsigned I = 0; I < Rebuilder.TUsBeforeFirstBuild - 1; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  for (unsigned I = 0; I < Rebuilder.TUsBeforeRebuild - 1; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.indexedTU(); }));
}

TEST_F(BackgroundIndexRebuilderTest, LoadingShards) {
  Rebuilder.startLoading();
  Rebuilder.loadedShard(10);
  Rebuilder.loadedShard(20);
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // No rebuild for no shards.
  Rebuilder.startLoading();
  EXPECT_FALSE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // Loads can overlap.
  Rebuilder.startLoading();
  Rebuilder.loadedShard(1);
  Rebuilder.startLoading();
  Rebuilder.loadedShard(1);
  EXPECT_FALSE(checkRebuild([&] { Rebuilder.doneLoading(); }));
  Rebuilder.loadedShard(1);
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // No rebuilding for indexed files while loading.
  Rebuilder.startLoading();
  for (unsigned I = 0; I < 3 * Rebuilder.TUsBeforeRebuild; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  // But they get indexed when we're done, even if no shards were loaded.
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));
}

TEST(BackgroundQueueTest, Priority) {
  // Create high and low priority tasks.
  // Once a bunch of high priority tasks have run, the queue is stopped.
  // So the low priority tasks should never run.
  BackgroundQueue Q;
  std::atomic<unsigned> HiRan(0), LoRan(0);
  BackgroundQueue::Task Lo([&] { ++LoRan; });
  BackgroundQueue::Task Hi([&] {
    if (++HiRan >= 10)
      Q.stop();
  });
  Hi.QueuePri = 100;

  // Enqueuing the low-priority ones first shouldn't make them run first.
  Q.append(std::vector<BackgroundQueue::Task>(30, Lo));
  for (unsigned I = 0; I < 30; ++I)
    Q.push(Hi);

  AsyncTaskRunner ThreadPool;
  for (unsigned I = 0; I < 5; ++I)
    ThreadPool.runAsync("worker", [&] { Q.work(); });
  // We should test enqueue with active workers, but it's hard to avoid races.
  // Just make sure we don't crash.
  Q.push(Lo);
  Q.append(std::vector<BackgroundQueue::Task>(2, Hi));

  // After finishing, check the tasks that ran.
  ThreadPool.wait();
  EXPECT_GE(HiRan, 10u);
  EXPECT_EQ(LoRan, 0u);
}

TEST(BackgroundQueueTest, Boost) {
  std::string Sequence;

  BackgroundQueue::Task A([&] { Sequence.push_back('A'); });
  A.Tag = "A";
  A.QueuePri = 1;

  BackgroundQueue::Task B([&] { Sequence.push_back('B'); });
  B.QueuePri = 2;
  B.Tag = "B";

  {
    BackgroundQueue Q;
    Q.append({A, B});
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("BA", Sequence) << "priority order";
  }
  Sequence.clear();
  {
    BackgroundQueue Q;
    Q.boost("A", 3);
    Q.append({A, B});
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("AB", Sequence) << "A was boosted before enqueueing";
  }
  Sequence.clear();
  {
    BackgroundQueue Q;
    Q.append({A, B});
    Q.boost("A", 3);
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("AB", Sequence) << "A was boosted after enqueueing";
  }
}

TEST(BackgroundQueueTest, Duplicates) {
  std::string Sequence;
  BackgroundQueue::Task A([&] { Sequence.push_back('A'); });
  A.QueuePri = 100;
  A.Key = 1;
  BackgroundQueue::Task B([&] { Sequence.push_back('B'); });
  // B has no key, and is not subject to duplicate detection.
  B.QueuePri = 50;

  BackgroundQueue Q;
  Q.append({A, B, A, B}); // One A is dropped, the other is high priority.
  Q.work(/*OnIdle=*/[&] {
    // The first time we go idle, we enqueue the same task again.
    if (!llvm::is_contained(Sequence, ' ')) {
      Sequence.push_back(' ');
      Q.append({A, B, A, B}); // Both As are dropped.
    } else {
      Q.stop();
    }
  });

  // This could reasonably be "ABB BBA", if we had good *re*indexing support.
  EXPECT_EQ("ABB BB", Sequence);
}

TEST(BackgroundQueueTest, Progress) {
  using testing::AnyOf;
  BackgroundQueue::Stats S;
  BackgroundQueue Q([&](BackgroundQueue::Stats New) {
    // Verify values are sane.
    // Items are enqueued one at a time (at least in this test).
    EXPECT_THAT(New.Enqueued, AnyOf(S.Enqueued, S.Enqueued + 1));
    // Items are completed one at a time.
    EXPECT_THAT(New.Completed, AnyOf(S.Completed, S.Completed + 1));
    // Items are started or completed one at a time.
    EXPECT_THAT(New.Active, AnyOf(S.Active - 1, S.Active, S.Active + 1));
    // Idle point only advances in time.
    EXPECT_GE(New.LastIdle, S.LastIdle);
    // Idle point is a task that has been completed in the past.
    EXPECT_LE(New.LastIdle, New.Completed);
    // LastIdle is now only if we're really idle.
    EXPECT_EQ(New.LastIdle == New.Enqueued,
              New.Completed == New.Enqueued && New.Active == 0u);
    S = New;
  });

  // Two types of tasks: a ping task enqueues a pong task.
  // This avoids all enqueues followed by all completions (boring!)
  std::atomic<int> PingCount(0), PongCount(0);
  BackgroundQueue::Task Pong([&] { ++PongCount; });
  BackgroundQueue::Task Ping([&] {
    ++PingCount;
    Q.push(Pong);
  });

  for (int I = 0; I < 1000; ++I)
    Q.push(Ping);
  // Spin up some workers and stop while idle.
  AsyncTaskRunner ThreadPool;
  for (unsigned I = 0; I < 5; ++I)
    ThreadPool.runAsync("worker", [&] { Q.work([&] { Q.stop(); }); });
  ThreadPool.wait();

  // Everything's done, check final stats.
  // Assertions above ensure we got from 0 to 2000 in a reasonable way.
  EXPECT_EQ(PingCount.load(), 1000);
  EXPECT_EQ(PongCount.load(), 1000);
  EXPECT_EQ(S.Active, 0u);
  EXPECT_EQ(S.Enqueued, 2000u);
  EXPECT_EQ(S.Completed, 2000u);
  EXPECT_EQ(S.LastIdle, 2000u);
}

TEST(BackgroundIndex, Profile) {
  MockFS FS;
  MockCompilationDatabase CDB;
  BackgroundIndex Idx(FS, CDB, [](llvm::StringRef) { return nullptr; },
                      /*Opts=*/{});

  llvm::BumpPtrAllocator Alloc;
  MemoryTree MT(&Alloc);
  Idx.profile(MT);
  ASSERT_THAT(MT.children(),
              UnorderedElementsAre(Pair("slabs", _), Pair("index", _)));
}

} // namespace clangd
} // namespace clang
