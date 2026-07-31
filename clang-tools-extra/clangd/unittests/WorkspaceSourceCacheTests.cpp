//===-- WorkspaceSourceCacheTests.cpp ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FileRename.h"
#include "TestFS.h"
#include "WorkspaceSourceCache.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;

class FailingReadFileSystem : public llvm::vfs::ProxyFileSystem {
public:
  FailingReadFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base,
                        Path FailPath)
      : ProxyFileSystem(std::move(Base)), FailPath(std::move(FailPath)) {}

  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForRead(const llvm::Twine &Path) override {
    if (Path.str() == FailPath)
      return std::make_error_code(std::errc::io_error);
    return ProxyFileSystem::openFileForRead(Path);
  }

private:
  Path FailPath;
};

class BufferSizeRecordingFile : public llvm::vfs::File {
public:
  BufferSizeRecordingFile(std::unique_ptr<llvm::vfs::File> Base,
                          std::vector<int64_t> &RequestedSizes)
      : Base(std::move(Base)), RequestedSizes(RequestedSizes) {}

  llvm::ErrorOr<llvm::vfs::Status> status() override {
    return Base->status();
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  getBuffer(const llvm::Twine &Name, int64_t FileSize,
            bool RequiresNullTerminator, bool IsVolatile) override {
    RequestedSizes.push_back(FileSize);
    return Base->getBuffer(Name, FileSize, RequiresNullTerminator, IsVolatile);
  }

  std::error_code close() override { return Base->close(); }

private:
  std::unique_ptr<llvm::vfs::File> Base;
  std::vector<int64_t> &RequestedSizes;
};

class BufferSizeRecordingFileSystem : public llvm::vfs::ProxyFileSystem {
public:
  explicit BufferSizeRecordingFileSystem(
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base)
      : ProxyFileSystem(std::move(Base)) {}

  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForRead(const llvm::Twine &Path) override {
    ++TextOpens;
    return open(Path);
  }

  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForReadBinary(const llvm::Twine &Path) override {
    ++BinaryOpens;
    return open(Path);
  }

  unsigned TextOpens = 0;
  unsigned BinaryOpens = 0;
  std::vector<int64_t> RequestedSizes;

private:
  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  open(const llvm::Twine &Path) {
    auto File = ProxyFileSystem::openFileForRead(Path);
    if (!File)
      return File.getError();
    return std::make_unique<BufferSizeRecordingFile>(std::move(*File),
                                                      RequestedSizes);
  }
};

TEST(FileRename, EnumeratesWorkspaceSourcesAndHeaders) {
  MockFS FS;
  FS.Files[testPath("main.cpp")] = "";
  FS.Files[testPath("include/header.h")] = "";
  FS.Files[testPath("include/fragment.inc")] = "#include \"old.h\"\n";
  FS.Files[testPath("generated")] = "#include <old.h>\n";
  FS.Files[testPath("include/spliced.inc")] = "#inc\\\nlude \"old.h\"\n";
  FS.Files[testPath("spliced-generated")] = "#imp\\\nort <old.h>\n";
  FS.Files[testPath("README.md")] = "";
  FS.Files[testPath("artifact.bin")] =
      std::string("binary\0incidental include bytes", 31);
  auto VFS = FS.view(std::nullopt);

  auto Files = workspaceSourceFiles(testRoot(), *VFS);
  ASSERT_THAT_EXPECTED(Files, llvm::Succeeded());
  EXPECT_THAT(
      *Files,
      testing::UnorderedElementsAre(
          testing::AllOf(
              testing::Field(&WorkspaceSourceFile::File, testPath("main.cpp")),
              testing::Field(&WorkspaceSourceFile::IsHeader, false)),
          testing::AllOf(testing::Field(&WorkspaceSourceFile::File,
                                        testPath("include/header.h")),
                         testing::Field(&WorkspaceSourceFile::IsHeader, true)),
          testing::AllOf(testing::Field(&WorkspaceSourceFile::File,
                                        testPath("include/fragment.inc")),
                         testing::Field(&WorkspaceSourceFile::IsHeader, true)),
          testing::AllOf(
              testing::Field(&WorkspaceSourceFile::File, testPath("generated")),
              testing::Field(&WorkspaceSourceFile::IsHeader, true)),
          testing::AllOf(testing::Field(&WorkspaceSourceFile::File,
                                        testPath("include/spliced.inc")),
                         testing::Field(&WorkspaceSourceFile::IsHeader, true)),
          testing::AllOf(
              testing::Field(&WorkspaceSourceFile::File,
                             testPath("spliced-generated")),
              testing::Field(&WorkspaceSourceFile::IsHeader, true))));
}

TEST(FileRename, EnforcesWorkspaceInventoryFileSizeLimit) {
  WorkspaceSourceLimits Limits;
  Limits.MaxTextFileBytes = 16;
  Limits.MaxBytesRead = 1024;
  Limits.MaxDirectiveScanBytes = 1024;
  for (bool KnownSource : {false, true}) {
    const Path File = testPath(KnownSource ? "boundary.cpp" : "boundary.dat");
    MockFS FS;
    FS.Files[File] = std::string(Limits.MaxTextFileBytes, 'x');
    auto VFS = FS.view(std::nullopt);
    WorkspaceSourceCache Cache(testRoot(), Limits);
    auto Snapshot = Cache.snapshot(*VFS);
    ASSERT_THAT_EXPECTED(Snapshot, llvm::Succeeded());
    if (KnownSource)
      EXPECT_THAT(Snapshot->Sources, ElementsAre(testing::Field(
                                         &WorkspaceSourceFile::File, File)));
    else
      EXPECT_TRUE(Snapshot->Sources.empty());
  }

  for (bool KnownSource : {false, true}) {
    const Path File = testPath(KnownSource ? "oversized.cpp" : "oversized.dat");
    MockFS FS;
    FS.Files[File] = std::string(Limits.MaxTextFileBytes + 1, 'x');
    auto VFS = FS.view(std::nullopt);
    WorkspaceSourceCache Cache(testRoot(), Limits);
    EXPECT_THAT_EXPECTED(
        Cache.snapshot(*VFS),
        llvm::FailedWithMessage(testing::AllOf(
            HasSubstr(File), HasSubstr("file-rename inventory limit"))));
  }

  MockFS FS;
  const Path Binary = testPath("large.bin");
  FS.Files[Binary] = std::string(Limits.MaxTextFileBytes + 1, 'x');
  FS.Files[Binary][3] = '\0';
  auto VFS = FS.view(std::nullopt);
  WorkspaceSourceCache Cache(testRoot(), Limits);
  auto Snapshot = Cache.snapshot(*VFS);
  ASSERT_THAT_EXPECTED(Snapshot, llvm::Succeeded());
  EXPECT_EQ(Snapshot->Files.lookup(Binary).Classification,
            WorkspaceFileClassification::Binary);
}

TEST(FileRename, ProbesOversizedFilesWithoutReadingThemWhole) {
  constexpr uint64_t ProbeBytes = 64 * 1024;
  WorkspaceSourceLimits Limits;
  Limits.MaxTextFileBytes = ProbeBytes;
  const Path Binary = testPath("giant.cpp");
  MockFS FS;
  FS.Files[Binary] = std::string(4 * ProbeBytes, 'x');
  FS.Files[Binary][ProbeBytes - 1] = '\0';
  llvm::IntrusiveRefCntPtr<BufferSizeRecordingFileSystem> VFS =
      new BufferSizeRecordingFileSystem(FS.view(std::nullopt));

  WorkspaceSourceCache Cache(testRoot(), Limits);
  auto Snapshot = Cache.snapshot(*VFS);
  ASSERT_THAT_EXPECTED(Snapshot, llvm::Succeeded());
  EXPECT_EQ(Snapshot->Files.lookup(Binary).Classification,
            WorkspaceFileClassification::Binary);
  ASSERT_THAT(VFS->RequestedSizes, ElementsAre(ProbeBytes));
  EXPECT_EQ(VFS->BinaryOpens, 1U);
  EXPECT_EQ(VFS->TextOpens, 0U);
}

TEST(FileRename, EnforcesWorkspaceInventoryAggregateLimits) {
  MockFS FS;
  FS.Files[testPath("one.unknown")] = "abc";
  FS.Files[testPath("two.unknown")] = "def";

  for (unsigned LimitKind = 0; LimitKind != 4; ++LimitKind) {
    WorkspaceSourceLimits Limits;
    if (LimitKind == 0)
      Limits.MaxFiles = 1;
    else if (LimitKind == 1)
      Limits.MaxBytesRead = 5;
    else if (LimitKind == 2)
      Limits.MaxDirectiveScanBytes = 5;
    else
      Limits.MaxEntries = 2;
    WorkspaceSourceCache Cache(testRoot(), Limits);
    auto VFS = FS.view(std::nullopt);
    EXPECT_THAT_EXPECTED(
        Cache.snapshot(*VFS),
        llvm::FailedWithMessage(HasSubstr("workspace file-rename inventory")));
  }
}

TEST(FileRename, InvalidatedFileCannotEscapeWorkspaceThroughSymlink) {
  llvm::SmallString<256> Workspace;
  llvm::SmallString<256> Outside;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("clangd-inventory", Workspace));
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("clangd-outside", Outside));
  llvm::scope_exit Cleanup([&] {
    llvm::sys::fs::remove_directories(Workspace);
    llvm::sys::fs::remove_directories(Outside);
  });
  llvm::SmallString<256> Source(Workspace);
  llvm::sys::path::append(Source, "source.cpp");
  llvm::SmallString<256> External(Outside);
  llvm::sys::path::append(External, "external.cpp");
  for (PathRef File : {Source.str(), External.str()}) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(File, EC);
    ASSERT_FALSE(EC);
    Stream << "int value;";
  }

  WorkspaceSourceCache Cache(Workspace.str().str());
  auto FS = llvm::vfs::getRealFileSystem();
  ASSERT_THAT_EXPECTED(Cache.snapshot(*FS), llvm::Succeeded());
  llvm::SmallString<256> Alias(Workspace);
  llvm::sys::path::append(Alias, "alias.cpp");
  ASSERT_FALSE(llvm::sys::fs::create_symlink(External, Alias));
  Cache.invalidate(Alias);
  EXPECT_THAT_EXPECTED(
      Cache.snapshot(*FS),
      llvm::FailedWithMessage(HasSubstr("traverses outside the workspace")));
}

TEST(FileRename, IgnoresInvalidationsOutsideWorkspace) {
  MockFS FS;
  const Path Source = testPath("source.cpp");
  const Path Outside = "/outside/external.cpp";
  FS.Files[Source] = "int source;";
  FS.Files[Outside] = "int external;";
  Path UnnormalizedRoot = testRoot();
  UnnormalizedRoot += "/./nested/..";
  WorkspaceSourceCache Cache(UnnormalizedRoot);
  auto View = FS.view(std::nullopt);
  auto Initial = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Initial, llvm::Succeeded());

  Cache.invalidate(Outside);
  Cache.invalidate("/outside");
  View = FS.view(std::nullopt);
  auto Unchanged = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Unchanged, llvm::Succeeded());
  EXPECT_EQ(*Unchanged, *Initial);
}

TEST(FileRename, FailedTransactionalRefreshRetainsDirtyRetry) {
  MockFS FS;
  const Path Existing = testPath("existing.cpp");
  const Path Added = testPath("added.cpp");
  FS.Files[Existing] = "int existing;";
  WorkspaceSourceCache Cache(testRoot());
  auto InitialFS = FS.view(std::nullopt);
  auto Initial = Cache.snapshot(*InitialFS);
  ASSERT_THAT_EXPECTED(Initial, llvm::Succeeded());
  ASSERT_TRUE(Initial->Files.contains(Existing));

  FS.Files[Added] = "int added;";
  Cache.invalidate(testRoot());
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Failing =
      new FailingReadFileSystem(FS.view(std::nullopt), Added);
  EXPECT_THAT_EXPECTED(Cache.snapshot(*Failing), llvm::Failed());

  // The directory metadata is unchanged in MockFS. Retaining the failed
  // invalidation is therefore the only reason this retries the full scan.
  auto RepairedFS = FS.view(std::nullopt);
  auto Repaired = Cache.snapshot(*RepairedFS);
  ASSERT_THAT_EXPECTED(Repaired, llvm::Succeeded());
  EXPECT_TRUE(Repaired->Files.contains(Existing));
  EXPECT_TRUE(Repaired->Files.contains(Added));
}

TEST(FileRename, ReusesWorkspaceInventoryWithoutRecursiveRescan) {
  MockFS FS;
  FS.Files[testPath("main.cpp")] = "#include \"header.h\"\n";
  FS.Files[testPath("header.h")] = "";
  auto Base = FS.view(std::nullopt);
  llvm::IntrusiveRefCntPtr<llvm::vfs::TracingFileSystem> Tracing =
      new llvm::vfs::TracingFileSystem(std::move(Base));
  WorkspaceSourceCache Cache(testRoot());

  auto First = Cache.snapshot(*Tracing);
  ASSERT_THAT_EXPECTED(First, llvm::Succeeded());
  ASSERT_GT(Tracing->NumDirBeginCalls, 0U);
  const size_t DirectoryReads = Tracing->NumDirBeginCalls;
  const size_t ContentReads = Tracing->NumOpenFileForReadCalls;

  auto Second = Cache.snapshot(*Tracing);
  ASSERT_THAT_EXPECTED(Second, llvm::Succeeded());
  EXPECT_EQ(Tracing->NumDirBeginCalls, DirectoryReads);
  EXPECT_EQ(Tracing->NumOpenFileForReadCalls, ContentReads);
  EXPECT_EQ(Second->Generation, First->Generation);
  EXPECT_EQ(*Second, *First);
  EXPECT_EQ(Second->Digests.size(), First->Digests.size());
  for (const auto &Entry : First->Digests)
    EXPECT_EQ(Second->Digests.lookup(Entry.first()), Entry.getValue());
}

TEST(FileRename, InitializationConsumesPendingInvalidations) {
  MockFS FS;
  const Path Header = testPath("header.h");
  FS.Files[Header] = "#pragma once\n";
  WorkspaceSourceCache Cache(testRoot());

  Cache.invalidate(Header);
  auto View = FS.view(std::nullopt);
  auto First = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(First, llvm::Succeeded());
  auto Second = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Second, llvm::Succeeded());

  EXPECT_EQ(*Second, *First);
  EXPECT_EQ(Second->Generation, First->Generation);
}

TEST(FileRename, RefreshesInvalidatedSameMetadataAndNestedFiles) {
  MockFS FS;
  const Path Header = testPath("header.h");
  const Path Nested = testPath("generated/nested.inc");
  FS.Files[Header] = "#include \"a.h\"\n";
  FS.Timestamps[Header] = 1;
  WorkspaceSourceCache Cache(testRoot());
  auto View = FS.view(std::nullopt);
  auto First = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(First, llvm::Succeeded());
  FileDigest FirstDigest = First->Digests.lookup(Header);

  // Identity, timestamp, and size are unchanged. The watched-file
  // invalidation is what makes this replacement observable without rereading
  // every cached file.
  FS.Files[Header] = "#include \"b.h\"\n";
  Cache.invalidate(Header);
  View = FS.view(std::nullopt);
  auto Changed = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Changed, llvm::Succeeded());
  EXPECT_NE(Changed->Digests.lookup(Header), FirstDigest);

  FS.Files[Nested] = "#include \"b.h\"\n";
  Cache.invalidate(Nested);
  View = FS.view(std::nullopt);
  auto Created = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Created, llvm::Succeeded());
  EXPECT_TRUE(Created->Digests.contains(Nested));
  EXPECT_THAT(Created->Sources,
              Contains(testing::Field(&WorkspaceSourceFile::File, Nested)));

  FS.Files.erase(Nested);
  Cache.invalidate(Nested);
  View = FS.view(std::nullopt);
  auto Deleted = Cache.snapshot(*View);
  ASSERT_THAT_EXPECTED(Deleted, llvm::Succeeded());
  EXPECT_FALSE(Deleted->Digests.contains(Nested));
}

} // namespace
} // namespace clangd
} // namespace clang
